#include "transport_websocket.h"

#include "command_message.h"
#include "device_config.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>

// DON'T include the HTML file yet - use inline HTML for testing
// #include "web/index.html"

namespace
{
  constexpr uint16_t WEBSOCKET_PORT = 81;
  constexpr uint16_t HTTP_PORT = 80;
  constexpr uint8_t DNS_PORT = 53;
  constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
  constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
  constexpr const char *AP_PASSWORD = "uhid1234";

  const IPAddress AP_IP(192, 168, 4, 1);
  const IPAddress AP_GATEWAY(192, 168, 4, 1);
  const IPAddress AP_SUBNET(255, 255, 255, 0);

  QueueHandle_t g_queue = nullptr;
  WebSocketsServer g_websocket(WEBSOCKET_PORT);
  WebServer g_httpServer(HTTP_PORT);
  DNSServer g_dnsServer;

  bool g_apActive = false;
  bool g_dnsActive = false;
  bool g_staConnected = false;
  bool g_staConnecting = false;
  unsigned long g_lastConnectionAttempt = 0;
  String g_apSsid;
  IPAddress g_apIp;

  // Simple test HTML page
  const char TEST_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BLE Bridge Config</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      max-width: 600px;
      margin: 50px auto;
      padding: 20px;
      background: #f0f0f0;
    }
    .container {
      background: white;
      padding: 30px;
      border-radius: 10px;
      box-shadow: 0 2px 10px rgba(0,0,0,0.1);
    }
    h1 { color: #333; margin-top: 0; }
    button {
      background: #007bff;
      color: white;
      border: none;
      padding: 10px 20px;
      border-radius: 5px;
      cursor: pointer;
      margin: 5px;
    }
    button:hover { background: #0056b3; }
    #status { margin: 20px 0; padding: 10px; background: #e9ecef; border-radius: 5px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>🎮 BLE Bridge Configuration</h1>
    <div id="status">
      <strong>Status:</strong> Connected to configuration portal
    </div>
    <h2>WiFi Configuration</h2>
    <label>SSID: <input type="text" id="ssid" placeholder="Your WiFi network"></label><br><br>
    <label>Password: <input type="password" id="password" placeholder="WiFi password"></label><br><br>
    <button onclick="saveWiFi()">Save WiFi</button>
    <button onclick="scanNetworks()">Scan Networks</button>
    <div id="networks"></div>
    <h2>Transport Mode</h2>
    <label><input type="radio" name="transport" value="websocket" checked> WebSocket</label><br>
    <label><input type="radio" name="transport" value="uart"> UART</label><br><br>
    <button onclick="saveConfig()">Save Settings</button>
    <h2>Test</h2>
    <button onclick="testConnection()">Test Connection</button>
    <div id="result"></div>
  </div>
  <script>
    function testConnection() {
      document.getElementById('result').innerHTML = '<p style="color:green">✓ Web interface is working!</p>';
    }
    
    async function saveWiFi() {
      const ssid = document.getElementById('ssid').value;
      const password = document.getElementById('password').value;
      
      if (!ssid) {
        alert('Please enter WiFi SSID');
        return;
      }
      
      try {
        const response = await fetch('/api/config', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ wifi: { ssid, password } })
        });
        
        const data = await response.json();
        document.getElementById('result').innerHTML = 
          '<p style="color:green">✓ WiFi credentials saved! Connecting...</p>';
      } catch (err) {
        document.getElementById('result').innerHTML = 
          '<p style="color:red">✗ Error: ' + err.message + '</p>';
      }
    }
    
    async function saveConfig() {
      const transport = document.querySelector('input[name="transport"]:checked').value;
      
      try {
        const response = await fetch('/api/config', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ transport })
        });
        
        const data = await response.json();
        document.getElementById('result').innerHTML = 
          '<p style="color:green">✓ Transport set to: ' + transport + '</p>';
      } catch (err) {
        document.getElementById('result').innerHTML = 
          '<p style="color:red">✗ Error: ' + err.message + '</p>';
      }
    }
    
    async function scanNetworks() {
      document.getElementById('networks').innerHTML = '<p>Scanning...</p>';
      
      try {
        const response = await fetch('/api/scan');
        const data = await response.json();
        
        let html = '<h3>Available Networks:</h3><ul>';
        data.networks.forEach(net => {
          html += `<li><strong>${net.ssid}</strong> (${net.rssi} dBm) ${net.secure ? '🔒' : ''}</li>`;
        });
        html += '</ul>';
        
        document.getElementById('networks').innerHTML = html;
      } catch (err) {
        document.getElementById('networks').innerHTML = 
          '<p style="color:red">Scan failed: ' + err.message + '</p>';
      }
    }
    
    // Auto-load config on page load
    window.onload = async function() {
      try {
        const response = await fetch('/api/config');
        const data = await response.json();
        document.getElementById('status').innerHTML = 
          '<strong>Status:</strong> Connected | Transport: ' + data.transport;
      } catch (err) {
        console.error('Failed to load config:', err);
      }
    };
  </script>
</body>
</html>
)rawliteral";

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
    uint32_t chipId = 0;
    for (int i = 0; i < 17; i = i + 8)
    {
      chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
    }

    char buffer[24];
    snprintf(buffer, sizeof(buffer), "ble-bridge-%06X", chipId);
    return String(buffer);
  }

  void startAccessPoint()
  {
    if (g_apActive)
    {
      Serial.println(F("[WiFi] AP already running"));
      return;
    }

    if (g_apSsid.isEmpty())
    {
      g_apSsid = generateApSsid();
    }

    Serial.println(F("[WiFi] =========================================="));
    Serial.println(F("[WiFi] Starting Configuration Access Point"));
    Serial.println(F("[WiFi] =========================================="));

    // Configure static IP for AP
    if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET))
    {
      Serial.println(F("[WiFi] WARNING: Failed to configure AP IP"));
    }

    // Start Access Point
    bool apStarted = WiFi.softAP(g_apSsid.c_str(), AP_PASSWORD, 1, 0, 4);

    if (!apStarted)
    {
      Serial.println(F("[WiFi] ERROR: Failed to start Access Point"));
      return;
    }

    delay(500);

    g_apIp = WiFi.softAPIP();
    g_apActive = true;

    Serial.println(F("[WiFi] ------------------------------------------"));
    Serial.printf("[WiFi] SSID:     %s\n", g_apSsid.c_str());
    Serial.printf("[WiFi] Password: %s\n", AP_PASSWORD);
    Serial.printf("[WiFi] IP:       %s\n", g_apIp.toString().c_str());
    Serial.printf("[WiFi] URL:      http://%s/\n", g_apIp.toString().c_str());
    Serial.println(F("[WiFi] ------------------------------------------"));

    // Start captive portal DNS - AFTER AP is running
    Serial.println(F("[DNS] Starting captive portal DNS..."));
    if (g_dnsServer.start(DNS_PORT, "*", g_apIp))
    {
      g_dnsActive = true;
      Serial.println(F("[DNS] Captive portal DNS active"));
    }
    else
    {
      Serial.println(F("[DNS] WARNING: DNS server failed to start"));
    }
  }

  void stopAccessPoint()
  {
    if (!g_apActive)
    {
      return;
    }

    Serial.println(F("[WiFi] Stopping Access Point"));

    if (g_dnsActive)
    {
      g_dnsServer.stop();
      g_dnsActive = false;
    }

    WiFi.softAPdisconnect(true);
    g_apActive = false;

    Serial.println(F("[WiFi] Access Point stopped"));
  }

  void connectToConfiguredNetwork()
  {
    const DeviceConfig &config = getDeviceConfig();

    if (!config.hasWifiCredentials || config.wifi.ssid.isEmpty())
    {
      Serial.println(F("[WiFi] No WiFi credentials configured"));
      return;
    }

    if (g_staConnecting)
    {
      Serial.println(F("[WiFi] Connection attempt already in progress"));
      return;
    }

    Serial.println(F("[WiFi] ------------------------------------------"));
    Serial.printf("[WiFi] Connecting to: %s\n", config.wifi.ssid.c_str());
    Serial.println(F("[WiFi] ------------------------------------------"));

    WiFi.mode(WIFI_AP_STA);
    delay(100);

    WiFi.disconnect(false, false);
    delay(100);

    WiFi.setAutoReconnect(true);
    WiFi.setAutoConnect(false);
    WiFi.setHostname("ble-bridge");

    WiFi.begin(config.wifi.ssid.c_str(), config.wifi.password.c_str());

    g_lastConnectionAttempt = millis();
    g_staConnecting = true;
    g_staConnected = false;
  }

  void handleConfigGet()
  {
    Serial.println(F("[HTTP] GET /api/config"));

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
    wifi["apSsid"] = g_apActive ? g_apSsid : "";
    wifi["apPassword"] = g_apActive ? String(AP_PASSWORD) : "";
    wifi["portalUrl"] = g_apActive ? String("http://") + g_apIp.toString() + "/" : "";

    String response;
    serializeJson(doc, response);

    g_httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    g_httpServer.send(200, "application/json", response);

    Serial.printf("[HTTP] Sent config: %d bytes\n", response.length());
  }

  void respondError(uint16_t code, const char *message)
  {
    Serial.printf("[HTTP] Error %d: %s\n", code, message);

    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = message;
    String payload;
    serializeJson(doc, payload);

    g_httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    g_httpServer.send(code, "application/json", payload);
  }

  void handleConfigPost()
  {
    Serial.println(F("[HTTP] POST /api/config"));

    if (g_httpServer.method() != HTTP_POST)
    {
      respondError(405, "Method Not Allowed");
      return;
    }

    const String body = g_httpServer.arg("plain");
    Serial.printf("[HTTP] Body: %s\n", body.c_str());

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

    DeviceConfig &config = getMutableDeviceConfig();
    bool configChanged = false;
    bool wifiChanged = false;
    bool uartChanged = false;

    // Handle transport change
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
        Serial.printf("[CFG] Transport changed to: %s\n", transportTypeToString(newTransport));
      }
    }

    // Handle UART config
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
          Serial.printf("[CFG] UART baud changed to: %lu\n", newBaud);
        }
      }
    }

    // Handle WiFi config
    if (!doc["wifi"].isNull())
    {
      JsonObject wifi = doc["wifi"].as<JsonObject>();
      bool forget = !wifi["forget"].isNull() && wifi["forget"].as<bool>();

      if (forget)
      {
        if (config.hasWifiCredentials || !config.wifi.ssid.isEmpty())
        {
          Serial.println(F("[WiFi] Forgetting stored credentials"));
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
              Serial.printf("[WiFi] Saving credentials for: %s\n", newSsid.c_str());
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

    if (wifiChanged)
    {
      if (config.hasWifiCredentials && !config.wifi.ssid.isEmpty())
      {
        WiFi.disconnect(true, true);
        delay(100);
        connectToConfiguredNetwork();
      }
      else
      {
        WiFi.disconnect(true, true);
        g_staConnected = false;
        g_staConnecting = false;

        if (!g_apActive)
        {
          startAccessPoint();
        }
      }
    }

    JsonDocument response;
    response["status"] = "ok";
    String payload;
    serializeJson(response, payload);

    g_httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    g_httpServer.send(200, "application/json", payload);

    Serial.println(F("[HTTP] Config saved successfully"));
  }

  void handleScanNetworks()
  {
    Serial.println(F("[WiFi] Scanning for networks..."));

    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    int16_t count = WiFi.scanNetworks(false, true, false, 300);

    Serial.printf("[WiFi] Found %d networks\n", count);

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

    g_httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    g_httpServer.send(200, "application/json", payload);
  }

  bool handleCaptivePortalRedirect()
  {
    if (!g_apActive)
    {
      return false;
    }

    String hostHeader = g_httpServer.hostHeader();

    // If host matches our IP or is empty, don't redirect
    if (hostHeader.isEmpty() || hostHeader.equals(g_apIp.toString()) ||
        hostHeader.startsWith("192.168.4.1"))
    {
      return false;
    }

    // Redirect to captive portal
    Serial.printf("[HTTP] Captive redirect from: %s\n", hostHeader.c_str());
    String redirectUrl = String("http://") + g_apIp.toString() + "/";
    g_httpServer.sendHeader("Location", redirectUrl, true);
    g_httpServer.send(302, "text/plain", "Redirecting to captive portal");
    return true;
  }

  void handleCaptivePortal()
  {
    Serial.println(F("[HTTP] Captive portal check"));
    if (!handleCaptivePortalRedirect())
    {
      g_httpServer.send(204, "text/plain", "");
    }
  }

  void handleNotFound()
  {
    Serial.printf("[HTTP] 404: %s\n", g_httpServer.uri().c_str());

    if (handleCaptivePortalRedirect())
    {
      return;
    }

    respondError(404, "Not Found");
  }

  void handleIndexHtml()
  {
    Serial.println(F("[HTTP] GET / - Serving index page"));
    g_httpServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    g_httpServer.sendHeader("Pragma", "no-cache");
    g_httpServer.sendHeader("Expires", "-1");
    g_httpServer.send_P(200, "text/html", TEST_HTML);
    Serial.println(F("[HTTP] Index page sent"));
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

  Serial.println(F("[WiFi] =========================================="));
  Serial.println(F("[WiFi] CRITICAL: Initializing WiFi BEFORE BLE"));
  Serial.println(F("[WiFi] =========================================="));

  WiFi.persistent(false);

  Serial.println(F("[WiFi] Setting AP+STA mode..."));
  WiFi.mode(WIFI_AP_STA);
  delay(500);

  Serial.println(F("[WiFi] WiFi mode set successfully"));

  // Start AP
  startAccessPoint();
  delay(1000);

  // Try to connect to saved network
  const DeviceConfig &config = getDeviceConfig();
  if (config.hasWifiCredentials && !config.wifi.ssid.isEmpty())
  {
    Serial.println(F("[WiFi] Stored credentials found - connecting..."));
    delay(500);
    connectToConfiguredNetwork();
    delay(1000);
  }

  // Start WebSocket server
  Serial.println(F("[WS] Starting WebSocket server..."));
  g_websocket.begin();
  g_websocket.onEvent(handleWebsocketEvent);
  Serial.printf("[WS] WebSocket listening on port %d\n", WEBSOCKET_PORT);

  // Start HTTP server
  Serial.println(F("[HTTP] Starting HTTP server..."));

  // Root page
  g_httpServer.on("/", HTTP_GET, handleIndexHtml);

  // API endpoints
  g_httpServer.on("/api/config", HTTP_GET, handleConfigGet);
  g_httpServer.on("/api/config", HTTP_POST, handleConfigPost);
  g_httpServer.on("/api/scan", HTTP_GET, handleScanNetworks);

  // Captive portal endpoints
  g_httpServer.on("/generate_204", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/gen_204", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/canonical.html", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/success.txt", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/ncsi.txt", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
  g_httpServer.on("/redirect", HTTP_GET, handleCaptivePortal);

  g_httpServer.onNotFound(handleNotFound);

  g_httpServer.begin();
  Serial.printf("[HTTP] HTTP server listening on port %d\n", HTTP_PORT);
  Serial.println(F("[HTTP] Server started - test with: http://192.168.4.1/"));

  Serial.println(F("[WiFi] =========================================="));
  Serial.println(F("[WiFi] WiFi initialization COMPLETE"));
  Serial.println(F("[WiFi] BLE can now safely initialize"));
  Serial.println(F("[WiFi] ==========================================\n"));
}

void websocketTransportLoop()
{
  // Process DNS for captive portal
  if (g_dnsActive)
  {
    g_dnsServer.processNextRequest();
  }

  // Handle WebSocket and HTTP clients
  g_websocket.loop();
  g_httpServer.handleClient();

  // Monitor WiFi STA connection
  wl_status_t status = WiFi.status();

  if (g_staConnecting)
  {
    if (status == WL_CONNECTED)
    {
      g_staConnecting = false;
      g_staConnected = true;

      Serial.println(F("[WiFi] =========================================="));
      Serial.printf("[WiFi] Connected to: %s\n", WiFi.SSID().c_str());
      Serial.printf("[WiFi] IP Address:   %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("[WiFi] URL:          http://%s/\n", WiFi.localIP().toString().c_str());
      Serial.println(F("[WiFi] ==========================================\n"));
    }
    else if (millis() - g_lastConnectionAttempt > WIFI_CONNECT_TIMEOUT_MS)
    {
      Serial.println(F("[WiFi] Connection timeout"));
      g_staConnecting = false;
      g_staConnected = false;
    }
  }
  else if (g_staConnected && status != WL_CONNECTED)
  {
    Serial.println(F("[WiFi] Connection lost"));
    g_staConnected = false;

    if (!g_apActive)
    {
      startAccessPoint();
    }
  }
  else if (!g_staConnected && !g_staConnecting)
  {
    const DeviceConfig &config = getDeviceConfig();

    if (config.hasWifiCredentials &&
        (millis() - g_lastConnectionAttempt) > WIFI_RETRY_INTERVAL_MS)
    {
      Serial.println(F("[WiFi] Retrying connection..."));
      connectToConfiguredNetwork();
    }
  }

  if (!g_staConnected && !g_apActive)
  {
    Serial.println(F("[WiFi] No STA connection - ensuring AP is active"));
    startAccessPoint();
  }
}

void websocketTransportBroadcast(const char *message)
{
  g_websocket.broadcastTXT(message);
}