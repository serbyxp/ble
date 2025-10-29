#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "ble_command_processor.h"
#include "command_message.h"
#include "device_config.h"
#include "transport_uart.h"
#include "transport_websocket.h"

namespace
{
  constexpr UBaseType_t COMMAND_QUEUE_LENGTH = 8;

  QueueHandle_t g_commandQueue = nullptr;
  BleCommandProcessor g_processor;

  void transportTask(void *)
  {
    websocketTransportBegin(g_commandQueue);
    uartTransportBegin(g_commandQueue);

    TransportType lastTransport = getDeviceConfig().transport;

    for (;;)
    {
      const DeviceConfig &config = getDeviceConfig();

      bool transportChanged = config.transport != lastTransport;
      bool uartSettingsChanged = consumeUartConfigChanged();

      if (transportChanged || uartSettingsChanged)
      {
        uartTransportHandleConfigChange(uartSettingsChanged);
        lastTransport = config.transport;
      }

      uartTransportLoop();
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
  Serial.println(F("[APP] Booting BLE bridge firmware"));

  bool configLoaded = loadDeviceConfig();

  if (!configLoaded)
  {
    Serial.println(F("[APP] Failed to load stored configuration; defaults will be used"));
  }
  else
  {
    const DeviceConfig &config = getDeviceConfig();
    Serial.printf("[APP] Loaded config: transport=%s, uart=%lu, hasWifi=%s\n",
                  transportTypeToString(config.transport),
                  static_cast<unsigned long>(config.uartBaudRate),
                  config.hasWifiCredentials ? "true" : "false");
    if (config.hasWifiCredentials)
    {
      Serial.printf("[APP] Stored WiFi SSID: %s\n", config.wifi.ssid.c_str());
    }
  }

  g_commandQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(CommandMessage));

  uartTransportBegin(g_commandQueue);

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

  Serial.println(F("[APP] Setup complete, tasks started"));
}

void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}
