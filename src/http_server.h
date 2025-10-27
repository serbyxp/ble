#pragma once

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum class TransportMode : uint8_t
{
  Uart = 0,
  Websocket = 1
};

namespace http_server
{
  constexpr size_t kMaxTransportPayload = 512;

  struct TransportMessage
  {
    size_t length;
    char payload[kMaxTransportPayload];
  };

  struct Dependencies
  {
    QueueHandle_t *command_queue = nullptr;
    QueueHandle_t *event_queue = nullptr;
    bool (*ensure_transport_queues)() = nullptr;
    bool (*enqueue_transport_message)(QueueHandle_t queue, const char *data, size_t length) = nullptr;
    TransportMode (*get_active_transport_mode)() = nullptr;
    const char *(*transport_mode_to_string)(TransportMode mode) = nullptr;
    TransportMode (*string_to_transport_mode)(const char *value) = nullptr;
    void (*apply_uart_baud_rate)(uint32_t baud) = nullptr;
    uint32_t (*get_uart_baud_rate)() = nullptr;
    bool (*apply_transport_mode)(TransportMode mode) = nullptr;
    bool (*save_transport_config)(TransportMode mode, uint32_t baud) = nullptr;
    void (*send_status_error)(const char *message) = nullptr;
    void (*send_event)(const char *name, const char *detail) = nullptr;
    size_t input_buffer_limit = 0;
  };

  void init(const Dependencies &dependencies);
  void start();
  void stop();
  void close_active_websocket();
} // namespace http_server

