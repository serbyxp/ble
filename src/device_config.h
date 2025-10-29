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

constexpr uint32_t UART_BAUD_DEFAULT = 115200;

struct DeviceConfig
{
  TransportType transport = TransportType::Websocket;
  uint32_t uartBaudRate = UART_BAUD_DEFAULT;
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

/**
 * @brief Check whether the provided baud rate is supported by the device.
 */
bool isSupportedUartBaudRate(uint32_t baudRate);

/**
 * @brief Obtain the list of supported UART baud rates.
 */
const uint32_t *getSupportedUartBaudRates(size_t &count);

/**
 * @brief Notify listeners that the UART configuration has changed.
 */
void notifyUartConfigChanged();

/**
 * @brief Consume any pending UART configuration change notification.
 */
bool consumeUartConfigChanged();
