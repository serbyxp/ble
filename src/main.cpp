#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "ble_command_processor.h"
#include "command_message.h"
#include "device_config.h"
#include "transport_websocket.h"

namespace
{
  constexpr UBaseType_t COMMAND_QUEUE_LENGTH = 8;

  QueueHandle_t g_commandQueue = nullptr;
  BleCommandProcessor g_processor;
  DeviceConfig &g_config = getMutableDeviceConfig();

  void sendInputTooLong()
  {
    Serial.println(F("{\"status\":\"error\",\"message\":\"Input too long\"}"));
  }

  void sendQueueFull()
  {
    Serial.println(F("{\"status\":\"error\",\"message\":\"Command queue full\"}"));
  }

  void transportTask(void *)
  {
    String buffer;
    buffer.reserve(COMMAND_MESSAGE_MAX_LENGTH);

    websocketTransportBegin(g_commandQueue);

    TransportType lastTransport = g_config.transport;

    for (;;)
    {
      const DeviceConfig &config = getDeviceConfig();

      if (config.transport != lastTransport)
      {
        if (config.transport != TransportType::Uart)
        {
          buffer = "";
        }
        lastTransport = config.transport;
      }

      if (config.transport == TransportType::Uart)
      {
        while (Serial.available())
        {
          char c = static_cast<char>(Serial.read());

          if (c == '\r')
          {
            continue;
          }

          if (c == '\n')
          {
            if (buffer.length() > 0)
            {
              CommandMessage message;
              message.length = buffer.length();
              buffer.toCharArray(message.payload, COMMAND_MESSAGE_MAX_LENGTH + 1);
              message.payload[message.length] = '\0';
              if (xQueueSend(g_commandQueue, &message, 0) != pdPASS)
              {
                sendQueueFull();
              }
              buffer = "";
            }
            continue;
          }

          if (buffer.length() >= COMMAND_MESSAGE_MAX_LENGTH)
          {
            sendInputTooLong();
            buffer = "";
            continue;
          }

          buffer += c;
        }
      }
      else
      {
        while (Serial.available())
        {
          Serial.read();
        }
      }

      websocketTransportLoop();
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

  void bleTask(void *)
  {
    g_processor.begin();
    g_processor.sendReadyEvent();

    for (;;)
    {
      CommandMessage message;
      if (xQueueReceive(g_commandQueue, &message, pdMS_TO_TICKS(10)) == pdPASS)
      {
        g_processor.handleCommand(message);
      }

      g_processor.pollConnection();
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

} // namespace

void setup()
{
  Serial.begin(115200);

  if (!loadDeviceConfig())
  {
    Serial.println(F("{\"status\":\"warning\",\"message\":\"Using default configuration\"}"));
  }

  g_commandQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(CommandMessage));
  if (!g_commandQueue)
  {
    Serial.println(F("{\"status\":\"error\",\"message\":\"Failed to create command queue\"}"));
    while (true)
    {
      delay(1000);
    }
  }

  xTaskCreatePinnedToCore(transportTask, "transport", 4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(bleTask, "ble", 6144, nullptr, 1, nullptr, 0);
}

void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}
