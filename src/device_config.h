#pragma once

#include <Arduino.h>

enum class TransportType
{
  Websocket,
  Uart
};

struct DeviceConfig
{
  TransportType transport;
  uint32_t uartBaud;
};

void deviceConfigInitialize();
DeviceConfig deviceConfigGet();
void deviceConfigSet(const DeviceConfig &config);
bool deviceConfigSave(const DeviceConfig &config);
const char *deviceConfigTransportToString(TransportType type);
bool deviceConfigParseTransport(const String &value, TransportType &type);
