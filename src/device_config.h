#pragma once

#include <Arduino.h>

enum class TransportType : uint8_t
{
  Uart = 0,
  Websocket = 1
};

struct WifiCredentials
{
  String ssid;
  String password;
};

struct DeviceConfig
{
  TransportType transport = TransportType::Websocket;
  WifiCredentials wifi;
  bool hasWifiCredentials = false;
};

/**
 * @brief Load persisted configuration values from NVS into the global config instance.
 *
 * @return true if the configuration was loaded successfully, false otherwise.
 */
bool loadDeviceConfig();

/**
 * @brief Persist the current configuration values stored in memory to NVS.
 *
 * @return true if the configuration was written successfully, false otherwise.
 */
bool saveDeviceConfig();

/**
 * @brief Obtain a mutable reference to the singleton configuration instance.
 */
DeviceConfig &getMutableDeviceConfig();

/**
 * @brief Obtain a read-only reference to the singleton configuration instance.
 */
const DeviceConfig &getDeviceConfig();

/**
 * @brief Convert a transport type into a lower-case textual representation.
 */
const char *transportTypeToString(TransportType type);

/**
 * @brief Parse a textual transport identifier into an enum value.
 *
 * The parsing is case-insensitive and accepts "uart" and "websocket".
 */
bool parseTransportType(const String &value, TransportType &typeOut);
