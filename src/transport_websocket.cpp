#include "transport_websocket.h"

#include "command_message.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <cstring>

#include "web/index.html"
#include "device_config.h"
#include "wifi_manager.h"

namespace
{
  constexpr uint16_t WEBSOCKET_PORT = 81;
  constexpr uint16_t HTTP_PORT = 80;

  QueueHandle_t g_queue = nullptr;
  WebSocketsServer g_websocket(WEBSOCKET_PORT);
  WebServer g_httpServer(HTTP_PORT);
  bool g_running = false;
  bool g_handlersRegistered = false;

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
      {
        CommandMessage message;
        message.length = length;
        memcpy(message.payload, payload, length);
        message.payload[length] = '\0';
        if (xQueueSend(g_queue, &message, 0) != pdPASS)
        {
          sendQueueFull(clientId);
        }
      }
      break;
    default:
      break;
    }
  }

  void handleIndexHtml()
  {
    g_httpServer.send_P(200, "text/html", INDEX_HTML);
  }

  void sendCorsHeaders()
  {
    g_httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    g_httpServer.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    g_httpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  }

  void sendJsonDocument(int statusCode, const JsonDocument &document)
  {
    String body;
    serializeJson(document, body);
    sendCorsHeaders();
    g_httpServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    g_httpServer.send(statusCode, "application/json", body);
  }

  void sendErrorResponse(int statusCode, const char *message)
  {
    StaticJsonDocument<128> doc;
    doc["status"] = "error";
    doc["message"] = message;
    sendJsonDocument(statusCode, doc);
  }

  void sendConfigResponse(const DeviceConfig &config)
  {
    StaticJsonDocument<128> doc;
    doc["transport"] = deviceConfigTransportToString(config.transport);
    doc["uartBaud"] = config.uartBaud;
    sendJsonDocument(200, doc);
  }

  void handleConfigGet()
  {
    DeviceConfig config = deviceConfigGet();
    sendConfigResponse(config);
  }

  void handleConfigPost()
  {
    String payload = g_httpServer.arg("plain");
    if (!payload.length())
    {
      sendErrorResponse(400, "Request body required");
      return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
    {
      sendErrorResponse(400, "Invalid JSON payload");
      return;
    }

    DeviceConfig nextConfig = deviceConfigGet();

    if (doc.containsKey("transport"))
    {
      const char *transportValue = doc["transport"].as<const char *>();
      if (!transportValue)
      {
        sendErrorResponse(400, "transport must be a string");
        return;
      }

      TransportType transportType;
      if (!deviceConfigParseTransport(String(transportValue), transportType))
      {
        sendErrorResponse(400, "transport must be 'websocket' or 'uart'");
        return;
      }

      nextConfig.transport = transportType;
    }

    if (doc.containsKey("uartBaud"))
    {
      long baud = doc["uartBaud"].as<long>();
      if (baud <= 0)
      {
        sendErrorResponse(400, "uartBaud must be a positive integer");
        return;
      }
      nextConfig.uartBaud = static_cast<uint32_t>(baud);
    }

    if (!deviceConfigSave(nextConfig))
    {
      sendErrorResponse(500, "Failed to store configuration");
      return;
    }

    deviceConfigSet(nextConfig);
    sendConfigResponse(nextConfig);
  }

  void handleConfigOptions()
  {
    sendCorsHeaders();
    g_httpServer.send(204, "text/plain", "");
  }

  void sendWifiStatusResponse()
  {
    WifiManagerStatus status = wifiManagerGetStatus();
    StaticJsonDocument<256> doc;
    doc["hasCredentials"] = status.hasCredentials;
    doc["connected"] = status.connected;
    if (!status.connectedSsid.isEmpty())
    {
      doc["ssid"] = status.connectedSsid;
    }
    if (status.connected)
    {
      doc["localIp"] = status.stationIp.toString();
    }
    doc["accessPointActive"] = status.accessPointActive;
    doc["accessPointSsid"] = wifiManagerAccessPointSsid();
    if (status.accessPointActive)
    {
      doc["accessPointIp"] = status.accessPointIp.toString();
    }
    sendJsonDocument(200, doc);
  }

  void handleWifiGet()
  {
    sendWifiStatusResponse();
  }

  void handleWifiPost()
  {
    if (!g_httpServer.hasArg("plain"))
    {
      sendErrorResponse(400, "Missing request body");
      return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, g_httpServer.arg("plain"));
    if (err)
    {
      sendErrorResponse(400, "Invalid JSON payload");
      return;
    }

    if (doc.containsKey("forget") && doc["forget"].as<bool>())
    {
      if (!wifiManagerForgetCredentials())
      {
        sendErrorResponse(500, "Failed to clear stored credentials");
        return;
      }
      sendWifiStatusResponse();
      return;
    }

    if (!doc.containsKey("ssid"))
    {
      sendErrorResponse(400, "ssid is required");
      return;
    }

    const char *ssidValue = doc["ssid"].as<const char *>();
    if (!ssidValue)
    {
      sendErrorResponse(400, "ssid must be a string");
      return;
    }

    String password;
    if (doc.containsKey("password") && !doc["password"].isNull())
    {
      if (!doc["password"].is<const char *>())
      {
        sendErrorResponse(400, "password must be a string");
        return;
      }
      const char *passwordValue = doc["password"].as<const char *>();
      if (passwordValue)
      {
        password = String(passwordValue);
      }
    }

    if (!wifiManagerSetCredentials(String(ssidValue), password))
    {
      sendErrorResponse(400, "ssid must not be empty");
      return;
    }

    sendWifiStatusResponse();
  }

  void handleWifiOptions()
  {
    sendCorsHeaders();
    g_httpServer.send(204, "text/plain", "");
  }

} // namespace

void websocketTransportBegin(QueueHandle_t queue)
{
  g_queue = queue;

  if (g_running)
  {
    return;
  }

  WifiManagerStatus wifiStatus = wifiManagerGetStatus();
  if (!wifiStatus.connected)
  {
    wifiManagerEnsureAccessPoint();
    wifiStatus = wifiManagerGetStatus();
  }

  if (wifiStatus.connected)
  {
    Serial.printf("[WS] Station IP Address: %s\n", wifiStatus.stationIp.toString().c_str());
  }

  if (wifiStatus.accessPointActive)
  {
    Serial.printf("[WS] Access Point SSID: %s\n", wifiManagerAccessPointSsid());
    Serial.printf("[WS] AP IP Address: %s\n", wifiStatus.accessPointIp.toString().c_str());
  }

  g_websocket.begin();
  g_websocket.onEvent(handleWebsocketEvent);

  if (!g_handlersRegistered)
  {
    g_httpServer.on("/", handleIndexHtml);
    g_httpServer.on("/api/config", HTTP_GET, handleConfigGet);
    g_httpServer.on("/api/config", HTTP_POST, handleConfigPost);
    g_httpServer.on("/api/config", HTTP_OPTIONS, handleConfigOptions);
    g_httpServer.on("/api/wifi", HTTP_GET, handleWifiGet);
    g_httpServer.on("/api/wifi", HTTP_POST, handleWifiPost);
    g_httpServer.on("/api/wifi", HTTP_OPTIONS, handleWifiOptions);
    g_handlersRegistered = true;
  }

  g_httpServer.begin();
  g_running = true;
}

void websocketTransportLoop()
{
  if (!g_running)
  {
    return;
  }

  g_websocket.loop();
  g_httpServer.handleClient();
}

void websocketTransportBroadcast(const char *message)
{
  if (!g_running)
  {
    return;
  }

  g_websocket.broadcastTXT(message);
}

void websocketTransportEnd()
{
  if (!g_running)
  {
    return;
  }

  g_websocket.close();
  g_httpServer.stop();
  g_running = false;
}
