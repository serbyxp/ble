#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

void websocketTransportBegin(QueueHandle_t queue);
void websocketTransportLoop();
void websocketTransportBroadcast(const char *message);
void websocketTransportEnd();
