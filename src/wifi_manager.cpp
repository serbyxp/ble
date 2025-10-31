#include "wifi_manager.h"

#include <Preferences.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <esp_system.h> // esp_random()

// ---------- Config ----------
static constexpr uint16_t CAPTIVE_DNS_PORT = 53;
static constexpr uint32_t CONNECTION_TIMEOUT_MS = 20000; // 20s
static constexpr uint32_t BACKOFF_BASE_MS = 30000;       // 30s
static constexpr uint32_t BACKOFF_MAX_MS = 300000;       // 5 min
static constexpr uint32_t ROLLBACK_WAIT_MS = 5000;       // 5s fallback assoc grace

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);
static const IPAddress AP_GW(192, 168, 4, 1);
static const char *AP_SSID = "ble-hid";

// ---------- State ----------
enum class State
{
  AccessPointOnly,
  Connecting,
  Connected
};
static State g_state = State::AccessPointOnly;

static String g_ssid;
static String g_password;

static bool g_hasCredentials = false;
static bool g_accessPointActive = false;
static bool g_dnsServerRunning = false;

static uint32_t g_connectStart = 0;

// Backoff for AP-only retry loop
static uint32_t g_backoffMs = BACKOFF_BASE_MS;
static uint32_t g_nextReconnectAt = 0;

// Event-driven failure flags (set by onEvent, consumed by Connecting handler)
static volatile wifi_err_reason_t g_lastDiscReason = WIFI_REASON_UNSPECIFIED;
static volatile bool g_evtAuthFail = false;
static volatile bool g_evtNoApFound = false;

static Preferences g_prefs;
static DNSServer g_dns;

// Track last good (connected) association for rollback
static String g_prevSsid;
static String g_prevPassword;
static uint8_t g_prevBssid[6] = {0};
static int32_t g_prevChannel = 0;
static bool g_prevValid = false;

// Pending credentials submitted via portal; only commit on success
static String g_pendingSsid;
static String g_pendingPassword;
static bool g_hasPending = false;

// ---------- Internals ----------
static void startDnsServer();
static void stopDnsServer();
static void startAccessPoint(); // always pure AP (idle portal)
static void stopAccessPoint();
static void beginStationConnection();
static void handleAccessPointOnly();
static void handleConnecting();
static void handleConnected();
static void snapshotCurrentAssociation();
static bool attemptStaSwitchWithRollback(const String &newSsid, const String &newPass, uint32_t timeoutMs);
static uint32_t jittered(uint32_t baseMs, uint32_t pct); // ±pct%
static void scheduleBackoff(bool increase);
static void clearConnectEvents();

// Event hook
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

// ---------- Helpers ----------
static uint32_t jittered(uint32_t baseMs, uint32_t pct)
{
  // pct in [0..50] typically; produce base ± (pct%)
  uint32_t span = (baseMs * pct) / 100;
  uint32_t r = esp_random() % (2 * span + 1); // [0..2*span]
  int32_t delta = (int32_t)r - (int32_t)span; // [-span..+span]
  int64_t v = (int64_t)baseMs + delta;
  if (v < 0)
    v = 0;
  return (uint32_t)v;
}

static void scheduleBackoff(bool increase)
{
  if (increase)
  {
    if (g_backoffMs < BACKOFF_MAX_MS)
    {
      uint64_t doubled = (uint64_t)g_backoffMs * 2ULL;
      g_backoffMs = (doubled > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : (uint32_t)doubled;
    }
  }
  else
  {
    g_backoffMs = BACKOFF_BASE_MS;
  }
  g_nextReconnectAt = millis() + jittered(g_backoffMs, 10);
}

static void clearConnectEvents()
{
  g_evtAuthFail = false;
  g_evtNoApFound = false;
  g_lastDiscReason = WIFI_REASON_UNSPECIFIED;
}

// ---------- DNS ----------
static void startDnsServer()
{
  if (g_dnsServerRunning)
    return;
  g_dns.start(CAPTIVE_DNS_PORT, "*", AP_IP);
  g_dnsServerRunning = true;
}

static void stopDnsServer()
{
  if (!g_dnsServerRunning)
    return;
  g_dns.stop();
  g_dnsServerRunning = false;
}

// ---------- AP lifecycle ----------
static void startAccessPoint()
{
  // Idle portal runs as pure AP; AP+STA is used only during active portal connect attempts.
  wifi_mode_t targetMode = WIFI_AP;
  if (WiFi.getMode() != targetMode)
  {
    WiFi.mode(targetMode);
  }

  if (!g_accessPointActive)
  {
    WiFi.softAPConfig(AP_IP, AP_GW, AP_NETMASK);
    WiFi.softAP(AP_SSID);
    g_accessPointActive = true;
    startDnsServer();
    Serial.println(F("[WiFi] AP started (pure AP) with captive DNS"));
  }
}

static void stopAccessPoint()
{
  if (!g_accessPointActive)
    return;
  stopDnsServer();
  WiFi.softAPdisconnect(true);
  g_accessPointActive = false;
  Serial.println(F("[WiFi] AP stopped (captive DNS stopped)"));
}

// ---------- Association snapshot & provisional switch ----------
static void snapshotCurrentAssociation()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    g_prevValid = false;
    return;
  }
  wifi_ap_record_t apInfo{};
  if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK)
  {
    g_prevSsid = WiFi.SSID();
    g_prevPassword = g_password; // last committed password
    memcpy(g_prevBssid, apInfo.bssid, 6);
    g_prevChannel = apInfo.primary;
    g_prevValid = true;
  }
  else
  {
    g_prevValid = false;
  }
}

static bool attemptStaSwitchWithRollback(const String &newSsid, const String &newPass, uint32_t timeoutMs)
{
  if (WiFi.getMode() != WIFI_STA)
    WiFi.mode(WIFI_STA);
  if (WiFi.status() != WL_CONNECTED)
    return false;

  snapshotCurrentAssociation();

  Serial.printf("[WiFi] Provisional switch: '%s' -> '%s'\n",
                g_prevValid ? g_prevSsid.c_str() : "(unknown)",
                newSsid.c_str());

  // Attempt new SSID (STA-only). If you know channel/BSSID from a scan, prefer the 5-arg begin().
  clearConnectEvents();
  WiFi.begin(newSsid.c_str(), newPass.c_str());

  uint32_t start = millis();
  while (millis() - start < timeoutMs)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.printf("[WiFi] Provisional switch SUCCESS: now on '%s'\n", WiFi.SSID().c_str());
      return true;
    }
    if (g_evtAuthFail || g_evtNoApFound)
      break; // early abort on known-fatal reasons
    delay(50);
  }

  // Timeout or fatal reason — rollback to previous association if we had one.
  Serial.printf("[WiFi] Provisional switch FAILED (%d): rolling back\n", (int)g_lastDiscReason);
  if (g_prevValid)
  {
    WiFi.begin(g_prevSsid.c_str(), g_prevPassword.c_str(), g_prevChannel, g_prevBssid, true);
    uint32_t rbStart = millis();
    while (millis() - rbStart < ROLLBACK_WAIT_MS)
    {
      if (WiFi.status() == WL_CONNECTED)
      {
        Serial.printf("[WiFi] Rollback SUCCESS: restored '%s'\n", WiFi.SSID().c_str());
        return false; // switch failed; we restored
      }
      delay(50);
    }
    Serial.println("[WiFi] Rollback did not reconnect within 5s (stack will keep retrying).");
  }
  else
  {
    Serial.println("[WiFi] No prior association snapshot; cannot fast-rollback.");
  }
  return false;
}

// ---------- Connect attempt ----------
static void beginStationConnection()
{
  // If AP is active (portal up), attempt in AP+STA so the portal stays reachable.
  // Otherwise, attempt in pure STA (boot case with creds / STA reconnects).
  wifi_mode_t targetMode = g_accessPointActive ? WIFI_AP_STA : WIFI_STA;
  if (WiFi.getMode() != targetMode)
  {
    WiFi.mode(targetMode);
  }

  Serial.printf("[WiFi] Connecting to SSID '%s' (%s)\n",
                g_ssid.c_str(), g_accessPointActive ? "AP+STA" : "STA");

  clearConnectEvents();
  WiFi.begin(g_ssid.c_str(), g_password.c_str());
  g_connectStart = millis();
  g_state = State::Connecting;
}

// ---------- Event hook ----------
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
  {
    g_lastDiscReason = static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);
    // Mark terminal/fatal categories we want to short-circuit on
    if (g_lastDiscReason == WIFI_REASON_AUTH_FAIL ||
        g_lastDiscReason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        g_lastDiscReason == WIFI_REASON_NO_AP_FOUND)
    {
      if (g_lastDiscReason == WIFI_REASON_NO_AP_FOUND)
        g_evtNoApFound = true;
      else
        g_evtAuthFail = true;
    }
    break;
  }
  default:
    break;
  }
}

// ---------- Public API ----------
void wifiManagerInitialize()
{
  // Memory + policy
  btStop(); // free Classic BT heap on ESP32
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setAutoConnect(false);

  // Event registration
  WiFi.onEvent(onWiFiEvent);

  // Load creds
  g_prefs.begin("wifi", false);
  g_ssid = g_prefs.getString("ssid", "");
  g_password = g_prefs.getString("pass", "");
  g_ssid.trim();
  g_password.trim();
  g_hasCredentials = (!g_ssid.isEmpty());

  if (g_hasCredentials)
  {
    // Boot path with creds: try STA only; do NOT start AP yet.
    Serial.println(F("[WiFi] Creds present: attempting STA-only"));
    if (WiFi.getMode() != WIFI_STA)
      WiFi.mode(WIFI_STA);
    beginStationConnection(); // stays in STA because AP is not active
  }
  else
  {
    // No creds: AP-only captive portal
    Serial.println(F("[WiFi] No creds: starting AP-only portal"));
    startAccessPoint();
    g_state = State::AccessPointOnly;
  }
}

void wifiManagerLoop()
{
  // State machine tick
  switch (g_state)
  {
  case State::AccessPointOnly:
    handleAccessPointOnly();
    break;
  case State::Connecting:
    handleConnecting();
    break;
  case State::Connected:
    handleConnected();
    break;
  }
  // Pump DNS if running
  if (g_dnsServerRunning)
    g_dns.processNextRequest();
}

bool wifiManagerHasCredentials() { return g_hasCredentials; }
bool wifiManagerIsAccessPointActive() { return g_accessPointActive; }
bool wifiManagerIsConnecting() { return g_state == State::Connecting; }
bool wifiManagerIsConnected() { return WiFi.status() == WL_CONNECTED; }

String wifiManagerConnectedSSID() { return (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String(); }
IPAddress wifiManagerLocalIp() { return (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : IPAddress(); }
IPAddress wifiManagerApIp() { return g_accessPointActive ? WiFi.softAPIP() : IPAddress(); }

WifiManagerStatus wifiManagerGetStatus()
{
  WifiManagerStatus status;
  status.hasCredentials = g_hasCredentials;
  status.connecting = g_state == State::Connecting;
  status.connected = WiFi.status() == WL_CONNECTED;
  status.accessPointActive = g_accessPointActive;
  status.connectedSsid = wifiManagerConnectedSSID();
  status.stationIp = wifiManagerLocalIp();
  if (status.accessPointActive)
  {
    status.accessPointIp = WiFi.softAPIP();
  }
  return status;
}

const char *wifiManagerAccessPointSsid()
{
  return AP_SSID;
}

bool wifiManagerSetCredentials(const String &ssid, const String &password)
{
  String s = ssid;
  String p = password;
  s.trim();
  p.trim();

  if (s.isEmpty())
  {
    Serial.println(F("[WiFi] Rejecting credentials update: SSID is empty"));
    return false;
  }

  // Case A: Portal flow (AP active) → attempt in AP+STA, commit on success
  if (g_accessPointActive)
  {
    Serial.println(F("[WiFi] Creds submitted via portal: AP+STA attempt, commit on success"));
    g_pendingSsid = s;
    g_pendingPassword = p;
    g_hasPending = true;

    // Do NOT write to NVS yet; run a connecting attempt with AP up
    if (WiFi.getMode() != WIFI_AP_STA)
      WiFi.mode(WIFI_AP_STA);
    clearConnectEvents();
    WiFi.begin(s.c_str(), p.c_str());
    g_connectStart = millis();
    g_state = State::Connecting; // on success, we'll commit and stop AP
    return true;
  }

  // Case B: Already STA-connected → provisional switch with rollback; commit on success
  if (WiFi.status() == WL_CONNECTED && WiFi.getMode() == WIFI_STA)
  {
    Serial.println(F("[WiFi] Provisional STA switch (no AP): commit only if success"));
    const bool switched = attemptStaSwitchWithRollback(s, p, CONNECTION_TIMEOUT_MS);
    if (switched)
    {
      const size_t storedSsid = g_prefs.putString("ssid", s);
      const size_t storedPass = g_prefs.putString("pass", p);
      if (storedSsid == 0 || storedPass == 0)
      {
        Serial.println(F("[WiFi] Failed to persist credentials after switch"));
        return false;
      }
      g_ssid = s;
      g_password = p;
      g_hasCredentials = true;
      Serial.println(F("[WiFi] New credentials committed after successful association"));
      g_state = State::Connected;
      scheduleBackoff(false); // reset backoff
    }
    else
    {
      Serial.println(F("[WiFi] Switch failed; kept previous association and credentials"));
      // keep current state; caller will observe connection status separately
    }
    return true;
  }

  // Case C: Neither AP nor connected STA (e.g., idle) → STA-first; AP on failure
  Serial.println(F("[WiFi] Setting creds in idle state: STA attempt, AP on failure"));
  const size_t storedSsid = g_prefs.putString("ssid", s);
  const size_t storedPass = g_prefs.putString("pass", p);
  if (storedSsid == 0 || storedPass == 0)
  {
    Serial.println(F("[WiFi] Failed to persist credentials in idle state"));
    return false;
  }
  g_ssid = s;
  g_password = p;
  g_hasCredentials = true;
  WiFi.disconnect(true);
  if (WiFi.getMode() != WIFI_STA)
    WiFi.mode(WIFI_STA);
  beginStationConnection();
  return true;
}

bool wifiManagerForgetCredentials()
{
  bool ok = true;
  if (g_prefs.isKey("ssid") && !g_prefs.remove("ssid"))
    ok = false;
  if (g_prefs.isKey("pass") && !g_prefs.remove("pass"))
    ok = false;

  g_pendingSsid = "";
  g_pendingPassword = "";
  g_hasPending = false;
  g_ssid = "";
  g_password = "";
  g_hasCredentials = false;

  Serial.println(F("[WiFi] Credentials forgotten: going AP-only"));
  WiFi.disconnect(true /*wifiOff*/);
  startAccessPoint();
  g_state = State::AccessPointOnly;
  scheduleBackoff(false); // reset backoff
  return ok;
}

void wifiManagerEnsureAccessPoint()
{
  // Keep the portal up unless we're actively connected (then it’s explicitly off).
  if (g_state != State::Connected && !g_accessPointActive)
    startAccessPoint();
}

// ---------- State handlers ----------
static void handleAccessPointOnly()
{
  // Ensure AP is alive in this state.
  if (!g_accessPointActive)
    startAccessPoint();

  if (!g_hasCredentials)
    return;

  uint32_t now = millis();
  if (g_nextReconnectAt == 0)
  {
    scheduleBackoff(false); // initialize backoff schedule
  }
  if (now >= g_nextReconnectAt)
  {
    // Attempt with AP+STA so portal remains reachable.
    beginStationConnection();
  }
}

static void handleConnecting()
{
  wl_status_t st = WiFi.status();

  // Early aborts from event reasons (fatal categories)
  if (g_evtAuthFail || g_evtNoApFound)
  {
    Serial.printf("[WiFi] Early abort due to event reason: %d\n", (int)g_lastDiscReason);
    // Portal flow: keep/return to AP-only; discard pending
    if (g_hasPending)
    {
      g_hasPending = false;
      if (!g_accessPointActive)
        startAccessPoint();
      g_state = State::AccessPointOnly;
      scheduleBackoff(true);
      Serial.println(F("[WiFi] Pending credentials discarded (event abort); portal remains active"));
    }
    else
    {
      // Non-portal attempt: ensure AP available for recovery
      if (!g_accessPointActive)
        startAccessPoint();
      g_state = State::AccessPointOnly;
      scheduleBackoff(true);
    }
    return;
  }

  if (st == WL_CONNECTED)
  {
    Serial.printf("[WiFi] Connected: %s  IP=%s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    // If this came from portal (pending creds), commit now
    if (g_hasPending)
    {
      g_prefs.putString("ssid", g_pendingSsid);
      g_prefs.putString("pass", g_pendingPassword);
      g_ssid = g_pendingSsid;
      g_password = g_pendingPassword;
      g_hasCredentials = true;
      g_hasPending = false;
      Serial.println(F("[WiFi] Pending credentials committed (portal flow)"));
    }
    // Success: drop portal (if any) and go STA-only
    if (g_accessPointActive)
      stopAccessPoint();
    if (WiFi.getMode() != WIFI_STA)
      WiFi.mode(WIFI_STA);
    g_state = State::Connected;
    scheduleBackoff(false); // reset backoff on success
    return;
  }

  uint32_t elapsed = millis() - g_connectStart;
  if (elapsed > CONNECTION_TIMEOUT_MS)
  {
    Serial.println(F("[WiFi] Connection timed out"));
    if (g_hasPending)
    {
      // Portal flow timeout → keep/return to AP-only; discard pending
      g_hasPending = false;
      if (!g_accessPointActive)
        startAccessPoint();
      g_state = State::AccessPointOnly;
      scheduleBackoff(true);
      Serial.println(F("[WiFi] Pending credentials discarded (timeout); portal remains active"));
    }
    else
    {
      // Non-portal attempt timeout: ensure AP is available for recovery
      if (!g_accessPointActive)
        startAccessPoint();
      g_state = State::AccessPointOnly;
      scheduleBackoff(true);
    }
  }
}

static void handleConnected()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    // If portal is somehow still up, ensure we’re pure STA.
    if (g_accessPointActive)
      stopAccessPoint();
    if (WiFi.getMode() != WIFI_STA)
      WiFi.mode(WIFI_STA);
    return;
  }

  // Link lost while in Connected state: keep STA-only; try soft reconnect first.
  Serial.println(F("[WiFi] Station connection lost"));
  if (!WiFi.reconnect())
  {
    WiFi.disconnect(true /*wifiOff*/);
    if (WiFi.getMode() != WIFI_STA)
      WiFi.mode(WIFI_STA);
    // Re-attempt using stored creds (STA-only)
    if (g_hasCredentials)
    {
      beginStationConnection();
    }
    else
    {
      // No creds left? fall back to AP-only
      startAccessPoint();
      g_state = State::AccessPointOnly;
      scheduleBackoff(false);
    }
  }
}

// ---------- Scan ----------
std::vector<WifiScanResult> wifiManagerScanNetworks()
{
  // Save current mode
  wifi_mode_t prior = WiFi.getMode();

  // Ensure scanning works regardless of AP-only; use AP+STA if portal is running.
  if (g_accessPointActive)
  {
    if (prior != WIFI_AP_STA)
      WiFi.mode(WIFI_AP_STA);
  }
  else
  {
    if (prior != WIFI_STA)
      WiFi.mode(WIFI_STA);
  }

  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  std::vector<WifiScanResult> out;
  out.reserve((n > 0) ? n : 0);

  for (int i = 0; i < n; ++i)
  {
    WifiScanResult r;
    r.ssid = WiFi.SSID(i);
    r.hidden = r.ssid.length() == 0;
    r.rssi = WiFi.RSSI(i);
    r.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    out.push_back(std::move(r));
  }

  // Restore prior mode (and thus AP/portal state)
  if (WiFi.getMode() != prior)
    WiFi.mode(prior);
  WiFi.scanDelete();
  return out;
}
