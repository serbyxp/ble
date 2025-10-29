#include "device_config.h"

#include <Preferences.h>
#include <BleCombo.h>

namespace
{
  constexpr const char *NAMESPACE = "device";
  Preferences g_preferences;
  DeviceConfig g_config;
  volatile bool g_uartConfigDirty = false;

  const char *const TRANSPORT_UART = "uart";
  const char *const TRANSPORT_WEBSOCKET = "websocket";
  const char *const KEY_BLE_NAME = "bleName";
  const char *const KEY_BLE_MANUFACTURER = "bleManuf";

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

  String ssid = g_preferences.getString("ssid", "");
  String password = g_preferences.getString("password", "");

  g_config.wifi.ssid = ssid;
  g_config.wifi.password = password;
  g_config.hasWifiCredentials = ssid.length() > 0;

  String bleName = g_preferences.getString(KEY_BLE_NAME, "");
  g_config.bleDeviceName = bleName;
  g_config.hasBleDeviceName = bleName.length() > 0;

  String bleManufacturer = g_preferences.getString(KEY_BLE_MANUFACTURER, "");
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

  if (g_config.hasBleDeviceName && g_config.bleDeviceName.length() > 0)
  {
    if (!g_preferences.putString(KEY_BLE_NAME, g_config.bleDeviceName))
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
    if (!g_preferences.putString(KEY_BLE_MANUFACTURER, g_config.bleManufacturerName))
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

String getEffectiveBleDeviceName()
{
  const DeviceConfig &config = getDeviceConfig();
  if (config.hasBleDeviceName && config.bleDeviceName.length() > 0)
  {
    return config.bleDeviceName;
  }
  if (config.wifi.ssid.length() > 0)
  {
    return config.wifi.ssid;
  }
  return String();
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
