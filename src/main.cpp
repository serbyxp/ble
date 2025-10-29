#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <WiFi.h>

// ESP32 specific includes for coexistence
#if defined(ESP32)
#include <esp_bt.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <esp_bt_main.h>
#endif

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
  bool g_bleReady = false;

  // BLE task on Core 0 - STARTS LAST
  void bleTask(void *)
  {
    Serial.println(F("[BLE] Task starting on Core 0"));

    // CRITICAL: Wait for WiFi to fully initialize first
    // BLE and WiFi share ONE radio - WiFi MUST start first
    Serial.println(F("[BLE] Waiting for WiFi initialization..."));
    vTaskDelay(pdMS_TO_TICKS(5000)); // 5 second delay

    Serial.println(F("[BLE] Initializing BLE stack..."));

    // Release BT memory that might be allocated
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    g_processor.begin();
    g_bleReady = true;

    // Additional delay for BLE advertising to stabilize
    vTaskDelay(pdMS_TO_TICKS(1000));

    g_processor.sendReadyEvent();
    Serial.println(F("[BLE] Ready - device should be visible for pairing"));
    Serial.println(F("[BLE] Look for: 'BlynkGO Keyboard/Mouse'"));

    for (;;)
    {
      CommandMessage message;
      if (xQueueReceive(g_commandQueue, &message, pdMS_TO_TICKS(50)) == pdPASS)
      {
        g_processor.handleCommand(message);
      }

      g_processor.pollConnection();

      // Lower priority polling - give WiFi time
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  // UART task on Core 1
  void uartTask(void *)
  {
    Serial.println(F("[UART] Task starting on Core 1"));
    vTaskDelay(pdMS_TO_TICKS(1000)); // Let WiFi start first

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
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  // WiFi/WebSocket task on Core 1 - STARTS FIRST
  void websocketTask(void *)
  {
    Serial.println(F("[WiFi] Task starting on Core 1"));
    Serial.println(F("[WiFi] Initializing WiFi BEFORE BLE..."));

    // Initialize WebSocket/WiFi transport FIRST
    websocketTransportBegin(g_commandQueue);

    Serial.println(F("[WiFi] WiFi initialized - BLE can now start"));

    for (;;)
    {
      websocketTransportLoop();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

} // namespace

void setup()
{
  Serial.begin(115200);
  delay(500); // Let serial stabilize

  Serial.println(F("\n\n========================================"));
  Serial.println(F("  ESP32 BLE+WiFi Bridge"));
  Serial.println(F("========================================"));
  Serial.println(F("NOTE: BLE and WiFi share ONE radio"));
  Serial.println(F("WiFi initializes FIRST, then BLE"));
  Serial.println(F("========================================\n"));

  // Load configuration
  bool configLoaded = loadDeviceConfig();

  if (!configLoaded)
  {
    Serial.println(F("[CFG] Creating default configuration"));
    DeviceConfig &config = getMutableDeviceConfig();
    config.transport = TransportType::Websocket;
    config.uartBaudRate = UART_BAUD_DEFAULT;
    config.hasWifiCredentials = false;
    saveDeviceConfig();
  }

  const DeviceConfig &config = getDeviceConfig();
  Serial.printf("[CFG] Transport: %s\n", transportTypeToString(config.transport));
  Serial.printf("[CFG] UART: %lu baud\n", static_cast<unsigned long>(config.uartBaudRate));
  Serial.printf("[CFG] WiFi: %s\n", config.hasWifiCredentials ? config.wifi.ssid.c_str() : "No credentials");

  // Create command queue
  g_commandQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(CommandMessage));

  if (!g_commandQueue)
  {
    Serial.println(F("[FATAL] Failed to create command queue!"));
    while (true)
      delay(1000);
  }

  Serial.println(F("\n[INIT] Creating tasks in priority order:"));
  Serial.println(F("[INIT] 1. WiFi/WebSocket (Core 1) - HIGHEST PRIORITY"));
  Serial.println(F("[INIT] 2. UART (Core 1)"));
  Serial.println(F("[INIT] 3. BLE (Core 0) - DELAYED START\n"));

  // CRITICAL ORDER: WiFi FIRST (highest priority)
  xTaskCreatePinnedToCore(
      websocketTask,
      "wifi",
      8192,
      nullptr,
      3, // HIGHEST priority
      nullptr,
      1 // Core 1 (WiFi runs here)
  );

  delay(200);

  // UART task - medium priority
  xTaskCreatePinnedToCore(
      uartTask,
      "uart",
      4096,
      nullptr,
      2,
      nullptr,
      1 // Core 1
  );

  delay(200);

  // BLE task - LOWEST priority, delayed start
  xTaskCreatePinnedToCore(
      bleTask,
      "ble",
      8192,
      nullptr,
      1, // LOWEST priority
      nullptr,
      0 // Core 0 (BLE controller runs here)
  );

  Serial.println(F("[INIT] All tasks created"));
  Serial.println(F("[INIT] WiFi will start immediately"));
  Serial.println(F("[INIT] BLE will start after 5 second delay\n"));
}

void loop()
{
  // Status indicator every 10 seconds
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 10000)
  {
    lastStatus = millis();
    Serial.printf("[STATUS] Uptime: %lu seconds | BLE: %s\n",
                  millis() / 1000,
                  g_bleReady ? "Ready" : "Initializing");
  }

  vTaskDelay(pdMS_TO_TICKS(1000));
}