#include "device_config.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>

namespace
{
  constexpr const char *PREF_NAMESPACE = "device";
  constexpr const char *KEY_TRANSPORT = "transport";
  constexpr const char *KEY_UART_BAUD = "uart_baud";
  constexpr uint32_t DEFAULT_UART_BAUD = 115200;

  Preferences g_preferences;
  bool g_initialized = false;
  DeviceConfig g_activeConfig{TransportType::Websocket, DEFAULT_UART_BAUD};
  portMUX_TYPE g_configMux = portMUX_INITIALIZER_UNLOCKED;

  void ensureInitialized()
  {
    if (g_initialized)
    {
      return;
    }

    g_preferences.begin(PREF_NAMESPACE, false);

    String storedTransport = g_preferences.getString(KEY_TRANSPORT, "websocket");
    storedTransport.toLowerCase();

    if (storedTransport == "uart")
    {
      g_activeConfig.transport = TransportType::Uart;
    }
    else
    {
      g_activeConfig.transport = TransportType::Websocket;
    }

    g_activeConfig.uartBaud = g_preferences.getUInt(KEY_UART_BAUD, DEFAULT_UART_BAUD);

    g_initialized = true;
  }
}

void deviceConfigInitialize()
{
  ensureInitialized();
}

DeviceConfig deviceConfigGet()
{
  ensureInitialized();

  DeviceConfig copy;
  portENTER_CRITICAL(&g_configMux);
  copy = g_activeConfig;
  portEXIT_CRITICAL(&g_configMux);
  return copy;
}

void deviceConfigSet(const DeviceConfig &config)
{
  ensureInitialized();

  portENTER_CRITICAL(&g_configMux);
  g_activeConfig = config;
  portEXIT_CRITICAL(&g_configMux);
}

bool deviceConfigSave(const DeviceConfig &config)
{
  ensureInitialized();

  bool ok = true;
  ok &= g_preferences.putString(KEY_TRANSPORT, deviceConfigTransportToString(config.transport)) > 0;
  ok &= g_preferences.putUInt(KEY_UART_BAUD, config.uartBaud) > 0;
  return ok;
}

const char *deviceConfigTransportToString(TransportType type)
{
  switch (type)
  {
  case TransportType::Websocket:
    return "websocket";
  case TransportType::Uart:
    return "uart";
  }
  return "websocket";
}

bool deviceConfigParseTransport(const String &value, TransportType &type)
{
  String lower = value;
  lower.trim();
  lower.toLowerCase();
  if (lower == "websocket")
  {
    type = TransportType::Websocket;
    return true;
  }
  if (lower == "uart")
  {
    type = TransportType::Uart;
    return true;
  }
  return false;
}
