#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "ble_command_processor.h"
#include "command_message.h"
#include "device_config.h"
#include "transport_websocket.h"
#include "wifi_manager.h"

namespace
{
  constexpr UBaseType_t COMMAND_QUEUE_LENGTH = 8;

  QueueHandle_t g_commandQueue = nullptr;
  BleCommandProcessor g_processor;
  bool g_bleInitialized = false;

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

    bool websocketRunning = false;
    DeviceConfig lastConfig = deviceConfigGet();

    if (lastConfig.transport == TransportType::Websocket)
    {
      websocketTransportBegin(g_commandQueue);
      websocketRunning = true;
    }

    for (;;)
    {
      wifiManagerLoop();
      DeviceConfig config = deviceConfigGet();

      if (config.transport == TransportType::Websocket)
      {
        if (!websocketRunning)
        {
          websocketTransportBegin(g_commandQueue);
          websocketRunning = true;
          buffer = "";
        }
      }
      else
      {
        if (websocketRunning)
        {
          websocketTransportEnd();
          websocketRunning = false;
        }

        if (config.uartBaud != lastConfig.uartBaud || lastConfig.transport != TransportType::Uart)
        {
          Serial.updateBaudRate(config.uartBaud);
        }

        if (lastConfig.transport != TransportType::Uart)
        {
          buffer = "";
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

      if (websocketRunning)
      {
        websocketTransportLoop();
      }

      lastConfig = config;
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

  void bleTask(void *)
  {
    Serial.println(F("[BLE] Task started"));

    // Bring BLE up regardless of Wi-Fi state; allow a short settle for coexistence.
    vTaskDelay(pdMS_TO_TICKS(750));

    Serial.println(F("[BLE] Initializing BLE..."));
    g_processor.begin();
    g_bleInitialized = true;
    Serial.println(F("[BLE] BLE initialized"));

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
  Serial.println(F("\n\n=== System Starting ==="));

  // Step 1: Initialize device config
  Serial.println(F("[Setup] Initializing device config..."));
  deviceConfigInitialize();
  DeviceConfig config = deviceConfigGet();

  // Step 2: Create command queue
  Serial.println(F("[Setup] Creating command queue..."));
  g_commandQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(CommandMessage));
  if (!g_commandQueue)
  {
    Serial.println(F("{\"status\":\"error\",\"message\":\"Failed to create command queue\"}"));
    while (true)
    {
      delay(1000);
    }
  }

  // Step 3: Initialize WiFi FIRST (critical for coexistence)
  Serial.println(F("[Setup] Initializing WiFi manager..."));
  wifiManagerInitialize();

  // Give WiFi time to start
  delay(500);

  // Step 4: Start WiFi/transport in its own task
  Serial.println(F("[Setup] Starting transport task..."));
  xTaskCreatePinnedToCore(transportTask, "transport", 4096, nullptr, 1, nullptr, 1);

  // Step 5: Start BLE in separate task (will wait for WiFi)
  Serial.println(F("[Setup] Starting BLE task (delayed init)..."));
  xTaskCreatePinnedToCore(bleTask, "ble", 6144, nullptr, 1, nullptr, 0);

  // Step 6: Start websocket if needed
  if (config.transport == TransportType::Websocket)
  {
    Serial.println(F("[Setup] Starting websocket transport..."));
    websocketTransportBegin(g_commandQueue);
  }

  Serial.println(F("[Setup] Setup complete, entering main loop"));
}

void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}