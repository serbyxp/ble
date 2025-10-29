#include "transport_uart.h"

#include "command_message.h"
#include "device_config.h"

#include <Arduino.h>

namespace
{
  QueueHandle_t g_queue = nullptr;
  String g_buffer;
  bool g_initialized = false;
  uint32_t g_lastBaudRate = UART_BAUD_DEFAULT;
  TransportType g_lastTransport = TransportType::Uart;

  void resetBuffer()
  {
    g_buffer = "";
  }

  void flushSerialInput()
  {
    while (Serial.available())
    {
      Serial.read();
    }
  }

  void sendInputTooLong()
  {
    Serial.println(F("{\"status\":\"error\",\"message\":\"Input too long\"}"));
  }

  void sendQueueFull()
  {
    Serial.println(F("{\"status\":\"error\",\"message\":\"Command queue full\"}"));
  }

  void ensureBufferInitialized()
  {
    if (!g_initialized)
    {
      g_buffer.reserve(COMMAND_MESSAGE_MAX_LENGTH);
      g_initialized = true;
    }
  }

} // namespace

void uartTransportBegin(QueueHandle_t queue)
{
  g_queue = queue;
  ensureBufferInitialized();

  const DeviceConfig &config = getDeviceConfig();
  g_lastTransport = config.transport;
  g_lastBaudRate = config.uartBaudRate;

  Serial.begin(g_lastBaudRate);
  resetBuffer();

  if (config.transport != TransportType::Uart)
  {
    flushSerialInput();
  }
}

void uartTransportLoop()
{
  ensureBufferInitialized();

  const DeviceConfig &config = getDeviceConfig();
  if (config.transport != TransportType::Uart)
  {
    flushSerialInput();
    return;
  }

  while (Serial.available())
  {
    char c = static_cast<char>(Serial.read());

    if (c == '\r')
    {
      continue;
    }

    if (c == '\n')
    {
      if (g_buffer.length() > 0 && g_queue)
      {
        CommandMessage message;
        message.length = g_buffer.length();
        g_buffer.toCharArray(message.payload, COMMAND_MESSAGE_MAX_LENGTH + 1);
        message.payload[message.length] = '\0';
        if (xQueueSend(g_queue, &message, 0) != pdPASS)
        {
          sendQueueFull();
        }
      }
      resetBuffer();
      continue;
    }

    if (g_buffer.length() >= COMMAND_MESSAGE_MAX_LENGTH)
    {
      sendInputTooLong();
      resetBuffer();
      continue;
    }

    g_buffer += c;
  }
}

void uartTransportHandleConfigChange(bool uartSettingsChanged)
{
  ensureBufferInitialized();

  const DeviceConfig &config = getDeviceConfig();
  bool transportChanged = config.transport != g_lastTransport;
  bool baudChanged = uartSettingsChanged || (config.uartBaudRate != g_lastBaudRate);

  if (baudChanged)
  {
    Serial.flush();
    Serial.begin(config.uartBaudRate);
    g_lastBaudRate = config.uartBaudRate;
    flushSerialInput();
  }

  if (transportChanged || baudChanged)
  {
    resetBuffer();
  }

  if (transportChanged && config.transport != TransportType::Uart)
  {
    flushSerialInput();
  }

  g_lastTransport = config.transport;
}
