#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

void websocketTransportBegin(QueueHandle_t queue);
void websocketTransportLoop();
void websocketTransportBroadcast(const char *message);
String generateApSsid();
