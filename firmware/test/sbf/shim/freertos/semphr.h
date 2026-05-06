#ifndef HOST_SHIM_SEMPHR_H
#define HOST_SHIM_SEMPHR_H
#include "freertos/FreeRTOS.h"
typedef struct host_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);
#endif
