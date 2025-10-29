#include "device_config.h"
#include "transport_websocket.h"

#include <Preferences.h>
#include <BleCombo.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace
{
  constexpr const char *NAMESPACE = "device";
  Preferences g_preferences;
  DeviceConfig g_config;
  volatile bool g_uartConfigDirty = false;
  bool g_bleIdentityDirty = false;
  portMUX_TYPE g_bleIdentityMux = portMUX_INITIALIZER_UNLOCKED;

  const char *const TRANSPORT_UART = "uart";
  const char *const TRANSPORT_WEBSOCKET = "websocket";
  const char *const KEY_BLE_NAME = "bleName";
  const char *const KEY_BLE_MANUFACTURER = "bleManuf";

  String readPreferenceString(Preferences &prefs, const char *key, size_t maxLength = 0)
  {
    if (!prefs.isKey(key))
    {
      return String();
    }

    String value = prefs.getString(key, "");
    if (maxLength > 0 && value.length() > maxLength)
    {
      value.remove(maxLength);
    }
    return value;
  }

  bool writePreferenceString(Preferences &prefs, const char *key, const String &value, size_t maxLength = 0)
  {
    if (maxLength > 0 && value.length() > maxLength)
    {
      return false;
    }

    size_t written = prefs.putString(key, value);
    return written > 0;
  }

  constexpr uint32_t SUPPORTED_BAUD_RATES[] = {
      9600,
      19200,
      38400,
      57600,
      115200,
      230400,
      460800,
      921600,
  };

  uint32_t sanitizeBaudRate(uint32_t value)
  {
    for (uint32_t supported : SUPPORTED_BAUD_RATES)
    {
      if (supported == value)
      {
        return value;
      }
    }
    return UART_BAUD_DEFAULT;
  }

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
  g_config.uartBaudRate = sanitizeBaudRate(g_preferences.getULong("uartBaud", UART_BAUD_DEFAULT));

  String ssid = readPreferenceString(g_preferences, "ssid", WIFI_SSID_MAX_LENGTH);
  String password = readPreferenceString(g_preferences, "password", WIFI_PASSWORD_MAX_LENGTH);

  g_config.wifi.ssid = ssid;
  g_config.wifi.password = password;
  g_config.hasWifiCredentials = ssid.length() > 0;

  String bleName = readPreferenceString(g_preferences, KEY_BLE_NAME);
  g_config.bleDeviceName = bleName;
  g_config.hasBleDeviceName = bleName.length() > 0;

  String bleManufacturer = readPreferenceString(g_preferences, KEY_BLE_MANUFACTURER);
  g_config.bleManufacturerName = bleManufacturer;
  g_config.hasBleManufacturerName = bleManufacturer.length() > 0;

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

  if (!g_preferences.putULong("uartBaud", g_config.uartBaudRate))
  {
    ok = false;
  }

  if (g_config.hasWifiCredentials && g_config.wifi.ssid.length() > 0)
  {
    if (!writePreferenceString(g_preferences, "ssid", g_config.wifi.ssid, WIFI_SSID_MAX_LENGTH))
    {
      ok = false;
    }
    if (!writePreferenceString(g_preferences, "password", g_config.wifi.password, WIFI_PASSWORD_MAX_LENGTH))
    {
      ok = false;
    }
  }
  else
  {
    g_preferences.remove("ssid");
    g_preferences.remove("password");
  }

  if (g_config.hasBleDeviceName && g_config.bleDeviceName.length() > 0)
  {
    if (!writePreferenceString(g_preferences, KEY_BLE_NAME, g_config.bleDeviceName))
    {
      ok = false;
    }
  }
  else
  {
    g_preferences.remove(KEY_BLE_NAME);
  }

  if (g_config.hasBleManufacturerName && g_config.bleManufacturerName.length() > 0)
  {
    if (!writePreferenceString(g_preferences, KEY_BLE_MANUFACTURER, g_config.bleManufacturerName))
    {
      ok = false;
    }
  }
  else
  {
    g_preferences.remove(KEY_BLE_MANUFACTURER);
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

bool isSupportedUartBaudRate(uint32_t baudRate)
{
  for (uint32_t supported : SUPPORTED_BAUD_RATES)
  {
    if (supported == baudRate)
    {
      return true;
    }
  }
  return false;
}

const uint32_t *getSupportedUartBaudRates(size_t &count)
{
  count = sizeof(SUPPORTED_BAUD_RATES) / sizeof(SUPPORTED_BAUD_RATES[0]);
  return SUPPORTED_BAUD_RATES;
}

void notifyUartConfigChanged()
{
  g_uartConfigDirty = true;
}

bool consumeUartConfigChanged()
{
  bool wasDirty = g_uartConfigDirty;
  g_uartConfigDirty = false;
  return wasDirty;
}

void notifyBleIdentityChanged()
{
  portENTER_CRITICAL(&g_bleIdentityMux);
  g_bleIdentityDirty = true;
  portEXIT_CRITICAL(&g_bleIdentityMux);
}

bool consumeBleIdentityChanged()
{
  portENTER_CRITICAL(&g_bleIdentityMux);
  bool wasDirty = g_bleIdentityDirty;
  g_bleIdentityDirty = false;
  portEXIT_CRITICAL(&g_bleIdentityMux);
  return wasDirty;
}

String getEffectiveBleDeviceName()
{
  const DeviceConfig &config = getDeviceConfig();
  if (config.hasBleDeviceName && config.bleDeviceName.length() > 0)
  {
    return config.bleDeviceName;
  }
  return generateApSsid();
}

String getEffectiveBleDeviceManufacturer()
{
  const DeviceConfig &config = getDeviceConfig();
  if (config.hasBleManufacturerName && config.bleManufacturerName.length() > 0)
  {
    return config.bleManufacturerName;
  }
  return String(Keyboard.deviceManufacturer.c_str());
}
