#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

void uartTransportBegin(QueueHandle_t queue);
void uartTransportLoop();
void uartTransportHandleConfigChange(bool uartSettingsChanged);
