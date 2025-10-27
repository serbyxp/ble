#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

namespace wifi_manager
{
  struct Callbacks
  {
    void (*dispatch_transport_json)(const char *payload) = nullptr;
    void (*send_status_error)(const char *message) = nullptr;
    void (*send_event)(const char *name, const char *detail) = nullptr;
    bool (*load_credentials)(String &ssid, String &password) = nullptr;
    bool (*save_credentials)(const String &ssid, const String &password) = nullptr;
  };

  void init(const Callbacks &callbacks);

  bool schedule_connect(const String &ssid, const String &password, bool keep_ap_active);
  bool connect_saved_credentials();
  void start_ap();
  void stop_ap();

  bool is_configuration_mode();
  void append_state_json(JsonVariant doc);
  void send_cached_state();

  void process();
  void process_dns();

  void on_event(WiFiEvent_t event, WiFiEventInfo_t info);

  bool scan_networks(JsonArray results, String &error_message, int &error_code);
} // namespace wifi_manager

