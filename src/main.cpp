#include <Arduino.h>
#include <BleCombo.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#if __has_include("sdkconfig.h")
#include <sdkconfig.h>
#endif
#include <WiFi.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <pgmspace.h>
#include <stdlib.h>
#include <strings.h>
#include <cstring>
#include <string>
#include <atomic>
#include <algorithm>
#include <vector>

namespace
{
constexpr size_t JSON_DOC_CAPACITY = 512;
constexpr size_t INPUT_BUFFER_LIMIT = 512;
constexpr size_t MAX_KEY_COMBO = 8;
constexpr uint8_t MOUSE_ALL_BUTTONS = MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE | MOUSE_BACK | MOUSE_FORWARD;
constexpr uint16_t DEFAULT_CHAR_DELAY_MS = 6;
constexpr const char *NVS_NAMESPACE_WIFI = "wifi";
constexpr const char *NVS_KEY_SSID = "ssid";
constexpr const char *NVS_KEY_PASS = "password";
constexpr const char *NVS_NAMESPACE_TRANSPORT = "transport";
constexpr const char *NVS_KEY_TRANSPORT_MODE = "mode";
constexpr const char *NVS_KEY_UART_BAUD = "baud";
constexpr uint32_t DEFAULT_UART_BAUD = 115200;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr unsigned long WIFI_RETRY_DELAY_MS = 500;
constexpr unsigned long WIFI_AP_SHUTDOWN_DELAY_MS = 3000;
constexpr const char *CONFIG_AP_SSID = "uhid-setup";
constexpr const char *CONFIG_AP_PASSWORD = "uhid1234";
constexpr uint16_t DNS_PORT = 53;
constexpr uint16_t HTTP_PORT = 80;

DNSServer dnsServer;
bool dnsServerActive = false;

#if defined(CONFIG_BT_NIMBLE_PINNED_TO_CORE)
constexpr BaseType_t kHttpTaskCore = (CONFIG_BT_NIMBLE_PINNED_TO_CORE == 0) ? 1 : 0;
#elif defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
constexpr BaseType_t kHttpTaskCore = tskNO_AFFINITY;
#elif defined(CONFIG_ARDUINO_RUNNING_CORE)
constexpr BaseType_t kHttpTaskCore = (CONFIG_ARDUINO_RUNNING_CORE == 0) ? 1 : 0;
#else
constexpr BaseType_t kHttpTaskCore = 0;
#endif

struct TransportMessage
{
  size_t length;
  char payload[INPUT_BUFFER_LIMIT];
};

constexpr UBaseType_t TRANSPORT_COMMAND_QUEUE_LENGTH = 8;
constexpr UBaseType_t TRANSPORT_EVENT_QUEUE_LENGTH = 8;

enum class TransportMode : uint8_t
{
  Uart = 0,
  Websocket = 1
};

std::atomic<TransportMode> activeTransportMode{TransportMode::Uart};
uint32_t uartBaudRate = DEFAULT_UART_BAUD;
bool serialActive = false;

esp_err_t handleWebSocket(httpd_req_t *req);
void httpServerTask(void *param);
void startHttpServerTask();
void transportPumpTask(void *param);
void startTransportPumpTask();
bool ensureTransportQueues();
void closeActiveWebsocket();
void resetTransportQueues();
bool applyTransportMode(TransportMode mode);
bool saveTransportConfig(TransportMode mode, uint32_t baud);
TransportMode loadTransportModeFromStorage(uint32_t &baudOut);
const char *transportModeToString(TransportMode mode);
TransportMode stringToTransportMode(const char *value);
esp_err_t handleTransportGet(httpd_req_t *req);
esp_err_t handleTransportPost(httpd_req_t *req);
void sendEvent(const char *name, const char *detail = nullptr);
void applyUartBaudRate(uint32_t baud);
void flushInputBuffer();
void processCommand(const String &payload);

QueueHandle_t transportCommandQueue = nullptr;
QueueHandle_t transportEventQueue = nullptr;
TaskHandle_t httpServerTaskHandle = nullptr;
TaskHandle_t transportPumpTaskHandle = nullptr;
httpd_handle_t httpServerHandle = nullptr;
volatile int wsClientSocket = -1;

bool enqueueTransportMessage(QueueHandle_t queue, const char *data, size_t length)
{
  if (!queue || !data)
  {
    return false;
  }

  TransportMessage message = {};
  if (length >= sizeof(message.payload))
  {
    return false;
  }

  message.length = length;
  if (length > 0)
  {
    memcpy(message.payload, data, length);
  }
  message.payload[length] = '\0';
  return xQueueSend(queue, &message, 0) == pdPASS;
}

void dispatchTransportJson(const char *payload)
{
  if (!payload)
  {
    return;
  }

  if (activeTransportMode.load() == TransportMode::Websocket)
  {
    if (!ensureTransportQueues())
    {
      return;
    }
    if (!enqueueTransportMessage(transportEventQueue, payload, strlen(payload)))
    {
      // Drop message if the queue is full.
    }
    return;
  }

  if (serialActive)
  {
    Serial.println(payload);
  }
}

void dispatchTransportJson(const String &payload)
{
  dispatchTransportJson(payload.c_str());
}

bool ensureTransportQueues()
{
  if (!transportCommandQueue)
  {
    transportCommandQueue = xQueueCreate(TRANSPORT_COMMAND_QUEUE_LENGTH, sizeof(TransportMessage));
  }
  if (!transportEventQueue)
  {
    transportEventQueue = xQueueCreate(TRANSPORT_EVENT_QUEUE_LENGTH, sizeof(TransportMessage));
  }
  return transportCommandQueue != nullptr && transportEventQueue != nullptr;
}

void closeActiveWebsocket()
{
  if (httpServerHandle && wsClientSocket >= 0)
  {
    httpd_sess_trigger_close(httpServerHandle, wsClientSocket);
  }
  wsClientSocket = -1;
}

void resetTransportQueues()
{
  if (transportCommandQueue)
  {
    xQueueReset(transportCommandQueue);
  }
  if (transportEventQueue)
  {
    xQueueReset(transportEventQueue);
  }
}

const char *transportModeToString(TransportMode mode)
{
  switch (mode)
  {
  case TransportMode::Websocket:
    return "websocket";
  case TransportMode::Uart:
  default:
    return "uart";
  }
}

TransportMode stringToTransportMode(const char *value)
{
  if (!value)
  {
    return TransportMode::Uart;
  }

  if (strcasecmp(value, "websocket") == 0)
  {
    return TransportMode::Websocket;
  }
  return TransportMode::Uart;
}

bool applyTransportMode(TransportMode mode)
{
  TransportMode previous = activeTransportMode.load();

  if (mode == TransportMode::Websocket)
  {
    if (!ensureTransportQueues())
    {
      return false;
    }
    activeTransportMode.store(TransportMode::Websocket);
    if (serialActive)
    {
      Serial.flush();
      Serial.end();
      serialActive = false;
    }
  }
  else
  {
    if (!serialActive)
    {
      Serial.begin(uartBaudRate);
      Serial.setTimeout(0);
      serialActive = true;
    }
    activeTransportMode.store(TransportMode::Uart);
    closeActiveWebsocket();
    resetTransportQueues();
  }

  if (previous != mode)
  {
    sendEvent("transport_mode", transportModeToString(mode));
  }

  return true;
}

bool saveTransportConfig(TransportMode mode, uint32_t baud)
{
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE_TRANSPORT, NVS_READWRITE, &handle);
  if (err != ESP_OK)
  {
    return false;
  }

  err = nvs_set_u8(handle, NVS_KEY_TRANSPORT_MODE, static_cast<uint8_t>(mode));
  if (err == ESP_OK)
  {
    err = nvs_set_u32(handle, NVS_KEY_UART_BAUD, baud);
  }
  if (err == ESP_OK)
  {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err == ESP_OK;
}

TransportMode loadTransportModeFromStorage(uint32_t &baudOut)
{
  baudOut = DEFAULT_UART_BAUD;
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE_TRANSPORT, NVS_READONLY, &handle);
  if (err != ESP_OK)
  {
    return TransportMode::Uart;
  }

  uint8_t modeValue = static_cast<uint8_t>(TransportMode::Uart);
  err = nvs_get_u8(handle, NVS_KEY_TRANSPORT_MODE, &modeValue);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
  {
    nvs_close(handle);
    return TransportMode::Uart;
  }

  if (modeValue > static_cast<uint8_t>(TransportMode::Websocket))
  {
    modeValue = static_cast<uint8_t>(TransportMode::Uart);
  }

  uint32_t baudValue = DEFAULT_UART_BAUD;
  esp_err_t baudErr = nvs_get_u32(handle, NVS_KEY_UART_BAUD, &baudValue);
  if (baudErr == ESP_OK && baudValue >= 9600 && baudValue <= 921600)
  {
    baudOut = baudValue;
  }

  nvs_close(handle);
  return static_cast<TransportMode>(modeValue);
}

void applyUartBaudRate(uint32_t baud)
{
  if (baud < 9600 || baud > 921600)
  {
    baud = DEFAULT_UART_BAUD;
  }
  uartBaudRate = baud;
  if (serialActive)
  {
    Serial.flush();
    Serial.end();
    Serial.begin(uartBaudRate);
    Serial.setTimeout(0);
  }
}
extern const uint8_t src_main_web_index_html_start[] asm("_binary_src_main_web_index_html_start");
extern const uint8_t src_main_web_index_html_end[] asm("_binary_src_main_web_index_html_end");

constexpr size_t CAPTIVE_PORTAL_INDEX_HTML_LENGTH = static_cast<size_t>(
    src_main_web_index_html_end - src_main_web_index_html_start);


bool connectStationAndPersist(const String &ssid, const String &password, bool keepApActive);
void startAccessPoint();

String inputBuffer;
bool lastBleConnectionState = false;
bool configurationMode = false;

enum class WifiState : uint8_t
{
  ApOnly,
  ApSta,
  TransitioningToSta,
  StaOnly
};

WifiState wifiState = WifiState::ApOnly;
wifi_mode_t wifiCurrentMode = WIFI_MODE_NULL;
wifi_mode_t wifiTargetMode = WIFI_MODE_AP;
bool temporaryApStaMode = false;
bool apShutdownPending = false;
unsigned long apShutdownDeadline = 0;
bool staConnectInProgress = false;

struct NamedCode
{
  const char *name;
  uint8_t code;
};

const NamedCode KEY_NAME_MAP[] = {
    {"KEY_RETURN", KEY_RETURN},
    {"RETURN", KEY_RETURN},
    {"ENTER", KEY_RETURN},
    {"KEY_ESC", KEY_ESC},
    {"ESC", KEY_ESC},
    {"ESCAPE", KEY_ESC},
    {"KEY_BACKSPACE", KEY_BACKSPACE},
    {"BACKSPACE", KEY_BACKSPACE},
    {"KEY_TAB", KEY_TAB},
    {"TAB", KEY_TAB},
    {"KEY_DELETE", KEY_DELETE},
    {"DELETE", KEY_DELETE},
    {"DEL", KEY_DELETE},
    {"KEY_INSERT", KEY_INSERT},
    {"INSERT", KEY_INSERT},
    {"INS", KEY_INSERT},
    {"KEY_PAGE_UP", KEY_PAGE_UP},
    {"PAGE_UP", KEY_PAGE_UP},
    {"PGUP", KEY_PAGE_UP},
    {"KEY_PAGE_DOWN", KEY_PAGE_DOWN},
    {"PAGE_DOWN", KEY_PAGE_DOWN},
    {"PGDN", KEY_PAGE_DOWN},
    {"KEY_HOME", KEY_HOME},
    {"HOME", KEY_HOME},
    {"KEY_END", KEY_END},
    {"END", KEY_END},
    {"KEY_RIGHT_ARROW", KEY_RIGHT_ARROW},
    {"RIGHT", KEY_RIGHT_ARROW},
    {"KEY_LEFT_ARROW", KEY_LEFT_ARROW},
    {"LEFT", KEY_LEFT_ARROW},
    {"KEY_UP_ARROW", KEY_UP_ARROW},
    {"UP", KEY_UP_ARROW},
    {"KEY_DOWN_ARROW", KEY_DOWN_ARROW},
    {"DOWN", KEY_DOWN_ARROW},
    {"KEY_CAPS_LOCK", KEY_CAPS_LOCK},
    {"CAPS_LOCK", KEY_CAPS_LOCK},
    {"CAPSLOCK", KEY_CAPS_LOCK},
    {"KEY_LEFT_CTRL", KEY_LEFT_CTRL},
    {"LEFT_CTRL", KEY_LEFT_CTRL},
    {"CTRL", KEY_LEFT_CTRL},
    {"CONTROL", KEY_LEFT_CTRL},
    {"KEY_RIGHT_CTRL", KEY_RIGHT_CTRL},
    {"RIGHT_CTRL", KEY_RIGHT_CTRL},
    {"KEY_LEFT_SHIFT", KEY_LEFT_SHIFT},
    {"LEFT_SHIFT", KEY_LEFT_SHIFT},
    {"SHIFT", KEY_LEFT_SHIFT},
    {"KEY_RIGHT_SHIFT", KEY_RIGHT_SHIFT},
    {"RIGHT_SHIFT", KEY_RIGHT_SHIFT},
    {"KEY_LEFT_ALT", KEY_LEFT_ALT},
    {"LEFT_ALT", KEY_LEFT_ALT},
    {"ALT", KEY_LEFT_ALT},
    {"KEY_RIGHT_ALT", KEY_RIGHT_ALT},
    {"RIGHT_ALT", KEY_RIGHT_ALT},
    {"ALTGR", KEY_RIGHT_ALT},
    {"KEY_LEFT_GUI", KEY_LEFT_GUI},
    {"LEFT_GUI", KEY_LEFT_GUI},
    {"LGUI", KEY_LEFT_GUI},
    {"GUI", KEY_LEFT_GUI},
    {"WIN", KEY_LEFT_GUI},
    {"WINDOWS", KEY_LEFT_GUI},
    {"COMMAND", KEY_LEFT_GUI},
    {"KEY_RIGHT_GUI", KEY_RIGHT_GUI},
    {"RIGHT_GUI", KEY_RIGHT_GUI},
    {"RGUI", KEY_RIGHT_GUI},
    {"SPACE", ' '},
    {"SPACEBAR", ' '}};

const size_t KEY_NAME_MAP_SIZE = sizeof(KEY_NAME_MAP) / sizeof(KEY_NAME_MAP[0]);

struct ButtonEntry
{
  const char *name;
  uint8_t mask;
};

const ButtonEntry MOUSE_BUTTON_MAP[] = {
    {"LEFT", MOUSE_LEFT},
    {"MOUSE_LEFT", MOUSE_LEFT},
    {"BUTTON1", MOUSE_LEFT},
    {"RIGHT", MOUSE_RIGHT},
    {"MOUSE_RIGHT", MOUSE_RIGHT},
    {"BUTTON2", MOUSE_RIGHT},
    {"MIDDLE", MOUSE_MIDDLE},
    {"SCROLL", MOUSE_MIDDLE},
    {"WHEEL", MOUSE_MIDDLE},
    {"BUTTON3", MOUSE_MIDDLE},
    {"BACK", MOUSE_BACK},
    {"BUTTON4", MOUSE_BACK},
    {"FORWARD", MOUSE_FORWARD},
    {"BUTTON5", MOUSE_FORWARD}};

const size_t MOUSE_BUTTON_MAP_SIZE = sizeof(MOUSE_BUTTON_MAP) / sizeof(MOUSE_BUTTON_MAP[0]);

struct ConsumerEntry
{
  const char *name;
  const MediaKeyReport *report;
};

const ConsumerEntry CONSUMER_KEY_MAP[] = {
    {"KEY_MEDIA_PLAY_PAUSE", &KEY_MEDIA_PLAY_PAUSE},
    {"MEDIA_PLAY_PAUSE", &KEY_MEDIA_PLAY_PAUSE},
    {"PLAY_PAUSE", &KEY_MEDIA_PLAY_PAUSE},
    {"KEY_MEDIA_STOP", &KEY_MEDIA_STOP},
    {"MEDIA_STOP", &KEY_MEDIA_STOP},
    {"KEY_MEDIA_NEXT_TRACK", &KEY_MEDIA_NEXT_TRACK},
    {"MEDIA_NEXT", &KEY_MEDIA_NEXT_TRACK},
    {"NEXT_TRACK", &KEY_MEDIA_NEXT_TRACK},
    {"KEY_MEDIA_PREVIOUS_TRACK", &KEY_MEDIA_PREVIOUS_TRACK},
    {"MEDIA_PREVIOUS", &KEY_MEDIA_PREVIOUS_TRACK},
    {"MEDIA_PREV", &KEY_MEDIA_PREVIOUS_TRACK},
    {"PREVIOUS_TRACK", &KEY_MEDIA_PREVIOUS_TRACK},
    {"KEY_MEDIA_VOLUME_UP", &KEY_MEDIA_VOLUME_UP},
    {"VOLUME_UP", &KEY_MEDIA_VOLUME_UP},
    {"KEY_MEDIA_VOLUME_DOWN", &KEY_MEDIA_VOLUME_DOWN},
    {"VOLUME_DOWN", &KEY_MEDIA_VOLUME_DOWN},
    {"KEY_MEDIA_MUTE", &KEY_MEDIA_MUTE},
    {"MUTE", &KEY_MEDIA_MUTE},
    {"KEY_MEDIA_WWW_HOME", &KEY_MEDIA_WWW_HOME},
    {"WWW_HOME", &KEY_MEDIA_WWW_HOME},
    {"KEY_MEDIA_EMAIL_READER", &KEY_MEDIA_EMAIL_READER},
    {"EMAIL", &KEY_MEDIA_EMAIL_READER},
    {"KEY_MEDIA_CALCULATOR", &KEY_MEDIA_CALCULATOR},
    {"CALCULATOR", &KEY_MEDIA_CALCULATOR},
    {"KEY_MEDIA_WWW_SEARCH", &KEY_MEDIA_WWW_SEARCH},
    {"WWW_SEARCH", &KEY_MEDIA_WWW_SEARCH},
    {"KEY_MEDIA_WWW_STOP", &KEY_MEDIA_WWW_STOP},
    {"WWW_STOP", &KEY_MEDIA_WWW_STOP},
    {"KEY_MEDIA_WWW_BACK", &KEY_MEDIA_WWW_BACK},
    {"WWW_BACK", &KEY_MEDIA_WWW_BACK},
    {"KEY_MEDIA_WWW_BOOKMARKS", &KEY_MEDIA_WWW_BOOKMARKS},
    {"WWW_BOOKMARKS", &KEY_MEDIA_WWW_BOOKMARKS}};

const size_t CONSUMER_KEY_MAP_SIZE = sizeof(CONSUMER_KEY_MAP) / sizeof(CONSUMER_KEY_MAP[0]);

constexpr size_t MAX_CONSUMER_KEYS = 8;

bool initializeNvs()
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    if (nvs_flash_erase() != ESP_OK)
    {
      return false;
    }
    err = nvs_flash_init();
  }
  return err == ESP_OK;
}

bool loadWifiCredentials(String &ssid, String &password)
{
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &handle);
  if (err != ESP_OK)
  {
    return false;
  }

  size_t ssidLength = 0;
  err = nvs_get_str(handle, NVS_KEY_SSID, nullptr, &ssidLength);
  if (err != ESP_OK || ssidLength <= 1)
  {
    nvs_close(handle);
    return false;
  }

  std::vector<char> ssidBuffer(ssidLength);
  err = nvs_get_str(handle, NVS_KEY_SSID, ssidBuffer.data(), &ssidLength);
  if (err != ESP_OK)
  {
    nvs_close(handle);
    return false;
  }

  size_t passwordLength = 0;
  err = nvs_get_str(handle, NVS_KEY_PASS, nullptr, &passwordLength);
  if (err == ESP_ERR_NVS_NOT_FOUND)
  {
    passwordLength = 1;
  }
  else if (err != ESP_OK)
  {
    nvs_close(handle);
    return false;
  }

  std::vector<char> passwordBuffer(passwordLength);
  if (passwordLength > 1)
  {
    err = nvs_get_str(handle, NVS_KEY_PASS, passwordBuffer.data(), &passwordLength);
    if (err != ESP_OK)
    {
      nvs_close(handle);
      return false;
    }
  }
  else
  {
    passwordBuffer[0] = '\0';
  }

  nvs_close(handle);

  ssid = String(ssidBuffer.data());
  password = String(passwordBuffer.data());
  return true;
}

bool saveWifiCredentials(const String &ssid, const String &password)
{
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &handle);
  if (err != ESP_OK)
  {
    return false;
  }

  err = nvs_set_str(handle, NVS_KEY_SSID, ssid.c_str());
  if (err != ESP_OK)
  {
    nvs_close(handle);
    return false;
  }

  err = nvs_set_str(handle, NVS_KEY_PASS, password.c_str());
  if (err != ESP_OK)
  {
    nvs_close(handle);
    return false;
  }

  err = nvs_commit(handle);
  nvs_close(handle);
  return err == ESP_OK;
}

const char *wifiAuthModeToString(wifi_auth_mode_t mode)
{
  switch (mode)
  {
  case WIFI_AUTH_OPEN:
    return "open";
  case WIFI_AUTH_WEP:
    return "wep";
  case WIFI_AUTH_WPA_PSK:
    return "wpa_psk";
  case WIFI_AUTH_WPA2_PSK:
    return "wpa2_psk";
  case WIFI_AUTH_WPA_WPA2_PSK:
    return "wpa_wpa2_psk";
  case WIFI_AUTH_WPA2_ENTERPRISE:
    return "wpa2_enterprise";
  case WIFI_AUTH_WPA3_PSK:
    return "wpa3_psk";
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return "wpa2_wpa3_psk";
  case WIFI_AUTH_WAPI_PSK:
    return "wapi_psk";
  default:
    return "unknown";
  }
}

void setNoCacheHeaders(httpd_req_t *req)
{
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");
}

const char *reasonPhraseForStatus(int statusCode)
{
  switch (statusCode)
  {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 500:
    return "Internal Server Error";
  default:
    return "OK";
  }
}

esp_err_t sendPortalPage(httpd_req_t *req)
{
  setNoCacheHeaders(req);
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req,
                         reinterpret_cast<const char *>(src_main_web_index_html_start),
                         CAPTIVE_PORTAL_INDEX_HTML_LENGTH);
}

esp_err_t sendJsonResponse(httpd_req_t *req, int statusCode, const JsonDocument &doc)
{
  String payload;
  serializeJson(doc, payload);
  setNoCacheHeaders(req);
  httpd_resp_set_type(req, "application/json");
  char status[32];
  const char *reason = reasonPhraseForStatus(statusCode);
  snprintf(status, sizeof(status), "%d %s", statusCode, reason);
  httpd_resp_set_status(req, status);
  return httpd_resp_send(req, payload.c_str(), HTTPD_RESP_USE_STRLEN);
}

void startCaptivePortalServices()
{
  if (dnsServerActive)
  {
    dnsServer.stop();
  }

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  IPAddress apIp = WiFi.softAPIP();
  dnsServer.start(DNS_PORT, "*", apIp);
  dnsServerActive = true;
}

void stopCaptivePortalServices()
{
  if (dnsServerActive)
  {
    dnsServer.stop();
    dnsServerActive = false;
  }
}

bool isApMode(wifi_mode_t mode)
{
  return mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
}

void setWifiModeTracked(wifi_mode_t mode)
{
  if (wifiCurrentMode == mode)
  {
    return;
  }
  if (wifiCurrentMode == WIFI_MODE_NULL)
  {
    WiFi.mode(mode);
  }
  else
  {
    esp_wifi_set_mode(mode);
  }
  wifiCurrentMode = mode;
}

void ensureApOnlyMode()
{
  temporaryApStaMode = false;
  wifiTargetMode = WIFI_MODE_AP;
  wifiState = WifiState::ApOnly;
  setWifiModeTracked(WIFI_MODE_AP);
}

void ensureStaOnlyMode()
{
  temporaryApStaMode = false;
  wifiTargetMode = WIFI_MODE_STA;
  wifiState = WifiState::StaOnly;
  setWifiModeTracked(WIFI_MODE_STA);
}

void requestApStaMode(bool temporary)
{
  setWifiModeTracked(WIFI_MODE_APSTA);
  wifiState = WifiState::ApSta;
  temporaryApStaMode = temporary;
  wifiTargetMode = temporary ? WIFI_MODE_AP : WIFI_MODE_STA;
}

void restoreApModeAfterTemporarySta()
{
  if (temporaryApStaMode && wifiState == WifiState::ApSta)
  {
    ensureApOnlyMode();
  }
}

void shutdownAccessPoint()
{
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  if (!isApMode(mode))
  {
    ensureStaOnlyMode();
    configurationMode = false;
    return;
  }

  WiFi.softAPdisconnect(true);
  stopCaptivePortalServices();
  ensureStaOnlyMode();
  configurationMode = false;
}

void scheduleStaOnlyTransition()
{
  if (wifiState != WifiState::ApSta || temporaryApStaMode)
  {
    return;
  }
  apShutdownPending = true;
  apShutdownDeadline = millis() + WIFI_AP_SHUTDOWN_DELAY_MS;
  wifiState = WifiState::TransitioningToSta;
  wifiTargetMode = WIFI_MODE_STA;
}

void finalizeStaOnlyTransition()
{
  if (!apShutdownPending)
  {
    return;
  }
  apShutdownPending = false;
  shutdownAccessPoint();
}

esp_err_t handleIndex(httpd_req_t *req)
{
  return sendPortalPage(req);
}

esp_err_t handleHttp404(httpd_req_t *req, httpd_err_code_t)
{
  if (configurationMode)
  {
    return sendPortalPage(req);
  }

  setNoCacheHeaders(req);
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "Not found", HTTPD_RESP_USE_STRLEN);
}

esp_err_t handleScan(httpd_req_t *req)
{
  requestApStaMode(true);
  wifi_scan_config_t scanConfig = {};
  scanConfig.show_hidden = false;
  scanConfig.scan_type = WIFI_SCAN_TYPE_ACTIVE;

  esp_err_t err = esp_wifi_scan_start(&scanConfig, true);
  if (err != ESP_OK)
  {
    restoreApModeAfterTemporarySta();
    DynamicJsonDocument response(160);
    response["status"] = "error";
    response["message"] = "Scan failed";
    response["code"] = static_cast<int>(err);
    return sendJsonResponse(req, 500, response);
  }

  uint16_t apCount = 0;
  esp_err_t countErr = esp_wifi_scan_get_ap_num(&apCount);
  if (countErr != ESP_OK)
  {
    restoreApModeAfterTemporarySta();
    DynamicJsonDocument response(160);
    response["status"] = "error";
    response["message"] = "Unable to read scan results";
    response["code"] = static_cast<int>(countErr);
    return sendJsonResponse(req, 500, response);
  }
  std::vector<wifi_ap_record_t> records(apCount);
  if (apCount > 0)
  {
    esp_wifi_scan_get_ap_records(&apCount, records.data());
    records.resize(apCount);
  }

  std::sort(records.begin(), records.end(), [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) {
    return a.rssi > b.rssi;
  });

  const size_t maxNetworks = 20;
  const size_t visibleNetworks = std::min(records.size(), maxNetworks);
  DynamicJsonDocument doc(256 + visibleNetworks * 96);
  JsonArray networks = doc.createNestedArray("networks");
  size_t added = 0;
  for (const auto &record : records)
  {
    if (added >= maxNetworks)
    {
      break;
    }

    JsonObject network = networks.createNestedObject();
    network["ssid"] = reinterpret_cast<const char *>(record.ssid);
    network["rssi"] = record.rssi;
    network["channel"] = record.primary;
    network["auth"] = wifiAuthModeToString(record.authmode);
    ++added;
  }

  restoreApModeAfterTemporarySta();
  doc["status"] = "ok";
  return sendJsonResponse(req, 200, doc);
}

esp_err_t handleConfigure(httpd_req_t *req)
{
  if (req->method != HTTP_POST)
  {
    DynamicJsonDocument response(128);
    response["status"] = "error";
    response["message"] = "Method not allowed";
    return sendJsonResponse(req, 405, response);
  }

  if (req->content_len == 0)
  {
    DynamicJsonDocument response(128);
    response["status"] = "error";
    response["message"] = "Missing request body";
    return sendJsonResponse(req, 400, response);
  }

  std::string body;
  body.resize(req->content_len);
  size_t received = 0;
  while (received < body.size())
  {
    int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
    if (ret <= 0)
    {
      DynamicJsonDocument response(128);
      response["status"] = "error";
      response["message"] = "Failed to read body";
      return sendJsonResponse(req, 400, response);
    }
    received += static_cast<size_t>(ret);
  }

  StaticJsonDocument<256> payload;
  DeserializationError error = deserializeJson(payload, body);
  if (error)
  {
    DynamicJsonDocument response(128);
    response["status"] = "error";
    response["message"] = "Invalid JSON";
    return sendJsonResponse(req, 400, response);
  }

  String ssid = payload["ssid"] | "";
  String password = payload["password"] | "";
  ssid.trim();

  if (ssid.isEmpty())
  {
    DynamicJsonDocument response(128);
    response["status"] = "error";
    response["message"] = "SSID is required";
    return sendJsonResponse(req, 400, response);
  }

  if (!configurationMode)
  {
    startAccessPoint();
  }

  bool connected = connectStationAndPersist(ssid, password, true);

  DynamicJsonDocument response(192);
  response["ssid"] = ssid;

  if (connected)
  {
    response["status"] = "ok";
    response["message"] = "Connected";
    return sendJsonResponse(req, 200, response);
  }
  else
  {
    response["status"] = "error";
    response["message"] = "Failed to connect";
    sendJsonResponse(req, 500, response);
    if (!configurationMode)
    {
      startAccessPoint();
    }
    return ESP_OK;
  }
}

esp_err_t handleTransportGet(httpd_req_t *req)
{
  DynamicJsonDocument doc(160);
  doc["status"] = "ok";
  doc["mode"] = transportModeToString(activeTransportMode.load());
  doc["baud"] = uartBaudRate;
  return sendJsonResponse(req, 200, doc);
}

esp_err_t handleTransportPost(httpd_req_t *req)
{
  if (req->method != HTTP_POST)
  {
    DynamicJsonDocument response(128);
    response["status"] = "error";
    response["message"] = "Method not allowed";
    return sendJsonResponse(req, 405, response);
  }

  if (req->content_len == 0)
  {
    DynamicJsonDocument response(128);
    response["status"] = "error";
    response["message"] = "Missing request body";
    return sendJsonResponse(req, 400, response);
  }

  std::string body;
  body.resize(req->content_len);
  size_t received = 0;
  while (received < body.size())
  {
    int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
    if (ret <= 0)
    {
      DynamicJsonDocument response(128);
      response["status"] = "error";
      response["message"] = "Failed to read body";
      return sendJsonResponse(req, 400, response);
    }
    received += static_cast<size_t>(ret);
  }

  StaticJsonDocument<160> payload;
  DeserializationError error = deserializeJson(payload, body);
  if (error)
  {
    DynamicJsonDocument response(128);
    response["status"] = "error";
    response["message"] = "Invalid JSON";
    return sendJsonResponse(req, 400, response);
  }

  const char *modeValue = payload["mode"] | "uart";
  TransportMode requestedMode = stringToTransportMode(modeValue);

  uint32_t requestedBaud = uartBaudRate;
  if (!payload["baud"].isNull())
  {
    int baudCandidate = payload["baud"].as<int>();
    if (baudCandidate < 9600 || baudCandidate > 921600)
    {
      DynamicJsonDocument response(160);
      response["status"] = "error";
      response["message"] = "Invalid baud rate";
      return sendJsonResponse(req, 400, response);
    }
    requestedBaud = static_cast<uint32_t>(baudCandidate);
  }

  if (requestedMode == TransportMode::Uart)
  {
    applyUartBaudRate(requestedBaud);
  }

  if (!applyTransportMode(requestedMode))
  {
    DynamicJsonDocument response(160);
    response["status"] = "error";
    response["message"] = "Failed to apply transport mode";
    return sendJsonResponse(req, 500, response);
  }

  if (!saveTransportConfig(activeTransportMode.load(), uartBaudRate))
  {
    DynamicJsonDocument response(160);
    response["status"] = "error";
    response["message"] = "Failed to persist transport";
    return sendJsonResponse(req, 500, response);
  }

  DynamicJsonDocument response(160);
  response["status"] = "ok";
  response["mode"] = transportModeToString(activeTransportMode.load());
  response["baud"] = uartBaudRate;
  return sendJsonResponse(req, 200, response);
}

void registerHttpEndpoints(httpd_handle_t server)
{
  if (!server)
  {
    return;
  }

  static const httpd_uri_t indexUri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = handleIndex,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};

  static const httpd_uri_t indexHtmlUri = {
      .uri = "/index.html",
      .method = HTTP_GET,
      .handler = handleIndex,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};

  static const httpd_uri_t scanUri = {
      .uri = "/scan",
      .method = HTTP_GET,
      .handler = handleScan,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};

  static const httpd_uri_t configureUri = {
      .uri = "/configure",
      .method = HTTP_POST,
      .handler = handleConfigure,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};

  static const httpd_uri_t transportGetUri = {
      .uri = "/api/transport",
      .method = HTTP_GET,
      .handler = handleTransportGet,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};

  static const httpd_uri_t transportPostUri = {
      .uri = "/api/transport",
      .method = HTTP_POST,
      .handler = handleTransportPost,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};

  static const httpd_uri_t androidPortalUri = {
      .uri = "/generate_204",
      .method = HTTP_GET,
      .handler = handleIndex,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};

  static const httpd_uri_t applePortalUri = {
      .uri = "/hotspot-detect.html",
      .method = HTTP_GET,
      .handler = handleIndex,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};

  static const httpd_uri_t windowsPortalUri = {
      .uri = "/ncsi.txt",
      .method = HTTP_GET,
      .handler = handleIndex,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr};

  httpd_register_uri_handler(server, &indexUri);
  httpd_register_uri_handler(server, &indexHtmlUri);
  httpd_register_uri_handler(server, &scanUri);
  httpd_register_uri_handler(server, &configureUri);
  httpd_register_uri_handler(server, &transportGetUri);
  httpd_register_uri_handler(server, &transportPostUri);
  httpd_register_uri_handler(server, &androidPortalUri);
  httpd_register_uri_handler(server, &applePortalUri);
  httpd_register_uri_handler(server, &windowsPortalUri);

  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handleHttp404);

  static const httpd_uri_t wsUri = {
      .uri = "/ws",
      .method = HTTP_GET,
      .handler = handleWebSocket,
      .user_ctx = nullptr,
      .is_websocket = true,
      .handle_ws_control_frames = true,
      .supported_subprotocol = nullptr};

  static const httpd_uri_t wsHidUri = {
      .uri = "/ws/hid",
      .method = HTTP_GET,
      .handler = handleWebSocket,
      .user_ctx = nullptr,
      .is_websocket = true,
      .handle_ws_control_frames = true,
      .supported_subprotocol = nullptr};

  httpd_register_uri_handler(server, &wsUri);
  httpd_register_uri_handler(server, &wsHidUri);
}

esp_err_t handleWebSocket(httpd_req_t *req)
{
  if (activeTransportMode.load() != TransportMode::Websocket)
  {
    return httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "WebSocket disabled");
  }

  if (req->method == HTTP_GET)
  {
    wsClientSocket = httpd_req_to_sockfd(req);
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {};
  esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
  if (ret != ESP_OK)
  {
    return ret;
  }

  std::vector<uint8_t> payload(frame.len + 1);
  frame.payload = payload.data();
  ret = httpd_ws_recv_frame(req, &frame, frame.len);
  if (ret != ESP_OK)
  {
    return ret;
  }
  payload[frame.len] = '\0';

  if (activeTransportMode.load() != TransportMode::Websocket)
  {
    return httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "WebSocket disabled");
  }

  if (!ensureTransportQueues())
  {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Queue unavailable");
  }

  switch (frame.type)
  {
  case HTTPD_WS_TYPE_TEXT:
    if (frame.len >= INPUT_BUFFER_LIMIT)
    {
      sendStatusError("JSON payload too large");
      return ESP_OK;
    }
    enqueueTransportMessage(transportCommandQueue,
                            reinterpret_cast<const char *>(frame.payload),
                            frame.len);
    break;
  case HTTPD_WS_TYPE_CLOSE:
    wsClientSocket = -1;
    break;
  case HTTPD_WS_TYPE_PING:
    frame.type = HTTPD_WS_TYPE_PONG;
    httpd_ws_send_frame(req, &frame);
    break;
  default:
    break;
  }

  return ESP_OK;
}

void httpServerTask(void *param)
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = HTTP_PORT;
  config.ctrl_port = HTTP_PORT + 1;
  config.task_priority = tskIDLE_PRIORITY + 4;
  config.stack_size = 8192;
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  config.core_id = 0;
#else
  config.core_id = kHttpTaskCore;
#endif

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) == ESP_OK)
  {
    httpServerHandle = server;
    registerHttpEndpoints(server);
  }

  for (;;)
  {
    if (activeTransportMode.load() == TransportMode::Websocket && transportEventQueue)
    {
      TransportMessage message = {};
      if (xQueueReceive(transportEventQueue, &message, pdMS_TO_TICKS(100)) == pdPASS)
      {
        if (server && wsClientSocket >= 0)
        {
          httpd_ws_frame_t frame = {};
          frame.type = HTTPD_WS_TYPE_TEXT;
          frame.payload = reinterpret_cast<uint8_t *>(message.payload);
          frame.len = message.length;
          esp_err_t err = httpd_ws_send_frame_async(server, wsClientSocket, &frame);
          if (err != ESP_OK)
          {
            wsClientSocket = -1;
            xQueueSendToFront(transportEventQueue, &message, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
          }
        }
        else
        {
          xQueueSendToFront(transportEventQueue, &message, 0);
          vTaskDelay(pdMS_TO_TICKS(50));
        }
      }
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

void startHttpServerTask()
{
  if (httpServerTaskHandle)
  {
    return;
  }

  constexpr uint32_t stackSize = 8192;
  constexpr UBaseType_t priority = tskIDLE_PRIORITY + 3;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  xTaskCreate(httpServerTask, "http_ws_task", stackSize, nullptr, priority, &httpServerTaskHandle);
#else
  xTaskCreatePinnedToCore(httpServerTask,
                          "http_ws_task",
                          stackSize,
                          nullptr,
                          priority,
                          &httpServerTaskHandle,
                          kHttpTaskCore);
#endif
}

void transportPumpTask(void *param)
{
  constexpr TickType_t idleDelay = pdMS_TO_TICKS(10);
  (void)param;

  for (;;)
  {
    TransportMode mode = activeTransportMode.load();
    if (mode == TransportMode::Websocket)
    {
      if (transportCommandQueue)
      {
        TransportMessage message = {};
        if (xQueueReceive(transportCommandQueue, &message, pdMS_TO_TICKS(50)) == pdPASS)
        {
          processCommand(String(message.payload));
        }
        else
        {
          vTaskDelay(idleDelay);
        }
      }
      else
      {
        vTaskDelay(idleDelay);
      }
    }
    else
    {
      bool processed = false;
      if (serialActive)
      {
        while (Serial.available())
        {
          processed = true;
          char c = static_cast<char>(Serial.read());

          if (c == '\r')
          {
            continue;
          }

          if (c == '\n')
          {
            flushInputBuffer();
            continue;
          }

          if (inputBuffer.length() >= INPUT_BUFFER_LIMIT)
          {
            sendStatusError("Input too long");
            inputBuffer = "";
            continue;
          }

          inputBuffer += c;
        }
      }

      if (!processed)
      {
        vTaskDelay(idleDelay);
      }
    }
  }
}

void startTransportPumpTask()
{
  if (transportPumpTaskHandle)
  {
    return;
  }

  constexpr uint32_t stackSize = 4096;
  constexpr UBaseType_t priority = tskIDLE_PRIORITY + 2;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  xTaskCreate(transportPumpTask, "transport_pump", stackSize, nullptr, priority, &transportPumpTaskHandle);
#else
  xTaskCreatePinnedToCore(transportPumpTask,
                          "transport_pump",
                          stackSize,
                          nullptr,
                          priority,
                          &transportPumpTaskHandle,
                          kHttpTaskCore);
#endif
}

void stopAccessPoint()
{
  apShutdownPending = false;
  shutdownAccessPoint();
}

void startAccessPoint()
{
  apShutdownPending = false;
  ensureApOnlyMode();
  IPAddress localIp(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIp, gateway, subnet);
  WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
  startCaptivePortalServices();
  configurationMode = true;
}

bool connectToStation(const String &ssid, const String &password, bool keepApActive)
{
  if (ssid.isEmpty())
  {
    return false;
  }

  apShutdownPending = false;
  if (keepApActive)
  {
    requestApStaMode(false);
  }
  else
  {
    shutdownAccessPoint();
  }
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());

  staConnectInProgress = true;
  unsigned long startAttempt = millis();
  while ((millis() - startAttempt) < WIFI_CONNECT_TIMEOUT_MS)
  {
    wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED)
    {
      staConnectInProgress = false;
      if (!keepApActive)
      {
        configurationMode = false;
      }
      return true;
    }
    if (status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST || status == WL_DISCONNECTED)
    {
      WiFi.reconnect();
    }
    delay(WIFI_RETRY_DELAY_MS);
  }

  staConnectInProgress = false;
  if (keepApActive)
  {
    esp_wifi_disconnect();
    startAccessPoint();
  }
  else
  {
    WiFi.disconnect();
  }
  return false;
}

void sendStatusOk()
{
  dispatchTransportJson("{\"status\":\"ok\"}");
}

void sendStatusError(const char *message)
{
  String payload = F("{\"status\":\"error\",\"message\":\"");
  payload += message;
  payload += F("\"}");
  dispatchTransportJson(payload);
}

void sendEvent(const char *name, const char *detail = nullptr)
{
  String payload = F("{\"event\":\"");
  payload += name;
  if (detail)
  {
    payload += F("\",\"detail\":\"");
    payload += detail;
    payload += F("\"}");
  }
  else
  {
    payload += F("\"}");
  }
  dispatchTransportJson(payload);
}

bool connectStationAndPersist(const String &ssid, const String &password, bool keepApActive)
{
  if (!saveWifiCredentials(ssid, password))
  {
    sendStatusError("Failed to save WiFi credentials");
  }

  if (!connectToStation(ssid, password, keepApActive))
  {
    return false;
  }

  if (!keepApActive)
  {
    stopAccessPoint();
  }
  sendEvent("wifi_sta_connected", ssid.c_str());
  return true;
}

void processWifiStateMachine()
{
  if (apShutdownPending && millis() >= apShutdownDeadline)
  {
    finalizeStaOnlyTransition();
  }
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
  (void)info;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 2
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
#else
  if (event == SYSTEM_EVENT_STA_GOT_IP)
#endif
  {
    staConnectInProgress = false;
    scheduleStaOnlyTransition();
  }
}

bool lookupKeyCode(const String &token, uint8_t &code)
{
  for (size_t i = 0; i < KEY_NAME_MAP_SIZE; ++i)
  {
    if (token.equals(KEY_NAME_MAP[i].name))
    {
      code = KEY_NAME_MAP[i].code;
      return true;
    }
  }
  return false;
}

bool lookupConsumerKey(const String &token, const MediaKeyReport *&report)
{
  for (size_t i = 0; i < CONSUMER_KEY_MAP_SIZE; ++i)
  {
    if (token.equals(CONSUMER_KEY_MAP[i].name))
    {
      report = CONSUMER_KEY_MAP[i].report;
      return true;
    }
  }
  return false;
}

void reportInvalidKey(JsonVariantConst value)
{
  if (value.is<const char *>())
  {
    String message = F("Unknown key: ");
    message += value.as<const char *>();
    sendStatusError(message.c_str());
  }
  else if (value.is<int>())
  {
    String message = F("Invalid key code: ");
    message += String(value.as<int>());
    sendStatusError(message.c_str());
  }
  else
  {
    sendStatusError("Invalid key entry");
  }
}

bool parseKeyCode(JsonVariantConst value, uint8_t &code)
{
  if (value.is<int>())
  {
    int raw = value.as<int>();
    if (raw < 0 || raw > 255)
    {
      return false;
    }
    code = static_cast<uint8_t>(raw);
    return true;
  }

  if (value.is<const char *>())
  {
    String token = value.as<const char *>();
    token.trim();
    if (!token.length())
    {
      return false;
    }

    if (token.length() == 1)
    {
      code = static_cast<uint8_t>(token[0]);
      return true;
    }

    if (token.startsWith("0x") || token.startsWith("0X"))
    {
      long parsed = strtol(token.c_str(), nullptr, 16);
      if (parsed >= 0 && parsed <= 255)
      {
        code = static_cast<uint8_t>(parsed);
        return true;
      }
    }

    String upper = token;
    upper.toUpperCase();

    if (lookupKeyCode(upper, code))
    {
      return true;
    }

    if (upper.startsWith("KEY_"))
    {
      String shortened = upper.substring(4);
      if (lookupKeyCode(shortened, code))
      {
        return true;
      }
    }

    if (upper.startsWith("F"))
    {
      long fn = upper.substring(1).toInt();
      if (fn >= 1 && fn <= 24)
      {
        code = KEY_F1 + static_cast<uint8_t>(fn - 1);
        return true;
      }
    }

    if (upper.startsWith("KEY_F"))
    {
      long fn = upper.substring(5).toInt();
      if (fn >= 1 && fn <= 24)
      {
        code = KEY_F1 + static_cast<uint8_t>(fn - 1);
        return true;
      }
    }
  }

  return false;
}

bool collectKeyCodes(JsonVariantConst source, uint8_t *codes, size_t &count, size_t maxCount)
{
  count = 0;
  if (source.is<JsonArrayConst>())
  {
    JsonArrayConst arr = source.as<JsonArrayConst>();
    for (JsonVariantConst key : arr)
    {
      if (count >= maxCount)
      {
        sendStatusError("Too many keys in combo");
        return false;
      }
      if (!parseKeyCode(key, codes[count]))
      {
        reportInvalidKey(key);
        return false;
      }
      ++count;
    }
    return count > 0;
  }

  if (!source.isNull())
  {
    if (!parseKeyCode(source, codes[0]))
    {
      reportInvalidKey(source);
      return false;
    }
    count = 1;
    return true;
  }

  return false;
}

bool extractKeyCodes(JsonVariantConst command, uint8_t *codes, size_t &count, size_t maxCount = MAX_KEY_COMBO)
{
  JsonVariantConst keys = command["keys"];
  if (!keys.isNull())
  {
    return collectKeyCodes(keys, codes, count, maxCount);
  }

  JsonVariantConst key = command["key"];
  if (!key.isNull())
  {
    return collectKeyCodes(key, codes, count, maxCount);
  }

  JsonVariantConst code = command["code"];
  if (!code.isNull())
  {
    return collectKeyCodes(code, codes, count, maxCount);
  }

  sendStatusError("keyboard action requires key(s) or code");
  return false;
}

void reportInvalidButton(JsonVariantConst value)
{
  if (value.is<const char *>())
  {
    String message = F("Unknown mouse button: ");
    message += value.as<const char *>();
    sendStatusError(message.c_str());
  }
  else if (value.is<int>())
  {
    String message = F("Invalid mouse button mask: ");
    message += String(value.as<int>());
    sendStatusError(message.c_str());
  }
  else
  {
    sendStatusError("Invalid mouse button entry");
  }
}

bool addButtonByName(const String &token, uint8_t &mask)
{
  for (size_t i = 0; i < MOUSE_BUTTON_MAP_SIZE; ++i)
  {
    if (token.equals(MOUSE_BUTTON_MAP[i].name))
    {
      mask |= MOUSE_BUTTON_MAP[i].mask;
      return true;
    }
  }
  return false;
}

bool parseButtonMask(JsonVariantConst value, uint8_t &mask)
{
  mask = 0;
  if (value.is<JsonArrayConst>())
  {
    JsonArrayConst arr = value.as<JsonArrayConst>();
    for (JsonVariantConst button : arr)
    {
      if (button.is<const char *>())
      {
        String token = button.as<const char *>();
        token.trim();
        token.toUpperCase();
        if (!addButtonByName(token, mask))
        {
          reportInvalidButton(button);
          return false;
        }
      }
      else if (button.is<int>())
      {
        mask |= static_cast<uint8_t>(button.as<int>());
      }
      else
      {
        reportInvalidButton(button);
        return false;
      }
    }
    return mask != 0;
  }

  if (value.is<const char *>())
  {
    String token = value.as<const char *>();
    token.trim();
    token.toUpperCase();
    if (addButtonByName(token, mask))
    {
      return true;
    }
    reportInvalidButton(value);
    return false;
  }

  if (value.is<int>())
  {
    int raw = value.as<int>();
    if (raw < 0 || raw > 255)
    {
      reportInvalidButton(value);
      return false;
    }
    mask = static_cast<uint8_t>(raw);
    return true;
  }

  reportInvalidButton(value);
  return false;
}

uint16_t clampRepeat(JsonVariantConst value)
{
  if (!value.is<int>())
  {
    return 1;
  }
  int repeat = value.as<int>();
  if (repeat < 1)
  {
    repeat = 1;
  }
  if (repeat > 100)
  {
    repeat = 100;
  }
  return static_cast<uint16_t>(repeat);
}

uint16_t clampDuration(JsonVariantConst value, uint16_t defaultValue, uint16_t minValue = 0, uint16_t maxValue = 1000)
{
  if (!value.is<int>())
  {
    return defaultValue;
  }

  int raw = value.as<int>();
  if (raw < static_cast<int>(minValue))
  {
    return minValue;
  }
  if (raw > static_cast<int>(maxValue))
  {
    return maxValue;
  }
  return static_cast<uint16_t>(raw);
}

int getOptionalInt(JsonVariantConst object, const char *primaryKey, const char *secondaryKey, int defaultValue)
{
  JsonVariantConst primary = object[primaryKey];
  if (!primary.isNull())
  {
    return primary.as<int>();
  }

  if (secondaryKey != nullptr)
  {
    JsonVariantConst secondary = object[secondaryKey];
    if (!secondary.isNull())
    {
      return secondary.as<int>();
    }
  }

  return defaultValue;
}

void reportInvalidConsumerKey(JsonVariantConst value)
{
  if (value.is<const char *>())
  {
    String message = F("Unknown consumer key: ");
    message += value.as<const char *>();
    sendStatusError(message.c_str());
  }
  else
  {
    sendStatusError("Invalid consumer key entry");
  }
}

bool collectConsumerReports(JsonVariantConst source, const MediaKeyReport **reports, size_t &count, size_t maxCount)
{
  count = 0;
  if (source.is<JsonArrayConst>())
  {
    JsonArrayConst arr = source.as<JsonArrayConst>();
    for (JsonVariantConst entry : arr)
    {
      if (!entry.is<const char *>())
      {
        reportInvalidConsumerKey(entry);
        return false;
      }
      if (count >= maxCount)
      {
        sendStatusError("Too many consumer keys");
        return false;
      }
      String token = entry.as<const char *>();
      token.trim();
      token.toUpperCase();
      if (!lookupConsumerKey(token, reports[count]))
      {
        reportInvalidConsumerKey(entry);
        return false;
      }
      ++count;
    }
    return count > 0;
  }

  if (source.is<const char *>())
  {
    String token = source.as<const char *>();
    token.trim();
    token.toUpperCase();
    if (lookupConsumerKey(token, reports[0]))
    {
      count = 1;
      return true;
    }
    reportInvalidConsumerKey(source);
    return false;
  }

  reportInvalidConsumerKey(source);
  return false;
}

void handleKeyboard(JsonVariantConst command)
{
  if (!Keyboard.isConnected())
  {
    sendStatusError("BLE keyboard not connected");
    return;
  }

  const char *action = command["action"] | "press";
  uint8_t codes[MAX_KEY_COMBO];
  size_t keyCount = 0;

  if (strcmp(action, "write") == 0 || strcmp(action, "print") == 0 || strcmp(action, "println") == 0)
  {
    const char *text = command["text"];
    uint16_t repeat = clampRepeat(command["repeat"]);
    bool addNewLine = strcmp(action, "println") == 0 || command["newline"].as<bool>();
    size_t textLength = text ? strlen(text) : 0;

    uint16_t charDelay = DEFAULT_CHAR_DELAY_MS;
    bool charDelaySpecified = false;
    auto updateCharDelay = [&](JsonVariantConst value) {
      if (!value.isNull())
      {
        charDelay = clampDuration(value, charDelay, 0, 1000);
        charDelaySpecified = true;
      }
    };
    updateCharDelay(command["charDelayMs"]);
    updateCharDelay(command["char_delay_ms"]);
    updateCharDelay(command["interKeyDelayMs"]);
    updateCharDelay(command["inter_key_delay_ms"]);
    if (!charDelaySpecified)
    {
      updateCharDelay(command["delayMs"]);
    }
    if (!charDelaySpecified)
    {
      updateCharDelay(command["delay_ms"]);
    }

    bool newlineCarriage = command["newlineCarriage"].isNull() ? true : command["newlineCarriage"].as<bool>();

    if (text)
    {
      for (uint16_t i = 0; i < repeat; ++i)
      {
        for (size_t idx = 0; idx < textLength; ++idx)
        {
          Keyboard.write(static_cast<uint8_t>(text[idx]));
          if (charDelay)
          {
            delay(charDelay);
          }
        }
        if (addNewLine)
        {
          if (newlineCarriage)
          {
            Keyboard.write('\r');
            if (charDelay)
            {
              delay(charDelay);
            }
          }
          Keyboard.write('\n');
          if (charDelay)
          {
            delay(charDelay);
          }
        }
      }
      sendStatusOk();
      return;
    }

    if (addNewLine)
    {
      for (uint16_t i = 0; i < repeat; ++i)
      {
        if (newlineCarriage)
        {
          Keyboard.write('\r');
          if (charDelay)
          {
            delay(charDelay);
          }
        }
        Keyboard.write('\n');
        if (charDelay)
        {
          delay(charDelay);
        }
      }
      sendStatusOk();
      return;
    }

    if (!extractKeyCodes(command, codes, keyCount))
    {
      return;
    }

    for (uint16_t i = 0; i < repeat; ++i)
    {
      for (size_t idx = 0; idx < keyCount; ++idx)
      {
        Keyboard.write(codes[idx]);
      }
      if (addNewLine)
      {
        Keyboard.println();
      }
    }
    sendStatusOk();
    return;
  }

  if (strcmp(action, "releaseAll") == 0 || strcmp(action, "release_all") == 0)
  {
    Keyboard.releaseAll();
    sendStatusOk();
    return;
  }

  if (!extractKeyCodes(command, codes, keyCount))
  {
    return;
  }

  if (strcmp(action, "press") == 0)
  {
    for (size_t idx = 0; idx < keyCount; ++idx)
    {
      Keyboard.press(codes[idx]);
    }
    sendStatusOk();
    return;
  }

  if (strcmp(action, "release") == 0)
  {
    for (size_t idx = 0; idx < keyCount; ++idx)
    {
      Keyboard.release(codes[idx]);
    }
    sendStatusOk();
    return;
  }

  if (strcmp(action, "tap") == 0 || strcmp(action, "click") == 0)
  {
    int holdValue = command["holdMs"] | 20;
    if (!command["hold_ms"].isNull())
    {
      holdValue = command["hold_ms"].as<int>();
    }
    if (holdValue < 0)
    {
      holdValue = 0;
    }
    if (holdValue > 1000)
    {
      holdValue = 1000;
    }
    uint16_t holdMs = static_cast<uint16_t>(holdValue);
    for (size_t idx = 0; idx < keyCount; ++idx)
    {
      Keyboard.press(codes[idx]);
    }
    delay(holdMs);
    for (size_t idx = keyCount; idx > 0; --idx)
    {
      Keyboard.release(codes[idx - 1]);
    }
    sendStatusOk();
    return;
  }

  String message = F("Unknown keyboard action: ");
  message += action;
  sendStatusError(message.c_str());
}

void handleMouse(JsonVariantConst command)
{
  if (!Keyboard.isConnected())
  {
    sendStatusError("BLE connection not established");
    return;
  }

  const char *action = command["action"] | "move";

  if (strcmp(action, "move") == 0)
  {
    int dx = getOptionalInt(command, "x", "dx", 0);
    int dy = getOptionalInt(command, "y", "dy", 0);
    int wheel = getOptionalInt(command, "wheel", "scroll", 0);
    int pan = getOptionalInt(command, "pan", nullptr, 0);
    Mouse.move(dx, dy, wheel, pan);
    sendStatusOk();
    return;
  }

  if (strcmp(action, "releaseAll") == 0 || strcmp(action, "release_all") == 0)
  {
    Mouse.release(MOUSE_ALL_BUTTONS);
    sendStatusOk();
    return;
  }

  uint8_t mask = 0;
  bool hasButtons = false;
  JsonVariantConst buttonsVariant = command["buttons"];
  JsonVariantConst buttonVariant = command["button"];

  if (!buttonsVariant.isNull())
  {
    hasButtons = parseButtonMask(buttonsVariant, mask);
    if (!hasButtons)
    {
      return;
    }
  }
  else if (!buttonVariant.isNull())
  {
    hasButtons = parseButtonMask(buttonVariant, mask);
    if (!hasButtons)
    {
      return;
    }
  }
  else
  {
    if (strcmp(action, "click") == 0 || strcmp(action, "press") == 0 || strcmp(action, "release") == 0)
    {
      mask = MOUSE_LEFT;
      hasButtons = true;
    }
  }

  if (strcmp(action, "click") == 0)
  {
    Mouse.click(mask);
    sendStatusOk();
    return;
  }

  if (strcmp(action, "press") == 0)
  {
    if (!hasButtons)
    {
      sendStatusError("mouse press requires button(s)");
      return;
    }
    Mouse.press(mask);
    sendStatusOk();
    return;
  }

  if (strcmp(action, "release") == 0)
  {
    if (!hasButtons)
    {
      sendStatusError("mouse release requires button(s)");
      return;
    }
    Mouse.release(mask);
    sendStatusOk();
    return;
  }

  String message = F("Unknown mouse action: ");
  message += action;
  sendStatusError(message.c_str());
}

void handleConsumer(JsonVariantConst command)
{
  if (!Keyboard.isConnected())
  {
    sendStatusError("BLE keyboard not connected");
    return;
  }

  const MediaKeyReport *reports[MAX_CONSUMER_KEYS];
  size_t count = 0;

  JsonVariantConst keysVariant = command["keys"];
  if (!keysVariant.isNull())
  {
    if (!collectConsumerReports(keysVariant, reports, count, MAX_CONSUMER_KEYS))
    {
      return;
    }
  }
  else
  {
    JsonVariantConst keyVariant = command["key"];
    if (!keyVariant.isNull())
    {
      if (!collectConsumerReports(keyVariant, reports, count, MAX_CONSUMER_KEYS))
      {
        return;
      }
    }
    else
    {
      JsonVariantConst codeVariant = command["code"];
      if (!codeVariant.isNull())
      {
        if (!collectConsumerReports(codeVariant, reports, count, MAX_CONSUMER_KEYS))
        {
          return;
        }
      }
      else
      {
        sendStatusError("consumer action requires key");
        return;
      }
    }
  }

  if (count == 0)
  {
    sendStatusError("consumer action requires key");
    return;
  }

  uint16_t repeat = clampRepeat(command["repeat"]);
  int gapValue = command["gapMs"] | 5;
  if (!command["gap_ms"].isNull())
  {
    gapValue = command["gap_ms"].as<int>();
  }
  if (gapValue < 0)
  {
    gapValue = 0;
  }
  if (gapValue > 1000)
  {
    gapValue = 1000;
  }
  uint16_t gapMs = static_cast<uint16_t>(gapValue);

  for (uint16_t r = 0; r < repeat; ++r)
  {
    for (size_t idx = 0; idx < count; ++idx)
    {
      Keyboard.write(*reports[idx]);
      if (gapMs > 0)
      {
        delay(gapMs);
      }
    }
  }

  sendStatusOk();
}

void processCommand(const String &payload)
{
  if (payload.length() == 0)
  {
    return;
  }

  if (payload.length() > JSON_DOC_CAPACITY)
  {
    sendStatusError("JSON payload too large");
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error)
  {
    String message = F("JSON parse error: ");
    message += error.c_str();
    sendStatusError(message.c_str());
    return;
  }

  const char *device = doc["device"];
  if (!device)
  {
    device = doc["type"];
  }

  if (!device)
  {
    sendStatusError("Command missing device/type field");
    return;
  }

  if (strcasecmp(device, "keyboard") == 0)
  {
    handleKeyboard(doc.as<JsonVariantConst>());
  }
  else if (strcasecmp(device, "mouse") == 0)
  {
    handleMouse(doc.as<JsonVariantConst>());
  }
  else if (strcasecmp(device, "consumer") == 0 || strcasecmp(device, "media") == 0)
  {
    handleConsumer(doc.as<JsonVariantConst>());
  }
  else
  {
    String message = F("Unknown device type: ");
    message += device;
    sendStatusError(message.c_str());
  }
}

void flushInputBuffer()
{
  if (inputBuffer.length() > 0)
  {
    processCommand(inputBuffer);
    inputBuffer = "";
  }
}
} // namespace

void setup()
{
  inputBuffer.reserve(INPUT_BUFFER_LIMIT);
  Keyboard.begin();
  Mouse.begin();

  if (!initializeNvs())
  {
    sendStatusError("Failed to initialize NVS");
  }

  uint32_t storedBaud = DEFAULT_UART_BAUD;
  TransportMode storedMode = loadTransportModeFromStorage(storedBaud);
  applyUartBaudRate(storedBaud);
  if (!applyTransportMode(storedMode))
  {
    applyTransportMode(TransportMode::Uart);
    sendStatusError("Falling back to UART transport");
  }

  startHttpServerTask();
  startTransportPumpTask();
  WiFi.onEvent(onWifiEvent);

  String savedSsid;
  String savedPassword;
  bool staConnected = false;
  if (loadWifiCredentials(savedSsid, savedPassword))
  {
    if (connectStationAndPersist(savedSsid, savedPassword, false))
    {
      staConnected = true;
    }
  }

  if (!staConnected)
  {
    startAccessPoint();
    if (configurationMode)
    {
      sendEvent("wifi_config_mode");
    }
  }

  sendEvent("ready");
}

void loop()
{
  bool connected = Keyboard.isConnected();
  if (connected != lastBleConnectionState)
  {
    lastBleConnectionState = connected;
    sendEvent(connected ? "ble_connected" : "ble_disconnected");
  }

  if (dnsServerActive)
  {
    dnsServer.processNextRequest();
  }

  processWifiStateMachine();
  delay(2);
}
