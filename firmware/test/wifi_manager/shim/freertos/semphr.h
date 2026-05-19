#ifndef HOST_SHIM_SEMPHR_H
#define HOST_SHIM_SEMPHR_H

#include "freertos/FreeRTOS.h"

/* wifi_ap.c does not use a mutex itself, but the headers may pull this
 * include transitively. Provide a typedef so the include chain doesn't
 * fail; the functions are unused. */
typedef struct host_mutex *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);

#endif /* HOST_SHIM_SEMPHR_H */
