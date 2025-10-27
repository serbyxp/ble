#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#ifdef ENABLE_WEBSOCKET_TRANSPORT

void websocketTransportBegin(QueueHandle_t queue);
void websocketTransportLoop();
void websocketTransportBroadcast(const char *message);

#else

inline void websocketTransportBegin(QueueHandle_t) {}
inline void websocketTransportLoop() {}
inline void websocketTransportBroadcast(const char *) {}

#endif
