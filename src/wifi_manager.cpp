#include "wifi_manager.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <atomic>
#include <memory>

namespace wifi_manager
{
  namespace
  {
    constexpr const char *NVS_NAMESPACE_WIFI = "wifi";
    constexpr const char *NVS_KEY_SSID = "ssid";
    constexpr const char *NVS_KEY_PASS = "password";
    constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
    constexpr unsigned long WIFI_RETRY_DELAY_MS = 500;
    constexpr unsigned long WIFI_AP_SHUTDOWN_DELAY_MS = 3000;
    constexpr const char *CONFIG_AP_SSID = "uhid-setup";
    constexpr const char *CONFIG_AP_PASSWORD = "uhid1234";
    constexpr size_t WIFI_MAX_SSID_LENGTH = 32;
    constexpr size_t WIFI_MAX_PASSWORD_LENGTH = 64;
    constexpr uint16_t DNS_PORT = 53;

    #if defined(CONFIG_BT_NIMBLE_PINNED_TO_CORE)
    constexpr BaseType_t kWifiTaskCore = (CONFIG_BT_NIMBLE_PINNED_TO_CORE == 0) ? 1 : 0;
    #elif defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    constexpr BaseType_t kWifiTaskCore = tskNO_AFFINITY;
    #elif defined(CONFIG_ARDUINO_RUNNING_CORE)
    constexpr BaseType_t kWifiTaskCore = (CONFIG_ARDUINO_RUNNING_CORE == 0) ? 1 : 0;
    #else
    constexpr BaseType_t kWifiTaskCore = 0;
    #endif

    struct WifiManagerState
    {
      String last_state;
      String last_ssid;
      String last_message;
      bool configuration_mode = false;
      bool dns_active = false;
      bool sta_connect_in_progress = false;
      bool temporary_apsta_mode = false;
      bool ap_shutdown_pending = false;
      unsigned long ap_shutdown_deadline = 0;
      wifi_mode_t current_mode = WIFI_MODE_NULL;
      wifi_mode_t target_mode = WIFI_MODE_AP;
      wifi_mode_t mode_before_temporary = WIFI_MODE_NULL;
    };

    class WifiStateLock
    {
    public:
      explicit WifiStateLock(SemaphoreHandle_t mutex) : mutex_(mutex)
      {
        if (mutex_)
        {
          xSemaphoreTake(mutex_, portMAX_DELAY);
        }
      }

      ~WifiStateLock()
      {
        if (mutex_)
        {
          xSemaphoreGive(mutex_);
        }
      }

    private:
      SemaphoreHandle_t mutex_;
    };

    Callbacks callbacks_;
    WifiManagerState state_;
    SemaphoreHandle_t state_mutex_ = nullptr;

    struct WifiConnectRequest
    {
      bool keep_ap_active;
      char ssid[WIFI_MAX_SSID_LENGTH + 1];
      char password[WIFI_MAX_PASSWORD_LENGTH + 1];
    };

    QueueHandle_t wifi_connect_request_queue_ = nullptr;
    TaskHandle_t wifi_connect_task_handle_ = nullptr;
    std::atomic<bool> wifi_connect_busy_{false};

    DNSServer dns_server_;

    bool ensure_state_mutex()
    {
      if (!state_mutex_)
      {
        state_mutex_ = xSemaphoreCreateMutex();
      }
      return state_mutex_ != nullptr;
    }

    WifiStateLock lock_state()
    {
      ensure_state_mutex();
      return WifiStateLock(state_mutex_);
    }

    bool load_credentials_internal(String &ssid, String &password)
    {
      nvs_handle_t handle;
      esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &handle);
      if (err != ESP_OK)
      {
        return false;
      }

      size_t ssid_length = WIFI_MAX_SSID_LENGTH + 1;
      size_t pass_length = WIFI_MAX_PASSWORD_LENGTH + 1;
      std::unique_ptr<char[]> ssid_buffer(new char[ssid_length]);
      std::unique_ptr<char[]> pass_buffer(new char[pass_length]);

      err = nvs_get_str(handle, NVS_KEY_SSID, ssid_buffer.get(), &ssid_length);
      if (err != ESP_OK)
      {
        nvs_close(handle);
        return false;
      }

      err = nvs_get_str(handle, NVS_KEY_PASS, pass_buffer.get(), &pass_length);
      nvs_close(handle);
      if (err != ESP_OK)
      {
        return false;
      }

      ssid = String(ssid_buffer.get());
      password = String(pass_buffer.get());
      return true;
    }

    bool save_credentials_internal(const String &ssid, const String &password)
    {
      nvs_handle_t handle;
      esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &handle);
      if (err != ESP_OK)
      {
        return false;
      }

      err = nvs_set_str(handle, NVS_KEY_SSID, ssid.c_str());
      if (err == ESP_OK)
      {
        err = nvs_set_str(handle, NVS_KEY_PASS, password.c_str());
      }
      if (err == ESP_OK)
      {
        err = nvs_commit(handle);
      }
      nvs_close(handle);
      return err == ESP_OK;
    }

    bool invoke_load_credentials(String &ssid, String &password)
    {
      if (callbacks_.load_credentials)
      {
        return callbacks_.load_credentials(ssid, password);
      }
      return load_credentials_internal(ssid, password);
    }

    bool invoke_save_credentials(const String &ssid, const String &password)
    {
      if (callbacks_.save_credentials)
      {
        return callbacks_.save_credentials(ssid, password);
      }
      return save_credentials_internal(ssid, password);
    }

    void start_captive_portal()
    {
      if (state_.dns_active)
      {
        dns_server_.stop();
      }

      dns_server_.setErrorReplyCode(DNSReplyCode::NoError);
      IPAddress ap_ip = WiFi.softAPIP();
      dns_server_.start(DNS_PORT, "*", ap_ip);
      state_.dns_active = true;
    }

    void stop_captive_portal()
    {
      if (state_.dns_active)
      {
        dns_server_.stop();
        state_.dns_active = false;
      }
    }

    void append_json_escaped(String &dest, const String &value)
    {
      for (size_t i = 0; i < value.length(); ++i)
      {
        char c = value[i];
        switch (c)
        {
        case '\\':
        case '"':
          dest += '\\';
          dest += c;
          break;
        case '\b':
          dest += F("\\b");
          break;
        case '\f':
          dest += F("\\f");
          break;
        case '\n':
          dest += F("\\n");
          break;
        case '\r':
          dest += F("\\r");
          break;
        case '\t':
          dest += F("\\t");
          break;
        default:
          if (static_cast<uint8_t>(c) < 0x20)
          {
            char buffer[7];
            snprintf(buffer, sizeof(buffer), "\\\\u%04x", static_cast<unsigned>(static_cast<uint8_t>(c)));
            dest += buffer;
          }
          else
          {
            dest += c;
          }
          break;
        }
      }
    }

    void dispatch_wifi_state(const String &state_value, const String &ssid, const String &message)
    {
      if (!callbacks_.dispatch_transport_json)
      {
        return;
      }

      String payload = F("{\"event\":\"wifi_state\",\"state\":\"");
      append_json_escaped(payload, state_value);
      payload += F("\"");

      if (!ssid.isEmpty())
      {
        payload += F(",\"ssid\":\"");
        append_json_escaped(payload, ssid);
        payload += F("\"");
      }

      if (!message.isEmpty())
      {
        payload += F(",\"message\":\"");
        append_json_escaped(payload, message);
        payload += F("\"");
      }

      payload += F("}");
      callbacks_.dispatch_transport_json(payload.c_str());
    }

    void publish_wifi_state_locked(const char *state_value, const char *ssid, const char *message)
    {
      String next_state = state_value ? String(state_value) : String();
      if (next_state.isEmpty())
      {
        if (ssid)
        {
          state_.last_ssid = String(ssid);
        }
        if (message)
        {
          state_.last_message = String(message);
        }
        state_.last_state = "";
        return;
      }

      String next_ssid = ssid ? String(ssid) : state_.last_ssid;
      if (!ssid && next_state == "connected")
      {
        String current_ssid = WiFi.SSID();
        if (current_ssid.length())
        {
          next_ssid = current_ssid;
        }
      }

      String next_message = message ? String(message) : state_.last_message;

      if (next_state == state_.last_state && next_ssid == state_.last_ssid && next_message == state_.last_message)
      {
        return;
      }

      state_.last_state = next_state;
      state_.last_ssid = next_ssid;
      state_.last_message = next_message;
      dispatch_wifi_state(state_.last_state, state_.last_ssid, state_.last_message);
    }

    void publish_wifi_state(const char *state_value, const char *ssid = nullptr, const char *message = nullptr)
    {
      WifiStateLock lock = lock_state();
      publish_wifi_state_locked(state_value, ssid, message);
    }

    void send_cached_wifi_state_locked()
    {
      if (state_.last_state.isEmpty())
      {
        return;
      }
      dispatch_wifi_state(state_.last_state, state_.last_ssid, state_.last_message);
    }

    void append_wifi_state_json_locked(JsonVariant doc)
    {
      if (doc.isNull())
      {
        return;
      }

      doc["state"] = state_.last_state.isEmpty() ? "" : state_.last_state;
      if (!state_.last_ssid.isEmpty())
      {
        doc["ssid"] = state_.last_ssid;
      }
      else
      {
        doc.remove("ssid");
      }

      if (!state_.last_message.isEmpty())
      {
        doc["message"] = state_.last_message;
      }
      else
      {
        doc.remove("message");
      }
    }

    bool ensure_wifi_started()
    {
      wifi_mode_t current_mode = WIFI_MODE_NULL;
      if (esp_wifi_get_mode(&current_mode) == ESP_OK)
      {
        state_.current_mode = current_mode;
      }

      if (state_.current_mode == WIFI_MODE_NULL)
      {
        return false;
      }

      esp_err_t err = esp_wifi_start();
      if (err == ESP_OK || err == ESP_ERR_WIFI_STATE)
      {
        return true;
      }

      if (err == ESP_ERR_WIFI_NOT_INIT)
      {
        if (!WiFi.mode(state_.current_mode))
        {
          state_.current_mode = WIFI_MODE_NULL;
          return false;
        }
        err = esp_wifi_start();
        return err == ESP_OK || err == ESP_ERR_WIFI_STATE;
      }

      return false;
    }

    void set_wifi_mode(wifi_mode_t mode)
    {
      if (state_.current_mode == mode)
      {
        return;
      }

      bool success = false;
      if (state_.current_mode == WIFI_MODE_NULL)
      {
        success = WiFi.mode(mode);
      }
      else
      {
        esp_err_t err = esp_wifi_set_mode(mode);
        if (err == ESP_OK)
        {
          success = true;
        }
        else
        {
          success = WiFi.mode(mode);
        }
      }

      if (!success)
      {
        wifi_mode_t actual = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&actual) == ESP_OK)
        {
          state_.current_mode = actual;
        }
        else
        {
          state_.current_mode = WIFI_MODE_NULL;
        }
        return;
      }

      wifi_mode_t actual = WIFI_MODE_NULL;
      if (esp_wifi_get_mode(&actual) == ESP_OK)
      {
        state_.current_mode = actual;
      }
      else
      {
        state_.current_mode = mode;
      }
    }

    bool is_ap_mode(wifi_mode_t mode)
    {
      return mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
    }

    void ensure_ap_only_mode()
    {
      state_.temporary_apsta_mode = false;
      state_.target_mode = WIFI_MODE_AP;
      set_wifi_mode(WIFI_MODE_AP);
    }

    void ensure_sta_only_mode()
    {
      state_.temporary_apsta_mode = false;
      state_.target_mode = WIFI_MODE_STA;
      set_wifi_mode(WIFI_MODE_STA);
    }

    void shutdown_access_point()
    {
      stop_captive_portal();
      WiFi.softAPdisconnect(true);
      ensure_sta_only_mode();
      state_.configuration_mode = false;
    }

    void request_ap_sta_mode(bool temporary)
    {
      if (temporary)
      {
        state_.mode_before_temporary = state_.current_mode;
        if (state_.mode_before_temporary == WIFI_MODE_STA)
        {
          state_.target_mode = WIFI_MODE_STA;
        }
        else
        {
          state_.target_mode = WIFI_MODE_AP;
        }
      }

      state_.temporary_apsta_mode = temporary;
      set_wifi_mode(WIFI_MODE_APSTA);
    }

    void restore_ap_mode_after_temporary_sta()
    {
      if (!state_.temporary_apsta_mode)
      {
        return;
      }

      state_.temporary_apsta_mode = false;
      if (state_.mode_before_temporary == WIFI_MODE_STA)
      {
        ensure_sta_only_mode();
      }
      else
      {
        ensure_ap_only_mode();
      }
    }

    void schedule_sta_only_transition()
    {
      if (state_.current_mode != WIFI_MODE_APSTA || state_.temporary_apsta_mode)
      {
        return;
      }
      state_.ap_shutdown_pending = true;
      state_.ap_shutdown_deadline = millis() + WIFI_AP_SHUTDOWN_DELAY_MS;
      state_.target_mode = WIFI_MODE_STA;
    }

    void finalize_sta_only_transition()
    {
      if (!state_.ap_shutdown_pending)
      {
        return;
      }
      state_.ap_shutdown_pending = false;
      shutdown_access_point();
    }

    bool connect_to_station_internal(const String &ssid, const String &password, bool keep_ap_active)
    {
      if (ssid.isEmpty())
      {
        return false;
      }

      publish_wifi_state("connecting", ssid.c_str());

      bool restore_ap_after_unlock = false;
      bool start_failed = false;

      {
        WifiStateLock lock = lock_state();
        state_.ap_shutdown_pending = false;
        if (keep_ap_active)
        {
          request_ap_sta_mode(false);
        }
        else
        {
          shutdown_access_point();
        }
        WiFi.persistent(false);
        WiFi.setAutoReconnect(true);

        if (!ensure_wifi_started())
        {
          if (keep_ap_active)
          {
            // Restore AP state when we fail to start the STA interface.
            restore_ap_after_unlock = true;
          }
          else
          {
            WiFi.disconnect();
          }
          publish_wifi_state_locked("failed", ssid.c_str(), "Failed to start WiFi");
          start_failed = true;
        }
        else
        {
          state_.sta_connect_in_progress = true;
        }
      }

      if (start_failed)
      {
        if (restore_ap_after_unlock)
        {
          start_ap();
        }
        return false;
      }

      WiFi.begin(ssid.c_str(), password.c_str());

      unsigned long start_attempt = millis();
      bool connected = false;
      while ((millis() - start_attempt) < WIFI_CONNECT_TIMEOUT_MS)
      {
        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED)
        {
          connected = true;
          break;
        }
        if (status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST || status == WL_DISCONNECTED)
        {
          WiFi.reconnect();
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
      }

      {
        WifiStateLock lock = lock_state();
        state_.sta_connect_in_progress = false;
        if (connected && !keep_ap_active)
        {
          state_.configuration_mode = false;
        }
      }

      if (!connected)
      {
        if (keep_ap_active)
        {
          esp_wifi_disconnect();
          start_ap();
        }
        else
        {
          WiFi.disconnect();
        }
        publish_wifi_state("failed", ssid.c_str(), "Connection timed out");
        return false;
      }

      if (!invoke_save_credentials(ssid, password))
      {
        if (callbacks_.send_status_error)
        {
          callbacks_.send_status_error("Failed to save WiFi credentials");
        }
        publish_wifi_state("failed", ssid.c_str(), "Failed to save WiFi credentials");
        if (keep_ap_active)
        {
          WifiStateLock lock = lock_state();
          state_.ap_shutdown_pending = false;
          state_.target_mode = WIFI_MODE_APSTA;
          state_.current_mode = WIFI_MODE_APSTA;
        }
        return false;
      }

      if (!keep_ap_active)
      {
        WifiStateLock lock = lock_state();
        stop_captive_portal();
      }

      if (callbacks_.send_event)
      {
        callbacks_.send_event("wifi_sta_connected", ssid.c_str());
      }
      publish_wifi_state("connected", ssid.c_str());
      if (!keep_ap_active)
      {
        WifiStateLock lock = lock_state();
        schedule_sta_only_transition();
      }
      return true;
    }

    void wifi_connect_task(void *param)
    {
      (void)param;

      for (;;)
      {
        WifiConnectRequest request = {};
        if (wifi_connect_request_queue_ &&
            xQueueReceive(wifi_connect_request_queue_, &request, portMAX_DELAY) == pdPASS)
        {
          String ssid = String(request.ssid);
          String password = String(request.password);
          bool keep_ap_active = request.keep_ap_active;

          wifi_connect_busy_.store(true);
          vTaskDelay(pdMS_TO_TICKS(500));
          bool connected = connect_to_station_internal(ssid, password, keep_ap_active);
          wifi_connect_busy_.store(false);
          if (!connected && !keep_ap_active)
          {
            start_ap();
            if (is_configuration_mode() && callbacks_.send_event)
            {
              callbacks_.send_event("wifi_config_mode", nullptr);
            }
          }
        }
      }
    }

    bool ensure_wifi_connect_task()
    {
      if (!wifi_connect_request_queue_)
      {
        wifi_connect_request_queue_ = xQueueCreate(1, sizeof(WifiConnectRequest));
      }

      if (!wifi_connect_request_queue_)
      {
        return false;
      }

      if (!wifi_connect_task_handle_)
      {
        constexpr uint32_t stack_size = 4096;
        constexpr UBaseType_t priority = tskIDLE_PRIORITY + 1;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
        if (xTaskCreate(wifi_connect_task, "wifi_connect", stack_size, nullptr, priority, &wifi_connect_task_handle_) != pdPASS)
#else
        if (xTaskCreatePinnedToCore(wifi_connect_task,
                                    "wifi_connect",
                                    stack_size,
                                    nullptr,
                                    priority,
                                    &wifi_connect_task_handle_,
                                    kWifiTaskCore) != pdPASS)
#endif
        {
          wifi_connect_task_handle_ = nullptr;
          return false;
        }
      }

      return true;
    }

    bool schedule_connect_internal(const String &ssid, const String &password, bool keep_ap_active)
    {
      if (ssid.isEmpty())
      {
        return false;
      }

      if (wifi_connect_busy_.load())
      {
        return false;
      }

      if (!ensure_wifi_connect_task())
      {
        return false;
      }

      if (wifi_connect_request_queue_ && uxQueueMessagesWaiting(wifi_connect_request_queue_) > 0)
      {
        return false;
      }

      WifiConnectRequest request = {};
      request.keep_ap_active = keep_ap_active;
      String limited_ssid = ssid;
      if (limited_ssid.length() > WIFI_MAX_SSID_LENGTH)
      {
        limited_ssid = limited_ssid.substring(0, static_cast<int>(WIFI_MAX_SSID_LENGTH));
      }
      limited_ssid.toCharArray(request.ssid, sizeof(request.ssid));

      String limited_password = password;
      if (limited_password.length() > WIFI_MAX_PASSWORD_LENGTH)
      {
        limited_password = limited_password.substring(0, static_cast<int>(WIFI_MAX_PASSWORD_LENGTH));
      }
      limited_password.toCharArray(request.password, sizeof(request.password));

      return xQueueSend(wifi_connect_request_queue_, &request, 0) == pdPASS;
    }

    const char *wifi_auth_mode_to_string(wifi_auth_mode_t mode)
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

    bool scan_networks_internal(JsonArray results, String &error_message, int &error_code)
    {
      WifiStateLock lock = lock_state();
      request_ap_sta_mode(true);

      if (!ensure_wifi_started())
      {
        restore_ap_mode_after_temporary_sta();
        error_message = "WiFi interface not ready";
        error_code = 0;
        return false;
      }

      int16_t count = WiFi.scanNetworks(false, false, false);
      if (count < 0)
      {
        restore_ap_mode_after_temporary_sta();
        error_message = "Scan failed";
        error_code = count;
        return false;
      }

      const size_t max_networks = 20;
      for (int16_t index = 0; index < count && index < static_cast<int16_t>(max_networks); ++index)
      {
        JsonObject network = results.add<JsonObject>();
        network["ssid"] = WiFi.SSID(index);
        network["rssi"] = WiFi.RSSI(index);
        network["channel"] = WiFi.channel(index);
        network["auth"] = wifi_auth_mode_to_string(static_cast<wifi_auth_mode_t>(WiFi.encryptionType(index)));
      }

      WiFi.scanDelete();
      restore_ap_mode_after_temporary_sta();
      return true;
    }
  } // namespace

  void init(const Callbacks &callbacks)
  {
    callbacks_ = callbacks;
    if (!callbacks_.dispatch_transport_json)
    {
      callbacks_.dispatch_transport_json = [](const char *) {};
    }
    ensure_state_mutex();
    WifiStateLock lock = lock_state();
    state_ = WifiManagerState();
  }

  bool schedule_connect(const String &ssid, const String &password, bool keep_ap_active)
  {
    return schedule_connect_internal(ssid, password, keep_ap_active);
  }

  bool connect_saved_credentials()
  {
    String ssid;
    String password;
    if (!invoke_load_credentials(ssid, password))
    {
      return false;
    }
    if (ssid.isEmpty())
    {
      return false;
    }
    return connect_to_station_internal(ssid, password, false);
  }

  void start_ap()
  {
    WifiStateLock lock = lock_state();
    state_.ap_shutdown_pending = false;
    ensure_ap_only_mode();

    auto report_failure = [](const char *message) {
      stop_captive_portal();
      WiFi.softAPdisconnect(true);
      state_.configuration_mode = false;
      publish_wifi_state_locked("failed", nullptr, message);
      if (callbacks_.send_status_error)
      {
        callbacks_.send_status_error(message);
      }
    };

    if (!ensure_wifi_started())
    {
      report_failure("Failed to start WiFi for access point");
      return;
    }

    IPAddress local_ip(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);

    if (!WiFi.softAPConfig(local_ip, gateway, subnet))
    {
      report_failure("Failed to configure access point network");
      return;
    }

    if (!WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD))
    {
      report_failure("Failed to start access point");
      return;
    }

    start_captive_portal();
    state_.configuration_mode = true;
    publish_wifi_state_locked("ap", CONFIG_AP_SSID, nullptr);
  }

  void stop_ap()
  {
    WifiStateLock lock = lock_state();
    state_.ap_shutdown_pending = false;
    shutdown_access_point();
    publish_wifi_state_locked("idle", nullptr, nullptr);
  }

  bool is_configuration_mode()
  {
    WifiStateLock lock = lock_state();
    return state_.configuration_mode;
  }

  void append_state_json(JsonVariant doc)
  {
    WifiStateLock lock = lock_state();
    append_wifi_state_json_locked(doc);
  }

  void send_cached_state()
  {
    WifiStateLock lock = lock_state();
    send_cached_wifi_state_locked();
  }

  void process()
  {
    WifiStateLock lock = lock_state();
    if (state_.ap_shutdown_pending && millis() >= state_.ap_shutdown_deadline)
    {
      finalize_sta_only_transition();
    }
  }

  void process_dns()
  {
    WifiStateLock lock = lock_state();
    if (state_.dns_active)
    {
      dns_server_.processNextRequest();
    }
  }

  void on_event(WiFiEvent_t event, WiFiEventInfo_t info)
  {
    WifiStateLock lock = lock_state();
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 2
    switch (event)
    {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      state_.sta_connect_in_progress = false;
      publish_wifi_state_locked("connected", WiFi.SSID().c_str());
      schedule_sta_only_transition();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    {
      state_.sta_connect_in_progress = false;
      uint8_t reason = info.wifi_sta_disconnected.reason;
      String message = F("Disconnect reason ");
      message += String(reason);
      publish_wifi_state_locked("failed", nullptr, message.c_str());
      break;
    }
    default:
      break;
    }
#else
    switch (event)
    {
    case SYSTEM_EVENT_STA_GOT_IP:
      state_.sta_connect_in_progress = false;
      publish_wifi_state_locked("connected", WiFi.SSID().c_str());
      schedule_sta_only_transition();
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
    {
      state_.sta_connect_in_progress = false;
      uint8_t reason = info.disconnected.reason;
      String message = F("Disconnect reason ");
      message += String(reason);
      publish_wifi_state_locked("failed", nullptr, message.c_str());
      break;
    }
    default:
      break;
    }
#endif
  }

  bool scan_networks(JsonArray results, String &error_message, int &error_code)
  {
    return scan_networks_internal(results, error_message, error_code);
  }
} // namespace wifi_manager

