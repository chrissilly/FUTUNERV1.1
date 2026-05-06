/* Host shim implementation: FreeRTOS mutex (toy single-threaded),
 * vTaskDelay no-op, tick counter monotonically increasing. Same
 * shape as firmware/test/wot_logger/shim/host_shim.c. */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stddef.h>

#define HOST_MUTEX_POOL_SIZE 8

struct host_mutex {
    int held;
    int in_use;
};

static struct host_mutex g_pool[HOST_MUTEX_POOL_SIZE];

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    for (int i = 0; i < HOST_MUTEX_POOL_SIZE; i++) {
        if (!g_pool[i].in_use) {
            g_pool[i].in_use = 1;
            g_pool[i].held = 0;
            return &g_pool[i];
        }
    }
    return NULL;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout) {
    (void)timeout;
    if (s == NULL) return pdFALSE;
    if (s->held) return pdFALSE;
    s->held = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (s == NULL) return pdFALSE;
    s->held = 0;
    return pdTRUE;
}

void vTaskDelay(TickType_t ticks) {
    (void)ticks;
}

static TickType_t g_ticks;

TickType_t xTaskGetTickCount(void) {
    return ++g_ticks;
}
