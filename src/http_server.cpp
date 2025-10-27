#include "http_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <freertos/task.h>

#include <cstdio>
#include <string>
#include <vector>
#include <strings.h>

#include "wifi_manager.h"

namespace http_server
{
  namespace
  {
    constexpr uint16_t HTTP_PORT = 80;
    constexpr const char *HTTP_STATUS_SERVICE_UNAVAILABLE = "503 Service Unavailable";

#if defined(CONFIG_BT_NIMBLE_PINNED_TO_CORE)
    constexpr BaseType_t kHttpTaskCore = (CONFIG_BT_NIMBLE_PINNED_TO_CORE == 0) ? 1 : 0;
#elif defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    constexpr BaseType_t kHttpTaskCore = tskNO_AFFINITY;
#elif defined(CONFIG_ARDUINO_RUNNING_CORE)
    constexpr BaseType_t kHttpTaskCore = (CONFIG_ARDUINO_RUNNING_CORE == 0) ? 1 : 0;
#else
    constexpr BaseType_t kHttpTaskCore = 0;
#endif

    Dependencies dependencies_;
    bool dependencies_initialized_ = false;

    TaskHandle_t http_server_task_handle = nullptr;
    httpd_handle_t http_server_handle = nullptr;
    volatile int ws_client_socket = -1;

    extern const uint8_t src_web_index_html_start[] asm("_binary_src_web_index_html_start");
    extern const uint8_t src_web_index_html_end[] asm("_binary_src_web_index_html_end");

    size_t captivePortalIndexHtmlLength()
    {
      return static_cast<size_t>(src_web_index_html_end - src_web_index_html_start);
    }

    QueueHandle_t command_queue()
    {
      return (dependencies_.command_queue != nullptr) ? *dependencies_.command_queue : nullptr;
    }

    QueueHandle_t event_queue()
    {
      return (dependencies_.event_queue != nullptr) ? *dependencies_.event_queue : nullptr;
    }

    bool ensure_transport_queues()
    {
      if (!dependencies_.ensure_transport_queues)
      {
        return false;
      }
      return dependencies_.ensure_transport_queues();
    }

    bool enqueue_transport_message(QueueHandle_t queue, const char *data, size_t length)
    {
      if (!dependencies_.enqueue_transport_message)
      {
        return false;
      }
      return dependencies_.enqueue_transport_message(queue, data, length);
    }

    TransportMode active_transport_mode()
    {
      if (!dependencies_.get_active_transport_mode)
      {
        return TransportMode::Uart;
      }
      return dependencies_.get_active_transport_mode();
    }

    const char *transport_mode_to_string(TransportMode mode)
    {
      if (!dependencies_.transport_mode_to_string)
      {
        return (mode == TransportMode::Websocket) ? "websocket" : "uart";
      }
      return dependencies_.transport_mode_to_string(mode);
    }

    TransportMode string_to_transport_mode(const char *value)
    {
      if (!dependencies_.string_to_transport_mode)
      {
        return (value && strcasecmp(value, "websocket") == 0) ? TransportMode::Websocket : TransportMode::Uart;
      }
      return dependencies_.string_to_transport_mode(value);
    }

    void apply_uart_baud_rate(uint32_t baud)
    {
      if (dependencies_.apply_uart_baud_rate)
      {
        dependencies_.apply_uart_baud_rate(baud);
      }
    }

    uint32_t uart_baud_rate()
    {
      if (!dependencies_.get_uart_baud_rate)
      {
        return 115200;
      }
      return dependencies_.get_uart_baud_rate();
    }

    bool apply_transport_mode(TransportMode mode)
    {
      if (!dependencies_.apply_transport_mode)
      {
        return false;
      }
      return dependencies_.apply_transport_mode(mode);
    }

    bool save_transport_config(TransportMode mode, uint32_t baud)
    {
      if (!dependencies_.save_transport_config)
      {
        return false;
      }
      return dependencies_.save_transport_config(mode, baud);
    }

    void send_status_error(const char *message)
    {
      if (dependencies_.send_status_error)
      {
        dependencies_.send_status_error(message);
      }
    }

    void send_event(const char *name, const char *detail)
    {
      if (dependencies_.send_event)
      {
        dependencies_.send_event(name, detail);
      }
    }

    size_t input_buffer_limit()
    {
      return dependencies_.input_buffer_limit > 0 ? dependencies_.input_buffer_limit : 512;
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
      case 409:
        return "Conflict";
      case 500:
        return "Internal Server Error";
      case 503:
        return "Service Unavailable";
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
                             reinterpret_cast<const char *>(src_web_index_html_start),
                             captivePortalIndexHtmlLength());
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

    esp_err_t handleIndex(httpd_req_t *req)
    {
      return sendPortalPage(req);
    }

    esp_err_t handleHttp404(httpd_req_t *req, httpd_err_code_t)
    {
      if (wifi_manager::is_configuration_mode())
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
      JsonDocument doc;
      auto obj = doc.to<JsonObject>();
      JsonArray networks = obj["networks"].to<JsonArray>();
      String errorMessage;
      int errorCode = 0;
      if (!wifi_manager::scan_networks(networks, errorMessage, errorCode))
      {
        JsonDocument response;
        auto errorObj = response.to<JsonObject>();
        errorObj["status"] = "error";
        errorObj["message"] = errorMessage;
        if (errorCode != 0)
        {
          errorObj["code"] = errorCode;
        }
        int status = (errorCode == 0) ? 503 : 500;
        return sendJsonResponse(req, status, response);
      }

      obj["status"] = "ok";
      return sendJsonResponse(req, 200, doc);
    }

    esp_err_t handleConfigure(httpd_req_t *req)
    {
      if (req->method != HTTP_POST)
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "Method not allowed";
        return sendJsonResponse(req, 405, response);
      }

      if (req->content_len == 0)
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "Missing request body";
        return sendJsonResponse(req, 400, response);
      }

      std::string body;
      body.resize(req->content_len);
      size_t received = 0;
      if (!body.empty())
      {
        char *buffer = &body[0];
        while (received < body.size())
        {
          int ret = httpd_req_recv(req, buffer + received, body.size() - received);
          if (ret <= 0)
          {
            JsonDocument response;
            auto obj = response.to<JsonObject>();
            obj["status"] = "error";
            obj["message"] = "Failed to read body";
            return sendJsonResponse(req, 400, response);
          }
          received += static_cast<size_t>(ret);
        }
      }
      body.push_back('\0');

      JsonDocument payload;
      DeserializationError error = deserializeJson(payload, body.c_str());
      if (error)
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "Invalid JSON";
        return sendJsonResponse(req, 400, response);
      }

      String ssid = payload["ssid"] | "";
      String password = payload["password"] | "";
      ssid.trim();

      if (ssid.isEmpty())
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "SSID is required";
        return sendJsonResponse(req, 400, response);
      }

      if (!wifi_manager::is_configuration_mode())
      {
        wifi_manager::start_ap();
        if (wifi_manager::is_configuration_mode())
        {
          send_event("wifi_config_mode", nullptr);
        }
      }

      if (!wifi_manager::schedule_connect(ssid, password, false))
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "A WiFi connection attempt is already in progress";
        return sendJsonResponse(req, 409, response);
      }

      send_event("wifi_connecting", ssid.c_str());

      JsonDocument response;
      auto obj = response.to<JsonObject>();
      obj["status"] = "ok";
      wifi_manager::append_state_json(obj);
      obj["state"] = "connecting";
      if (!obj["ssid"].is<const char *>() || obj["ssid"].as<const char *>()[0] == '\0')
      {
        obj["ssid"] = ssid;
      }
      const char *messageValue = obj["message"].is<const char *>() ? obj["message"].as<const char *>() : nullptr;
      if (!messageValue || messageValue[0] == '\0')
      {
        obj["message"] = "Connecting";
      }
      return sendJsonResponse(req, 200, response);
    }

    esp_err_t handleWifiStateGet(httpd_req_t *req)
    {
      JsonDocument doc;
      auto obj = doc.to<JsonObject>();
      obj["status"] = "ok";
      wifi_manager::append_state_json(obj);
      return sendJsonResponse(req, 200, doc);
    }

    esp_err_t handleTransportGet(httpd_req_t *req)
    {
      JsonDocument doc;
      auto obj = doc.to<JsonObject>();
      obj["status"] = "ok";
      obj["mode"] = transport_mode_to_string(active_transport_mode());
      obj["baud"] = uart_baud_rate();
      return sendJsonResponse(req, 200, doc);
    }

    esp_err_t handleTransportPost(httpd_req_t *req)
    {
      if (req->method != HTTP_POST)
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "Method not allowed";
        return sendJsonResponse(req, 405, response);
      }

      if (req->content_len == 0)
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "Missing request body";
        return sendJsonResponse(req, 400, response);
      }

      std::string body;
      body.resize(req->content_len);
      size_t received = 0;
      if (!body.empty())
      {
        char *buffer = &body[0];
        while (received < body.size())
        {
          int ret = httpd_req_recv(req, buffer + received, body.size() - received);
          if (ret <= 0)
          {
            JsonDocument response;
            auto obj = response.to<JsonObject>();
            obj["status"] = "error";
            obj["message"] = "Failed to read body";
            return sendJsonResponse(req, 400, response);
          }
          received += static_cast<size_t>(ret);
        }
      }
      body.push_back('\0');

      JsonDocument payload;
      DeserializationError error = deserializeJson(payload, body.c_str());
      if (error)
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "Invalid JSON";
        return sendJsonResponse(req, 400, response);
      }

      const char *modeValue = payload["mode"] | "uart";
      TransportMode requestedMode = string_to_transport_mode(modeValue);

      uint32_t requestedBaud = uart_baud_rate();
      if (!payload["baud"].isNull())
      {
        int baudCandidate = payload["baud"].as<int>();
        if (baudCandidate < 9600 || baudCandidate > 921600)
        {
          JsonDocument response;
          auto obj = response.to<JsonObject>();
          obj["status"] = "error";
          obj["message"] = "Invalid baud rate";
          return sendJsonResponse(req, 400, response);
        }
        requestedBaud = static_cast<uint32_t>(baudCandidate);
      }

      if (requestedMode == TransportMode::Uart)
      {
        apply_uart_baud_rate(requestedBaud);
      }

      if (!apply_transport_mode(requestedMode))
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "Failed to apply transport mode";
        return sendJsonResponse(req, 500, response);
      }

      if (!save_transport_config(active_transport_mode(), uart_baud_rate()))
      {
        JsonDocument response;
        auto obj = response.to<JsonObject>();
        obj["status"] = "error";
        obj["message"] = "Failed to persist transport";
        return sendJsonResponse(req, 500, response);
      }

      JsonDocument response;
      auto obj = response.to<JsonObject>();
      obj["status"] = "ok";
      obj["mode"] = transport_mode_to_string(active_transport_mode());
      obj["baud"] = uart_baud_rate();
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

      static const httpd_uri_t wifiStateGetUri = {
          .uri = "/api/wifi/state",
          .method = HTTP_GET,
          .handler = handleWifiStateGet,
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
      httpd_register_uri_handler(server, &wifiStateGetUri);
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
      if (active_transport_mode() != TransportMode::Websocket)
      {
        httpd_resp_set_status(req, HTTP_STATUS_SERVICE_UNAVAILABLE);
        return httpd_resp_send(req, "WebSocket disabled", HTTPD_RESP_USE_STRLEN);
      }

      if (req->method == HTTP_GET)
      {
        ws_client_socket = httpd_req_to_sockfd(req);
        wifi_manager::send_cached_state();
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

      if (active_transport_mode() != TransportMode::Websocket)
      {
        httpd_resp_set_status(req, HTTP_STATUS_SERVICE_UNAVAILABLE);
        return httpd_resp_send(req, "WebSocket disabled", HTTPD_RESP_USE_STRLEN);
      }

      if (!ensure_transport_queues())
      {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Queue unavailable");
      }

      QueueHandle_t queue = command_queue();
      if (!queue)
      {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Queue unavailable");
      }

      switch (frame.type)
      {
      case HTTPD_WS_TYPE_TEXT:
        if (frame.len >= input_buffer_limit())
        {
          send_status_error("JSON payload too large");
          return ESP_OK;
        }
        enqueue_transport_message(queue, reinterpret_cast<const char *>(frame.payload), frame.len);
        break;
      case HTTPD_WS_TYPE_CLOSE:
        ws_client_socket = -1;
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
      (void)param;

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
        http_server_handle = server;
        registerHttpEndpoints(server);
      }

      constexpr TickType_t idleDelay = pdMS_TO_TICKS(100);
      for (;;)
      {
        QueueHandle_t events = event_queue();
        if (active_transport_mode() == TransportMode::Websocket && events)
        {
          TransportMessage message = {};
          if (xQueueReceive(events, &message, idleDelay) == pdPASS)
          {
            if (server && ws_client_socket >= 0)
            {
              httpd_ws_frame_t frame = {};
              frame.type = HTTPD_WS_TYPE_TEXT;
              frame.payload = reinterpret_cast<uint8_t *>(message.payload);
              frame.len = message.length;
              esp_err_t err = httpd_ws_send_frame_async(server, ws_client_socket, &frame);
              if (err != ESP_OK)
              {
                ws_client_socket = -1;
                xQueueSendToFront(events, &message, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
              }
            }
            else
            {
              xQueueSendToFront(events, &message, 0);
              vTaskDelay(pdMS_TO_TICKS(50));
            }
          }
        }
        else
        {
          vTaskDelay(idleDelay);
        }
      }
    }
  } // namespace

  void init(const Dependencies &dependencies)
  {
    dependencies_ = dependencies;
    dependencies_initialized_ = true;
  }

  void start()
  {
    if (!dependencies_initialized_ || http_server_task_handle)
    {
      return;
    }

    constexpr uint32_t stackSize = 8192;
    constexpr UBaseType_t priority = tskIDLE_PRIORITY + 3;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    xTaskCreate(httpServerTask, "http_ws_task", stackSize, nullptr, priority, &http_server_task_handle);
#else
    xTaskCreatePinnedToCore(httpServerTask,
                            "http_ws_task",
                            stackSize,
                            nullptr,
                            priority,
                            &http_server_task_handle,
                            kHttpTaskCore);
#endif
  }

  void stop()
  {
    close_active_websocket();
    if (http_server_handle)
    {
      httpd_handle_t server = http_server_handle;
      http_server_handle = nullptr;
      httpd_stop(server);
    }

    if (http_server_task_handle)
    {
      TaskHandle_t handle = http_server_task_handle;
      http_server_task_handle = nullptr;
      if (xTaskGetCurrentTaskHandle() == handle)
      {
        vTaskDelete(nullptr);
      }
      else
      {
        vTaskDelete(handle);
      }
    }
  }

  void close_active_websocket()
  {
    if (http_server_handle && ws_client_socket >= 0)
    {
      httpd_sess_trigger_close(http_server_handle, ws_client_socket);
    }
    ws_client_socket = -1;
  }
} // namespace http_server

