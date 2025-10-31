#include "wifi_manager.h"

#include <Preferences.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <esp_system.h>

// ---------- Config ----------
static constexpr uint16_t CAPTIVE_DNS_PORT = 53;
static constexpr uint32_t CONNECTION_TIMEOUT_MS = 20000;
static constexpr uint32_t BACKOFF_BASE_MS = 30000;
static constexpr uint32_t BACKOFF_MAX_MS = 300000;
static constexpr uint32_t ROLLBACK_WAIT_MS = 5000;

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);
static const IPAddress AP_GW(192, 168, 4, 1);

static const char *AP_SSID = "ble-hid";
static const char *AP_PASSWORD = "uhid1234";

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

static uint32_t g_backoffMs = BACKOFF_BASE_MS;
static uint32_t g_nextReconnectAt = 0;

static volatile wifi_err_reason_t g_lastDiscReason = WIFI_REASON_UNSPECIFIED;
static volatile bool g_evtAuthFail = false;
static volatile bool g_evtNoApFound = false;

static Preferences g_prefs;
static DNSServer g_dns;

static String g_prevSsid;
static String g_prevPassword;
static uint8_t g_prevBssid[6] = {0};
static int32_t g_prevChannel = 0;
static bool g_prevValid = false;

static String g_pendingSsid;
static String g_pendingPassword;
static bool g_hasPending = false;

// ---------- Internals ----------
static void startDnsServer();
static void stopDnsServer();
static void startAccessPoint();
static void stopAccessPoint();
static void beginStationConnection();
static void handleAccessPointOnly();
static void handleConnecting();
static void handleConnected();
static void snapshotCurrentAssociation();
static bool attemptStaSwitchWithRollback(const String &newSsid, const String &newPass, uint32_t timeoutMs);
static uint32_t jittered(uint32_t baseMs, uint32_t pct);
static void scheduleBackoff(bool increase);
static void clearConnectEvents();
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

// ---------- Helpers ----------
static uint32_t jittered(uint32_t baseMs, uint32_t pct)
{
  uint32_t span = (baseMs * pct) / 100;
  uint32_t r = esp_random() % (2 * span + 1);
  int32_t delta = (int32_t)r - (int32_t)span;
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

  Serial.println(F("[WiFi] Starting DNS server..."));
  if (!g_dns.start(CAPTIVE_DNS_PORT, "*", AP_IP))
  {
    Serial.println(F("[WiFi] DNS server failed to start"));
    return;
  }
  g_dnsServerRunning = true;
  Serial.println(F("[WiFi] DNS server started"));
}

static void stopDnsServer()
{
  if (!g_dnsServerRunning)
    return;
  g_dns.stop();
  g_dnsServerRunning = false;
  Serial.println(F("[WiFi] DNS server stopped"));
}

// ---------- AP lifecycle ----------
static void startAccessPoint()
{
  if (g_accessPointActive)
  {
    Serial.println(F("[WiFi] AP already active"));
    return;
  }

  Serial.println(F("[WiFi] Starting AP - Step 1: Shutdown"));
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.println(F("[WiFi] Starting AP - Step 2: Set mode"));
  WiFi.mode(WIFI_AP);

  // IMPORTANT: Configure IP BEFORE softAP()
  Serial.println(F("[WiFi] Starting AP - Step 3: Configure IP"));
  if (!WiFi.softAPConfig(AP_IP, AP_GW, AP_NETMASK))
  {
    Serial.println(F("[WiFi] ERROR: softAPConfig() returned false"));
    return;
  }

  // Start AP with fixed channel to avoid auto-channel instability
  const int channel = 6; // consider 1/6/11 depending on your RF environment
  const int ssid_hidden = 0;
  const int max_conn = 4;

  Serial.println(F("[WiFi] Starting AP - Step 4: softAP()"));
  Serial.printf("[WiFi]   SSID: %s, Password: %s, Channel: %d\n", AP_SSID, AP_PASSWORD, channel);
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, channel, ssid_hidden, max_conn);
  if (!apStarted)
  {
    Serial.println(F("[WiFi] ERROR: softAP() returned false"));
    return;
  }

  g_accessPointActive = true;

  Serial.println(F("[WiFi] Starting AP - Step 5: Start DNS"));
  startDnsServer();

  Serial.println(F("[WiFi] ===== AP STARTED ====="));
  Serial.printf("[WiFi] SSID: %s\n", AP_SSID);
  Serial.printf("[WiFi] Password: %s\n", AP_PASSWORD);
  Serial.printf("[WiFi] IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("[WiFi] MAC: %s\n", WiFi.softAPmacAddress().c_str());
}

static void stopAccessPoint()
{
  if (!g_accessPointActive)
    return;

  Serial.println(F("[WiFi] Stopping AP"));
  stopDnsServer();
  WiFi.softAPdisconnect(true);
  g_accessPointActive = false;
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
    g_prevPassword = g_password;
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

  clearConnectEvents();
  WiFi.begin(newSsid.c_str(), newPass.c_str());

  uint32_t start = millis();
  while (millis() - start < timeoutMs)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.printf("[WiFi] Switch SUCCESS: '%s'\n", WiFi.SSID().c_str());
      return true;
    }
    if (g_evtAuthFail || g_evtNoApFound)
      break;
    delay(50);
  }

  Serial.printf("[WiFi] Switch FAILED (%d), rolling back\n", (int)g_lastDiscReason);
  if (g_prevValid)
  {
    WiFi.begin(g_prevSsid.c_str(), g_prevPassword.c_str(), g_prevChannel, g_prevBssid, true);
    uint32_t rbStart = millis();
    while (millis() - rbStart < ROLLBACK_WAIT_MS)
    {
      if (WiFi.status() == WL_CONNECTED)
      {
        Serial.printf("[WiFi] Rollback SUCCESS: '%s'\n", WiFi.SSID().c_str());
        return false;
      }
      delay(50);
    }
    Serial.println(F("[WiFi] Rollback timeout"));
  }
  return false;
}

// ---------- Connect attempt ----------
static void beginStationConnection()
{
  wifi_mode_t targetMode = g_accessPointActive ? WIFI_AP_STA : WIFI_STA;

  if (WiFi.getMode() != targetMode)
  {
    WiFi.mode(targetMode);
  }

  Serial.printf("[WiFi] Connecting to '%s' (%s)\n",
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
  case ARDUINO_EVENT_WIFI_AP_START:
    Serial.println(F("[WiFi] Event: AP_START"));
    break;
  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
  {
    uint8_t *mac = info.wifi_ap_staconnected.mac;
    Serial.printf("[WiFi] Event: Client CONNECTED MAC:%02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    break;
  }
  case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
  {
    uint8_t *mac = info.wifi_ap_stadisconnected.mac;
    Serial.printf("[WiFi] Event: Client DISCONNECTED MAC:%02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    break;
  }
  case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
  {
    uint8_t *mac = info.wifi_ap_probereqrecved.mac;
    Serial.printf("[WiFi] Probe from MAC:%02X:%02X:%02X:%02X:%02X:%02X RSSI:%d\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  info.wifi_ap_probereqrecved.rssi);
    break;
  }
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
  {
    g_lastDiscReason = static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);

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
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.printf("[WiFi] Event: GOT_IP %s\n", WiFi.localIP().toString().c_str());
    break;
  default:
    break;
  }
}

// ---------- Public API ----------
void wifiManagerInitialize()
{
  Serial.println(F("[WiFi] === Initializing WiFi Manager ==="));

  // Basic setup
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setAutoConnect(false);

  // Coexistence for BLE
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

  // Events
  WiFi.onEvent(onWiFiEvent);

  // Load creds
  Serial.println(F("[WiFi] Loading credentials..."));
  g_prefs.begin("wifi", false);
  g_ssid = g_prefs.getString("ssid", "");
  g_password = g_prefs.getString("pass", "");
  g_ssid.trim();
  g_password.trim();
  g_hasCredentials = (!g_ssid.isEmpty());

  if (g_hasCredentials)
  {
    Serial.printf("[WiFi] Found creds for: '%s'\n", g_ssid.c_str());
    WiFi.mode(WIFI_STA);
    beginStationConnection();
  }
  else
  {
    Serial.println(F("[WiFi] No credentials - starting AP"));
    startAccessPoint();
    g_state = State::AccessPointOnly;
  }

  Serial.println(F("[WiFi] === Initialization complete ==="));
}

void wifiManagerLoop()
{
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

const char *wifiManagerAccessPointSsid() { return AP_SSID; }
const char *wifiManagerAccessPointPassword() { return AP_PASSWORD; }

bool wifiManagerSetCredentials(const String &ssid, const String &password)
{
  String s = ssid;
  String p = password;
  s.trim();
  p.trim();

  if (s.isEmpty())
    return false;

  if (g_accessPointActive)
  {
    g_pendingSsid = s;
    g_pendingPassword = p;
    g_hasPending = true;

    if (WiFi.getMode() != WIFI_AP_STA)
      WiFi.mode(WIFI_AP_STA);

    clearConnectEvents();
    WiFi.begin(s.c_str(), p.c_str());
    g_connectStart = millis();
    g_state = State::Connecting;
    return true;
  }

  if (WiFi.status() == WL_CONNECTED && WiFi.getMode() == WIFI_STA)
  {
    const bool switched = attemptStaSwitchWithRollback(s, p, CONNECTION_TIMEOUT_MS);
    if (switched)
    {
      g_prefs.putString("ssid", s);
      g_prefs.putString("pass", p);
      g_ssid = s;
      g_password = p;
      g_hasCredentials = true;
      g_state = State::Connected;
      scheduleBackoff(false);
    }
    return true;
  }

  g_prefs.putString("ssid", s);
  g_prefs.putString("pass", p);
  g_ssid = s;
  g_password = p;
  g_hasCredentials = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  beginStationConnection();
  return true;
}

bool wifiManagerForgetCredentials()
{
  g_prefs.remove("ssid");
  g_prefs.remove("pass");

  g_pendingSsid = "";
  g_pendingPassword = "";
  g_hasPending = false;
  g_ssid = "";
  g_password = "";
  g_hasCredentials = false;

  WiFi.disconnect(true);
  startAccessPoint();
  g_state = State::AccessPointOnly;
  scheduleBackoff(false);
  return true;
}

void wifiManagerEnsureAccessPoint()
{
  if (g_state != State::Connected && !g_accessPointActive)
    startAccessPoint();
}

// ---------- State handlers ----------
static void handleAccessPointOnly()
{
  if (!g_accessPointActive)
    startAccessPoint();

  if (!g_hasCredentials)
    return;

  uint32_t now = millis();
  if (g_nextReconnectAt == 0)
    scheduleBackoff(false);

  if (now >= g_nextReconnectAt)
    beginStationConnection();
}

static void handleConnecting()
{
  wl_status_t st = WiFi.status();

  if (g_evtAuthFail || g_evtNoApFound)
  {
    if (g_hasPending)
      g_hasPending = false;

    if (!g_accessPointActive)
      startAccessPoint();

    g_state = State::AccessPointOnly;
    scheduleBackoff(true);
    return;
  }

  if (st == WL_CONNECTED)
  {
    if (g_hasPending)
    {
      g_prefs.putString("ssid", g_pendingSsid);
      g_prefs.putString("pass", g_pendingPassword);
      g_ssid = g_pendingSsid;
      g_password = g_pendingPassword;
      g_hasCredentials = true;
      g_hasPending = false;
    }

    if (g_accessPointActive)
      stopAccessPoint();

    if (WiFi.getMode() != WIFI_STA)
      WiFi.mode(WIFI_STA);

    g_state = State::Connected;
    scheduleBackoff(false);
    return;
  }

  if (millis() - g_connectStart > CONNECTION_TIMEOUT_MS)
  {
    if (g_hasPending)
      g_hasPending = false;

    if (!g_accessPointActive)
      startAccessPoint();

    g_state = State::AccessPointOnly;
    scheduleBackoff(true);
  }
}

static void handleConnected()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (g_accessPointActive)
      stopAccessPoint();
    if (WiFi.getMode() != WIFI_STA)
      WiFi.mode(WIFI_STA);
    return;
  }

  if (!WiFi.reconnect())
  {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);

    if (g_hasCredentials)
      beginStationConnection();
    else
    {
      startAccessPoint();
      g_state = State::AccessPointOnly;
      scheduleBackoff(false);
    }
  }
}

// ---------- Scan ----------
std::vector<WifiScanResult> wifiManagerScanNetworks()
{
  wifi_mode_t prior = WiFi.getMode();

  if (g_accessPointActive && prior != WIFI_AP_STA)
    WiFi.mode(WIFI_AP_STA);
  else if (!g_accessPointActive && prior != WIFI_STA)
    WiFi.mode(WIFI_STA);

  int n = WiFi.scanNetworks(false, true);
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

  if (WiFi.getMode() != prior)
    WiFi.mode(prior);

  WiFi.scanDelete();
  return out;
}