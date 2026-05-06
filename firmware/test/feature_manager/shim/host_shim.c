/* Host shim implementation for FreeRTOS primitives used by the
 * feature manager. Single-threaded: the toy mutex is just a held flag,
 * which is enough to confirm the manager balances takes/gives correctly
 * and never re-enters itself. Real concurrency is verified on-target. */

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
    if (s->held) {
        /* Single-threaded test: a re-entrant take indicates a bug.
         * Return failure to surface it. */
        return pdFALSE;
    }
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
    /* No-op on host: the manager's stop-and-wait loop bounds itself
     * via FEATURE_MGR_STOP_TIMEOUT_MS / FEATURE_MGR_STOP_POLL_INTERVAL_MS,
     * not via wall-clock time. */
}

static TickType_t g_ticks;

TickType_t xTaskGetTickCount(void) {
    return ++g_ticks;
}
