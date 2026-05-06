/* Host shim for freertos/FreeRTOS.h — provides type/macro stubs so
 * feature_manager.c compiles on the dev machine. */
#ifndef HOST_SHIM_FREERTOS_H
#define HOST_SHIM_FREERTOS_H

#include <stdint.h>
#include <stdbool.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;

#define pdTRUE              ((BaseType_t)1)
#define pdFALSE             ((BaseType_t)0)
#define portMAX_DELAY       ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS  ((TickType_t)1)

/* On host, treat 1 ms == 1 tick. The test never blocks on real time;
 * vTaskDelay is a no-op (see host_shim.c). */
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))

#endif /* HOST_SHIM_FREERTOS_H */
