#ifndef HOST_SHIM_TASK_H
#define HOST_SHIM_TASK_H
#include "freertos/FreeRTOS.h"
void       vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
#endif
