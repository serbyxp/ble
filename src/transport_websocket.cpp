#include "transport_websocket.h"

#ifdef ENABLE_WEBSOCKET_TRANSPORT

#include "command_message.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <cstring>

#include "web/index.html"

namespace
{
  constexpr uint16_t WEBSOCKET_PORT = 81;
  constexpr uint16_t HTTP_PORT = 80;

  QueueHandle_t g_queue = nullptr;
  WebSocketsServer g_websocket(WEBSOCKET_PORT);
  WebServer g_httpServer(HTTP_PORT);

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

} // namespace

void websocketTransportBegin(QueueHandle_t queue)
{
  g_queue = queue;

  WiFi.mode(WIFI_AP);
  WiFi.softAP("ble-hid", "uhid1234");
  IPAddress apIp = WiFi.softAPIP();
  Serial.printf("[WS] Access Point SSID: %s\n", WiFi.softAPSSID().c_str());
  Serial.printf("[WS] AP IP Address: %s\n", apIp.toString().c_str());

  g_websocket.begin();
  g_websocket.onEvent(handleWebsocketEvent);

  g_httpServer.on("/", handleIndexHtml);
  g_httpServer.begin();
}

void websocketTransportLoop()
{
  g_websocket.loop();
  g_httpServer.handleClient();
}

void websocketTransportBroadcast(const char *message)
{
  g_websocket.broadcastTXT(message);
}

#endif
