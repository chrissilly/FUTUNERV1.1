/* Host shim for freertos/task.h — vTaskDelay is a no-op so the
 * stop-and-wait poll loop in feature_manager.c terminates immediately
 * in tests via the budget counter rather than via wall clock. */
#ifndef HOST_SHIM_TASK_H
#define HOST_SHIM_TASK_H

#include "freertos/FreeRTOS.h"

void       vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);

#endif /* HOST_SHIM_TASK_H */
