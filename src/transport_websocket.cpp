#include "transport_websocket.h"

#include "command_message.h"
#include "device_config.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <cstring>

#include "web/index.html"

namespace
{
  constexpr uint16_t WEBSOCKET_PORT = 81;
  constexpr uint16_t HTTP_PORT = 80;
  constexpr uint8_t DNS_PORT = 53;
  constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
  constexpr const char *AP_PASSWORD = "uhid1234";

  QueueHandle_t g_queue = nullptr;
  WebSocketsServer g_websocket(WEBSOCKET_PORT);
  WebServer g_httpServer(HTTP_PORT);
  DNSServer g_dnsServer;

  bool g_apActive = false;
  bool g_dnsActive = false;
  bool g_staConnected = false;
  unsigned long g_lastConnectionAttempt = 0;
  String g_apSsid;
  IPAddress g_apIp;

  void sendQueueFull(uint8_t clientId)
  {
    static const char RESPONSE[] = "{\"status\":\"error\",\"message\":\"Command queue full\"}";
    g_websocket.sendTXT(clientId, RESPONSE);
  }

  void sendTooLong(uint8_t clientId)
  {
    static const char RESPONSE[] = "{\"status\":\"error\",\"message\":\"Input too long\"}";
    g_websocket.sendTXT(clientId, RESPONSE);
  }

  void sendTransportDisabled(uint8_t clientId)
  {
    static const char RESPONSE[] = "{\"status\":\"error\",\"message\":\"WebSocket transport is disabled\"}";
    g_websocket.sendTXT(clientId, RESPONSE);
  }

  String generateApSsid()
  {
    uint32_t identifier = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFF);
    char buffer[20];
    snprintf(buffer, sizeof(buffer), "ble-bridge-%06X", identifier);
    return String(buffer);
  }

  void startAccessPoint()
  {
    if (g_apActive)
    {
      return;
    }

    if (g_apSsid.isEmpty())
    {
      g_apSsid = generateApSsid();
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(g_apSsid.c_str(), AP_PASSWORD);
    g_apIp = WiFi.softAPIP();

    if (g_dnsServer.start(DNS_PORT, "*", g_apIp))
    {
      g_dnsActive = true;
    }

    g_apActive = true;
    Serial.printf("[WS] Access Point SSID: %s\n", g_apSsid.c_str());
    Serial.printf("[WS] AP IP Address: %s\n", g_apIp.toString().c_str());
  }

  void stopAccessPoint()
  {
    if (!g_apActive)
    {
      return;
    }

    if (g_dnsActive)
    {
      g_dnsServer.stop();
      g_dnsActive = false;
    }

    WiFi.softAPdisconnect(true);
    g_apActive = false;
    Serial.println("[WS] Access Point disabled");
  }

  void connectToConfiguredNetwork()
  {
    const DeviceConfig &config = getDeviceConfig();
    if (!config.hasWifiCredentials || config.wifi.ssid.isEmpty())
    {
      return;
    }

    startAccessPoint();
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(false, false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(config.wifi.ssid.c_str(), config.wifi.password.c_str());
    g_lastConnectionAttempt = millis();
    g_staConnected = false;
    Serial.printf("[WS] Connecting to WiFi SSID: %s\n", config.wifi.ssid.c_str());
  }

  void handleConfigGet()
  {
    JsonDocument doc(1024);
    const DeviceConfig &config = getDeviceConfig();

    doc["transport"] = transportTypeToString(config.transport);

    JsonObject uart = doc["uart"].to<JsonObject>();
    uart["baud"] = config.uartBaudRate;
    size_t supportedCount = 0;
    const uint32_t *supported = getSupportedUartBaudRates(supportedCount);
    JsonArray supportedArray = uart["supported"].to<JsonArray>();
    for (size_t i = 0; i < supportedCount; ++i)
    {
      supportedArray.add(supported[i]);
    }

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = config.hasWifiCredentials ? config.wifi.ssid : "";
    wifi["connected"] = WiFi.status() == WL_CONNECTED;
    wifi["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
    wifi["apActive"] = g_apActive;
    wifi["portalUrl"] = g_apActive ? String("http://") + g_apIp.toString() + "/" : "";

    String response;
    serializeJson(doc, response);
    g_httpServer.send(200, "application/json", response);
  }

  void respondError(uint16_t code, const char *message)
  {
    JsonDocument doc(256);
    doc["status"] = "error";
    doc["message"] = message;
    String payload;
    serializeJson(doc, payload);
    g_httpServer.send(code, "application/json", payload);
  }

  void connectOrDisconnectBasedOnConfig(bool wifiChanged)
  {
    DeviceConfig &config = getMutableDeviceConfig();
    if (wifiChanged)
    {
      if (config.hasWifiCredentials && !config.wifi.ssid.isEmpty())
      {
        connectToConfiguredNetwork();
      }
      else
      {
        WiFi.disconnect(true, true);
        startAccessPoint();
      }
    }
  }

  void handleConfigPost()
  {
    if (g_httpServer.method() != HTTP_POST)
    {
      respondError(405, "Method Not Allowed");
      return;
    }

    const String body = g_httpServer.arg("plain");
    if (body.isEmpty())
    {
      respondError(400, "Empty body");
      return;
    }

    JsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);
    if (error)
    {
      respondError(400, "Invalid JSON payload");
      return;
    }

    DeviceConfig &config = getMutableDeviceConfig();
    bool configChanged = false;
    bool wifiChanged = false;
    bool uartChanged = false;

    if (!doc["transport"].isNull())
    {
      TransportType newTransport;
      if (!parseTransportType(doc["transport"].as<String>(), newTransport))
      {
        respondError(400, "Unsupported transport option");
        return;
      }
      if (config.transport != newTransport)
      {
        config.transport = newTransport;
        configChanged = true;
      }
    }

    if (!doc["uart"].isNull())
    {
      JsonObject uart = doc["uart"].as<JsonObject>();
      if (!uart["baud"].isNull())
      {
        if (!uart["baud"].is<uint32_t>())
        {
          respondError(400, "Invalid UART baud rate");
          return;
        }
        uint32_t newBaud = uart["baud"].as<uint32_t>();
        if (!isSupportedUartBaudRate(newBaud))
        {
          respondError(400, "Unsupported UART baud rate");
          return;
        }
        if (config.uartBaudRate != newBaud)
        {
          config.uartBaudRate = newBaud;
          configChanged = true;
          uartChanged = true;
        }
      }
    }

    if (!doc["wifi"].isNull())
    {
      JsonObject wifi = doc["wifi"].as<JsonObject>();
      bool forget = !wifi["forget"].isNull() && wifi["forget"].as<bool>();
      if (forget)
      {
        if (config.hasWifiCredentials || !config.wifi.ssid.isEmpty())
        {
          config.hasWifiCredentials = false;
          config.wifi.ssid = "";
          config.wifi.password = "";
          configChanged = true;
          wifiChanged = true;
        }
      }
      else
      {
        bool hasSsid = !wifi["ssid"].isNull();
        bool hasPassword = !wifi["password"].isNull();
        String newSsid = hasSsid ? String(wifi["ssid"].as<const char *>()) : config.wifi.ssid;
        String newPassword = hasPassword ? String(wifi["password"].as<const char *>()) : config.wifi.password;
        bool newHasCredentials = newSsid.length() > 0;

        if (hasSsid || hasPassword)
        {
          if (newHasCredentials)
          {
            bool ssidChanged = config.wifi.ssid != newSsid;
            bool passwordChanged = hasPassword && config.wifi.password != newPassword;
            if (ssidChanged || passwordChanged || !config.hasWifiCredentials)
            {
              config.wifi.ssid = newSsid;
              config.wifi.password = newPassword;
              config.hasWifiCredentials = true;
              configChanged = true;
              wifiChanged = true;
            }
          }
          else if (config.hasWifiCredentials)
          {
            config.hasWifiCredentials = false;
            config.wifi.ssid = "";
            config.wifi.password = "";
            configChanged = true;
            wifiChanged = true;
          }
        }
      }
    }

    if (configChanged)
    {
      saveDeviceConfig();
    }

    if (uartChanged)
    {
      notifyUartConfigChanged();
    }

    connectOrDisconnectBasedOnConfig(wifiChanged);

    JsonDocument response(128);
    response["status"] = "ok";
    String payload;
    serializeJson(response, payload);
    g_httpServer.send(200, "application/json", payload);
  }

  void handleScanNetworks()
  {
    JsonDocument doc(2048);
    JsonArray networks = doc["networks"].to<JsonArray>();

    int16_t count = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
    for (int16_t i = 0; i < count; ++i)
    {
      JsonObject network = networks.add<JsonObject>();
      network["ssid"] = WiFi.SSID(i);
      network["rssi"] = WiFi.RSSI(i);
      network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();

    String payload;
    serializeJson(doc, payload);
    g_httpServer.send(200, "application/json", payload);
  }

  bool handleCaptivePortalRedirect()
  {
    if (!g_apActive)
    {
      return false;
    }

    String hostHeader = g_httpServer.hostHeader();
    if (hostHeader.isEmpty() || hostHeader.equals(g_apIp.toString()))
    {
      return false;
    }

    String redirectUrl = String("http://") + g_apIp.toString();
    g_httpServer.sendHeader("Location", redirectUrl, true);
    g_httpServer.send(302, "text/plain", "");
    return true;
  }

  void handleCaptivePortal()
  {
    if (!handleCaptivePortalRedirect())
    {
      g_httpServer.send(204, "text/plain", "");
    }
  }

  void handleNotFound()
  {
    if (handleCaptivePortalRedirect())
    {
      return;
    }

    respondError(404, "Not Found");
  }

  void handleIndexHtml()
  {
    g_httpServer.send_P(200, "text/html", INDEX_HTML);
  }

  void handleWebsocketEvent(uint8_t clientId, WStype_t type, uint8_t *payload, size_t length)
  {
    switch (type)
    {
    case WStype_CONNECTED:
    {
      IPAddress ip = g_websocket.remoteIP(clientId);
      Serial.printf("[WS] Client %u connected from %s\n", clientId, ip.toString().c_str());
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client %u disconnected\n", clientId);
      break;
    case WStype_TEXT:
    {
      const DeviceConfig &config = getDeviceConfig();
      if (config.transport != TransportType::Websocket)
      {
        sendTransportDisabled(clientId);
        return;
      }

      if (length == 0)
      {
        return;
      }
      if (length > COMMAND_MESSAGE_MAX_LENGTH)
      {
        sendTooLong(clientId);
        return;
      }
      if (!g_queue)
      {
        return;
      }

      CommandMessage message;
      message.length = length;
      memcpy(message.payload, payload, length);
      message.payload[length] = '\0';
      if (xQueueSend(g_queue, &message, 0) != pdPASS)
      {
        sendQueueFull(clientId);
      }
      break;
    }
    default:
      break;
    }
  }

} // namespace

void websocketTransportBegin(QueueHandle_t queue)
{
  g_queue = queue;

  WiFi.persistent(false);
  startAccessPoint();
  connectToConfiguredNetwork();

  g_websocket.begin();
  g_websocket.onEvent(handleWebsocketEvent);

  g_httpServer.on("/", HTTP_GET, handleIndexHtml);
  g_httpServer.on("/api/config", HTTP_GET, handleConfigGet);
  g_httpServer.on("/api/config", HTTP_POST, handleConfigPost);
  g_httpServer.on("/api/scan", HTTP_GET, handleScanNetworks);
  g_httpServer.on("/generate_204", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/gen_204", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/canonical.html", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/success.txt", HTTP_GET, handleCaptivePortal);
  g_httpServer.onNotFound(handleNotFound);
  g_httpServer.begin();
}

void websocketTransportLoop()
{
  if (g_dnsActive)
  {
    g_dnsServer.processNextRequest();
  }

  g_websocket.loop();
  g_httpServer.handleClient();

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED)
  {
    if (!g_staConnected)
    {
      g_staConnected = true;
      Serial.printf("[WS] Connected to WiFi. IP: %s\n", WiFi.localIP().toString().c_str());
    }
    if (g_apActive)
    {
      stopAccessPoint();
      WiFi.mode(WIFI_STA);
    }
  }
  else
  {
    if (g_staConnected)
    {
      g_staConnected = false;
      Serial.println("[WS] WiFi disconnected");
    }

    const DeviceConfig &config = getDeviceConfig();
    if (config.hasWifiCredentials && (millis() - g_lastConnectionAttempt) > WIFI_RETRY_INTERVAL_MS)
    {
      connectToConfiguredNetwork();
    }
    if (!g_apActive)
    {
      startAccessPoint();
    }
  }
}

void websocketTransportBroadcast(const char *message)
{
  g_websocket.broadcastTXT(message);
}
