#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>
#include <DNSServer.h>
#include <vector>
#include <esp_bt.h>
#include <esp_coexist.h>
#include <esp_wifi.h>

namespace
{
  constexpr const char *PREF_NAMESPACE = "wifi";
  constexpr const char *KEY_SSID = "ssid";
  constexpr const char *KEY_PASSWORD = "password";
  constexpr const char *ACCESS_POINT_SSID = "ble-hid";
  constexpr const char *ACCESS_POINT_PASSWORD = "uhid1234";
  constexpr uint32_t CONNECTION_TIMEOUT_MS = 20000;
  constexpr uint32_t RECONNECT_INTERVAL_MS = 30000;

  constexpr uint8_t DNS_PORT = 53;

  Preferences g_preferences;
  bool g_preferencesInitialized = false;
  bool g_initialized = false;
  bool g_accessPointActive = false;
  bool g_hasCredentials = false;
  String g_savedSsid;
  String g_savedPassword;
  DNSServer g_dnsServer;
  bool g_dnsServerRunning = false;

  void startDnsServer(const IPAddress &ip)
  {
    if (g_dnsServerRunning)
    {
      g_dnsServer.stop();
      g_dnsServerRunning = false;
    }

    g_dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    if (g_dnsServer.start(DNS_PORT, "*", ip))
    {
      g_dnsServerRunning = true;
      Serial.println(F("[WiFi] Captive portal DNS started"));
    }
    else
    {
      Serial.println(F("[WiFi] Failed to start captive portal DNS"));
    }
  }

  void stopDnsServer()
  {
    if (!g_dnsServerRunning)
    {
      return;
    }

    g_dnsServer.stop();
    g_dnsServerRunning = false;
    Serial.println(F("[WiFi] Captive portal DNS stopped"));
  }

  enum class WifiState
  {
    AccessPointOnly,
    Connecting,
    Connected
  };

  WifiState g_state = WifiState::AccessPointOnly;
  uint32_t g_connectStart = 0;
  uint32_t g_lastReconnectAttempt = 0;

  void ensurePreferences()
  {
    if (g_preferencesInitialized)
    {
      return;
    }

    g_preferences.begin(PREF_NAMESPACE, false);
    g_savedSsid = g_preferences.getString(KEY_SSID, "");
    g_savedPassword = g_preferences.getString(KEY_PASSWORD, "");
    g_hasCredentials = !g_savedSsid.isEmpty();
    g_preferencesInitialized = true;
  }

  void startAccessPoint()
  {
    if (g_accessPointActive)
    {
      return;
    }

    wifi_mode_t targetMode = g_hasCredentials ? WIFI_AP_STA : WIFI_AP;
    if (WiFi.getMode() != targetMode)
    {
      WiFi.mode(targetMode);
    }
    IPAddress apIp(192, 168, 4, 1);
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
    if (WiFi.softAP(ACCESS_POINT_SSID, ACCESS_POINT_PASSWORD))
    {
      g_accessPointActive = true;
      Serial.printf("[WiFi] Started access point '%s'\n", ACCESS_POINT_SSID);
      startDnsServer(WiFi.softAPIP());
    }
    else
    {
      Serial.println(F("[WiFi] Failed to start access point"));
    }
  }

  void stopAccessPoint()
  {
    if (!g_accessPointActive)
    {
      return;
    }

    WiFi.softAPdisconnect(true);
    g_accessPointActive = false;
    Serial.println(F("[WiFi] Access point stopped"));
    stopDnsServer();
  }

  void beginSavedStation()
  {
    if (g_savedPassword.isEmpty())
    {
      WiFi.begin(g_savedSsid.c_str());
    }
    else
    {
      WiFi.begin(g_savedSsid.c_str(), g_savedPassword.c_str());
    }
  }

  void beginStationConnection()
  {
    if (!g_hasCredentials)
    {
      return;
    }

    wifi_mode_t targetMode = g_accessPointActive ? WIFI_AP_STA : WIFI_STA;
    if (WiFi.getMode() != targetMode)
    {
      WiFi.mode(targetMode);
    }

    Serial.printf("[WiFi] Connecting to '%s'\n", g_savedSsid.c_str());
    if (g_accessPointActive)
    {
      Serial.println(F("[WiFi] Access point remains active during station connection attempt"));
    }
    beginSavedStation();
    uint32_t now = millis();
    g_connectStart = now;
    g_lastReconnectAttempt = now;
    g_state = WifiState::Connecting;
  }

  void handleConnected()
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println(F("[WiFi] Station connection lost"));
      WiFi.disconnect();
      if (g_accessPointActive)
      {
        Serial.println(F("[WiFi] Access point already active during reconnection attempt"));
      }
      else if (g_hasCredentials)
      {
        Serial.println(F("[WiFi] Restarting access point for reconnection attempt"));
        startAccessPoint();
      }
      beginStationConnection();
      return;
    }

    if (g_accessPointActive)
    {
      Serial.println(F("[WiFi] Station connected; shutting down access point"));
      stopAccessPoint();
    }

    if (WiFi.getMode() != WIFI_MODE_STA)
    {
      WiFi.mode(WIFI_STA);
    }
  }

  void handleConnecting()
  {
    wl_status_t status = WiFi.status();
    uint32_t now = millis();

    if (status == WL_CONNECTED)
    {
      Serial.printf("[WiFi] Connected to '%s' with IP %s\n", g_savedSsid.c_str(), WiFi.localIP().toString().c_str());
      g_state = WifiState::Connected;
      return;
    }

    if (now - g_connectStart >= CONNECTION_TIMEOUT_MS)
    {
      Serial.println(F("[WiFi] Connection timed out, keeping access point active"));
      WiFi.disconnect(true);
      if (!g_accessPointActive)
      {
        Serial.println(F("[WiFi] Warning: access point unexpectedly inactive during retry"));
        startAccessPoint();
      }
      else
      {
        Serial.println(F("[WiFi] Access point still active; will retry station connection"));
      }
      g_state = WifiState::AccessPointOnly;
      g_lastReconnectAttempt = now;
    }
  }

  void handleAccessPointOnly()
  {
    if (!g_accessPointActive)
    {
      if (g_hasCredentials)
      {
        Serial.println(F("[WiFi] Access point offline, retrying station connection"));
        startAccessPoint();
        beginStationConnection();
      }
      else
      {
        startAccessPoint();
      }
      return;
    }

    if (!g_hasCredentials)
    {
      return;
    }

    if (!g_accessPointActive)
    {
      Serial.println(F("[WiFi] Warning: expected access point to remain active during retries"));
    }

    uint32_t now = millis();
    if (now - g_lastReconnectAttempt < RECONNECT_INTERVAL_MS)
    {
      return;
    }

    beginStationConnection();
  }

  void resetStationState()
  {
    WiFi.disconnect(true);
    stopAccessPoint();
    if (!g_hasCredentials)
    {
      g_state = WifiState::AccessPointOnly;
      startAccessPoint();
    }
    else
    {
      beginStationConnection();
    }
  }

} // namespace

void wifiManagerInitialize()
{
  if (g_initialized)
  {
    return;
  }

  ensurePreferences();

  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setAutoConnect(false);

  if (g_hasCredentials)
  {
    beginStationConnection();
  }
  else
  {
    WiFi.mode(WIFI_AP);
    startAccessPoint();
    g_state = WifiState::AccessPointOnly;
  }

  g_initialized = true;
}

void wifiManagerLoop()
{
  if (!g_initialized)
  {
    return;
  }

  switch (g_state)
  {
  case WifiState::Connected:
    handleConnected();
    break;
  case WifiState::Connecting:
    handleConnecting();
    break;
  case WifiState::AccessPointOnly:
    handleAccessPointOnly();
    break;
  }

  if (g_dnsServerRunning)
  {
    g_dnsServer.processNextRequest();
  }
}

WifiManagerStatus wifiManagerGetStatus()
{
  WifiManagerStatus status{};
  status.hasCredentials = g_hasCredentials;
  status.connected = WiFi.status() == WL_CONNECTED;
  status.connecting = g_state == WifiState::Connecting;
  status.accessPointActive = g_accessPointActive;
  status.connectedSsid = status.connected ? WiFi.SSID() : (g_hasCredentials ? g_savedSsid : String());
  status.stationIp = status.connected ? WiFi.localIP() : IPAddress();
  status.accessPointIp = g_accessPointActive ? WiFi.softAPIP() : IPAddress();
  return status;
}

bool wifiManagerIsConnecting()
{
  return g_state == WifiState::Connecting;
}

const char *wifiManagerAccessPointSsid()
{
  return ACCESS_POINT_SSID;
}

const char *wifiManagerAccessPointPassword()
{
  return ACCESS_POINT_PASSWORD;
}

bool wifiManagerSetCredentials(const String &ssid, const String &password)
{
  ensurePreferences();

  String trimmedSsid = ssid;
  trimmedSsid.trim();

  if (trimmedSsid.isEmpty())
  {
    return false;
  }

  g_preferences.putString(KEY_SSID, trimmedSsid);
  g_preferences.putString(KEY_PASSWORD, password);
  g_savedSsid = trimmedSsid;
  g_savedPassword = password;
  g_hasCredentials = true;
  g_lastReconnectAttempt = 0;
  resetStationState();
  return true;
}

bool wifiManagerForgetCredentials()
{
  ensurePreferences();

  bool ok = true;
  ok &= g_preferences.remove(KEY_SSID);
  ok &= g_preferences.remove(KEY_PASSWORD);

  g_savedSsid = "";
  g_savedPassword = "";
  g_hasCredentials = false;
  g_lastReconnectAttempt = 0;
  resetStationState();
  return ok;
}

void wifiManagerEnsureAccessPoint()
{
  if (!g_initialized)
  {
    return;
  }

  if (g_state == WifiState::Connecting && g_hasCredentials)
  {
    Serial.println(F("[WiFi] Access point request deferred: station connection in progress"));
    return;
  }

  if (!g_accessPointActive)
  {
    startAccessPoint();
    g_state = WifiState::AccessPointOnly;
  }
}

std::vector<WifiScanResult> wifiManagerScanNetworks()
{
  std::vector<WifiScanResult> results;

  if (!g_initialized)
  {
    return results;
  }

  wifi_mode_t previousMode = WiFi.getMode();
  bool modeChanged = false;
  if (previousMode == WIFI_MODE_AP)
  {
    WiFi.mode(WIFI_AP_STA);
    modeChanged = true;
  }
  else if (previousMode == WIFI_MODE_NULL)
  {
    WiFi.mode(WIFI_STA);
    modeChanged = true;
  }

  int16_t count = WiFi.scanNetworks(/*async*/ false, /*show_hidden*/ true);

  if (count > 0)
  {
    results.reserve(static_cast<size_t>(count));
    for (int16_t i = 0; i < count; ++i)
    {
      WifiScanResult result;
      result.ssid = WiFi.SSID(i);
      result.rssi = WiFi.RSSI(i);
      result.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      result.hidden = result.ssid.isEmpty();
      results.push_back(std::move(result));
    }
    std::sort(results.begin(), results.end(), [](const WifiScanResult &a, const WifiScanResult &b) {
      return a.rssi > b.rssi;
    });
  }

  WiFi.scanDelete();

  if (modeChanged)
  {
    WiFi.mode(previousMode);
  }

  return results;
}
