#include "device_config.h"

#include <Preferences.h>

namespace
{
constexpr const char *NAMESPACE = "device";
Preferences g_preferences;
DeviceConfig g_config;

const char *const TRANSPORT_UART = "uart";
const char *const TRANSPORT_WEBSOCKET = "websocket";

TransportType sanitizeTransport(uint8_t value)
{
  switch (static_cast<TransportType>(value))
  {
  case TransportType::Uart:
    return TransportType::Uart;
  case TransportType::Websocket:
    return TransportType::Websocket;
  }
  return TransportType::Websocket;
}

} // namespace

bool loadDeviceConfig()
{
  if (!g_preferences.begin(NAMESPACE, true))
  {
    return false;
  }

  g_config.transport = sanitizeTransport(g_preferences.getUChar("transport", static_cast<uint8_t>(TransportType::Websocket)));

  String ssid = g_preferences.getString("ssid", "");
  String password = g_preferences.getString("password", "");

  g_config.wifi.ssid = ssid;
  g_config.wifi.password = password;
  g_config.hasWifiCredentials = ssid.length() > 0;

  g_preferences.end();
  return true;
}

bool saveDeviceConfig()
{
  if (!g_preferences.begin(NAMESPACE, false))
  {
    return false;
  }

  bool ok = true;
  if (!g_preferences.putUChar("transport", static_cast<uint8_t>(g_config.transport)))
  {
    ok = false;
  }

  if (g_config.hasWifiCredentials && g_config.wifi.ssid.length() > 0)
  {
    if (!g_preferences.putString("ssid", g_config.wifi.ssid))
    {
      ok = false;
    }
    if (!g_preferences.putString("password", g_config.wifi.password))
    {
      ok = false;
    }
  }
  else
  {
    g_preferences.remove("ssid");
    g_preferences.remove("password");
  }

  g_preferences.end();
  return ok;
}

DeviceConfig &getMutableDeviceConfig()
{
  return g_config;
}

const DeviceConfig &getDeviceConfig()
{
  return g_config;
}

const char *transportTypeToString(TransportType type)
{
  switch (type)
  {
  case TransportType::Uart:
    return TRANSPORT_UART;
  case TransportType::Websocket:
    return TRANSPORT_WEBSOCKET;
  }
  return TRANSPORT_WEBSOCKET;
}

bool parseTransportType(const String &value, TransportType &typeOut)
{
  if (value.equalsIgnoreCase(TRANSPORT_UART))
  {
    typeOut = TransportType::Uart;
    return true;
  }
  if (value.equalsIgnoreCase(TRANSPORT_WEBSOCKET))
  {
    typeOut = TransportType::Websocket;
    return true;
  }
  return false;
}
