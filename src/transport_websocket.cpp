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
    JsonDocument doc;
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
    JsonDocument doc;
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

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    if (error)
    {
      respondError(400, "Invalid JSON payload");
      return;
    }

    const DeviceConfig &currentConfig = getDeviceConfig();
    DeviceConfig updatedConfig = currentConfig;
    const char *errorMessage = nullptr;

    if (doc.containsKey("transport"))
    {
      if (doc["transport"].isNull())
      {
        errorMessage = "Transport option cannot be null";
      }
      else
      {
        TransportType newTransport;
        if (!parseTransportType(doc["transport"].as<String>(), newTransport))
        {
          errorMessage = "Unsupported transport option";
        }
        else
        {
          updatedConfig.transport = newTransport;
        }
      }
    }

    if (!errorMessage && doc.containsKey("uart"))
    {
      if (doc["uart"].isNull() || !doc["uart"].is<JsonObject>())
      {
        errorMessage = "UART settings must be an object";
      }
      else
      {
        JsonObject uart = doc["uart"].as<JsonObject>();
        if (uart.containsKey("baud"))
        {
          if (uart["baud"].isNull())
          {
            errorMessage = "UART baud rate cannot be null";
          }
          else if (!uart["baud"].is<uint32_t>())
          {
            errorMessage = "Invalid UART baud rate";
          }
          else
          {
            uint32_t newBaud = uart["baud"].as<uint32_t>();
            if (!isSupportedUartBaudRate(newBaud))
            {
              errorMessage = "Unsupported UART baud rate";
            }
            else
            {
              updatedConfig.uartBaudRate = newBaud;
            }
          }
        }
      }
    }

    if (!errorMessage && doc.containsKey("wifi"))
    {
      if (doc["wifi"].isNull() || !doc["wifi"].is<JsonObject>())
      {
        errorMessage = "WiFi settings must be an object";
      }
      else
      {
        JsonObject wifi = doc["wifi"].as<JsonObject>();
        if (wifi.containsKey("forget"))
        {
          if (wifi["forget"].isNull())
          {
            errorMessage = "WiFi forget flag cannot be null";
          }
          else if (!wifi["forget"].is<bool>())
          {
            errorMessage = "WiFi forget flag must be a boolean";
          }
        }
        if (!errorMessage)
        {
          bool forget = wifi.containsKey("forget") && wifi["forget"].as<bool>();
          if (forget)
          {
            updatedConfig.hasWifiCredentials = false;
            updatedConfig.wifi.ssid = "";
            updatedConfig.wifi.password = "";
          }
          else
          {
            if (wifi.containsKey("ssid") && wifi["ssid"].isNull())
            {
              errorMessage = "WiFi SSID cannot be null";
            }
            else if (wifi.containsKey("password") && wifi["password"].isNull())
            {
              errorMessage = "WiFi password cannot be null";
            }
            else if (wifi.containsKey("ssid") || wifi.containsKey("password"))
            {
              bool hasSsid = wifi.containsKey("ssid");
              bool hasPassword = wifi.containsKey("password");
              String newSsid = hasSsid ? String(wifi["ssid"].as<const char *>()) : updatedConfig.wifi.ssid;
              String newPassword = hasPassword ? String(wifi["password"].as<const char *>()) : updatedConfig.wifi.password;
              if (newSsid.length() > 0)
              {
                updatedConfig.wifi.ssid = newSsid;
                updatedConfig.wifi.password = newPassword;
                updatedConfig.hasWifiCredentials = true;
              }
              else
              {
                updatedConfig.hasWifiCredentials = false;
                updatedConfig.wifi.ssid = "";
                updatedConfig.wifi.password = "";
              }
            }
          }
        }
      }
    }

    if (errorMessage)
    {
      respondError(400, errorMessage);
      return;
    }

    bool transportChanged = updatedConfig.transport != currentConfig.transport;
    bool uartChanged = updatedConfig.uartBaudRate != currentConfig.uartBaudRate;
    bool wifiChanged =
        updatedConfig.hasWifiCredentials != currentConfig.hasWifiCredentials ||
        updatedConfig.wifi.ssid != currentConfig.wifi.ssid ||
        updatedConfig.wifi.password != currentConfig.wifi.password;

    if (transportChanged || uartChanged || wifiChanged)
    {
      DeviceConfig &mutableConfig = getMutableDeviceConfig();
      mutableConfig = updatedConfig;
      saveDeviceConfig();
    }

    if (uartChanged)
    {
      notifyUartConfigChanged();
    }

    connectOrDisconnectBasedOnConfig(wifiChanged);

    JsonDocument response;
    response["status"] = "ok";
    String payload;
    serializeJson(response, payload);
    g_httpServer.send(200, "application/json", payload);
  }

  void handleScanNetworks()
  {
    JsonDocument doc;
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
