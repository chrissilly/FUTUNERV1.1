#include "feature_manager.h"
#include "feature_manager_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * feature_manager — see feature_manager.h for the public contract and
 * FUTV1.1/CLAUDE.md for the project-level ON/OFF discipline this module
 * enforces.
 *
 * Concurrency model:
 *   The manager mutex is held across the FULL duration of a swap —
 *   stop() + is_running() poll + start(). That means a concurrent
 *   feature_manager_active() call can stall up to roughly
 *   FEATURE_MGR_STOP_TIMEOUT_MS during a user-initiated swap. Acceptable
 *   for a single-user dongle (option 3a per the kickoff review).
 *
 *   TODO(swap-stall): if real-world telemetry ever shows the stall is
 *   user-visible, switch to a transitioning-flag scheme that releases
 *   the lock during the long is_running() poll.
 */

static const char *TAG = "FEATURE_MGR";

static const feature_descriptor_t *s_registry[FEATURE_MGR_MAX_REGISTERED];
static feature_id_t  s_active      = FEATURE_NONE;
static SemaphoreHandle_t s_mutex   = NULL;
static bool          s_initialized = false;

/* ---------------------------------------------------------------------- */
/* Internal helpers                                                       */
/* ---------------------------------------------------------------------- */

static inline bool id_in_range(feature_id_t id) {
    return id > FEATURE_NONE && id < FEATURE_COUNT;
}

static void set_err(char *err_out, size_t err_len, const char *fmt, ...) {
    if (err_out == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_out, err_len, fmt, ap);
    va_end(ap);
}

static esp_err_t take_mutex(void) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(FEATURE_MGR_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "could not acquire feature manager mutex within timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void give_mutex(void) {
    xSemaphoreGive(s_mutex);
}

/* Wait for `desc->is_running()` to return false, bounded by
 * FEATURE_MGR_STOP_TIMEOUT_MS. Returns ESP_OK on clean stop,
 * ESP_ERR_TIMEOUT if the feature is still running at deadline. */
static esp_err_t wait_for_stopped(const feature_descriptor_t *desc) {
    const TickType_t poll = pdMS_TO_TICKS(FEATURE_MGR_STOP_POLL_INTERVAL_MS);
    const TickType_t budget = pdMS_TO_TICKS(FEATURE_MGR_STOP_TIMEOUT_MS);
    TickType_t waited = 0;
    while (desc->is_running()) {
        if (waited >= budget) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(poll);
        waited += poll;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

esp_err_t feature_manager_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    memset(s_registry, 0, sizeof(s_registry));
    s_active = FEATURE_NONE;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "feature manager initialized; no feature active");
    return ESP_OK;
}

esp_err_t feature_manager_register(const feature_descriptor_t *desc) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (desc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!id_in_range(desc->id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (desc->name == NULL || desc->start == NULL || desc->stop == NULL || desc->is_running == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = take_mutex();
    if (rc != ESP_OK) {
        return rc;
    }

    if (s_registry[desc->id] != NULL) {
        give_mutex();
        ESP_LOGW(TAG, "feature id %d (%s) already registered; rejecting duplicate",
                 (int)desc->id, desc->name);
        return ESP_ERR_INVALID_STATE;
    }
    s_registry[desc->id] = desc;
    ESP_LOGI(TAG, "registered feature id=%d name=%s", (int)desc->id, desc->name);
    give_mutex();
    return ESP_OK;
}

esp_err_t feature_manager_request_start(feature_id_t id, char *err_out, size_t err_len) {
    if (err_out != NULL && err_len > 0) {
        err_out[0] = '\0';
    }

    if (!s_initialized) {
        set_err(err_out, err_len, "feature manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!id_in_range(id)) {
        set_err(err_out, err_len, "invalid feature id %d", (int)id);
        ESP_LOGW(TAG, "request_start rejected: invalid id %d", (int)id);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = take_mutex();
    if (rc != ESP_OK) {
        set_err(err_out, err_len, "feature manager busy (mutex acquire failed)");
        return rc;
    }

    const feature_descriptor_t *target = s_registry[id];
    if (target == NULL) {
        give_mutex();
        set_err(err_out, err_len, "feature id %d is not registered", (int)id);
        ESP_LOGW(TAG, "request_start: feature %d not registered", (int)id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Idempotent: already active. */
    if (s_active == id) {
        ESP_LOGI(TAG, "request_start: feature %s already active (idempotent)", target->name);
        give_mutex();
        return ESP_OK;
    }

    /* Preempt path: another feature is active. */
    if (s_active != FEATURE_NONE) {
        const feature_descriptor_t *current = s_registry[s_active];
        ESP_LOGW(TAG, "preempting active feature %s for incoming %s",
                 current->name, target->name);

        esp_err_t stop_rc = current->stop();
        if (stop_rc != ESP_OK) {
            ESP_LOGE(TAG, "stop() of %s returned rc=%d; aborting swap, active stays %s",
                     current->name, (int)stop_rc, current->name);
            set_err(err_out, err_len,
                    "could not stop active feature %s (rc=%d); active unchanged",
                    current->name, (int)stop_rc);
            give_mutex();
            return stop_rc;
        }

        esp_err_t wait_rc = wait_for_stopped(current);
        if (wait_rc != ESP_OK) {
            ESP_LOGE(TAG, "%s did not finish stopping within timeout; aborting swap",
                     current->name);
            set_err(err_out, err_len,
                    "feature %s did not finish stopping within timeout; active unchanged",
                    current->name);
            give_mutex();
            return wait_rc;
        }
        ESP_LOGI(TAG, "%s stopped cleanly; ready to start %s",
                 current->name, target->name);
        s_active = FEATURE_NONE;
    }

    /* Fresh-start path: no active feature now. */
    esp_err_t start_rc = target->start();
    if (start_rc != ESP_OK) {
        ESP_LOGE(TAG, "start() of %s returned rc=%d", target->name, (int)start_rc);
        set_err(err_out, err_len,
                "could not start feature %s (rc=%d)", target->name, (int)start_rc);
        give_mutex();
        return start_rc;
    }
    s_active = id;
    ESP_LOGI(TAG, "active feature is now %s (id=%d)", target->name, (int)id);
    give_mutex();
    return ESP_OK;
}

esp_err_t feature_manager_request_stop(feature_id_t id) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (id == FEATURE_NONE) {
        return ESP_OK; /* explicit no-op per the documented contract */
    }
    if (id >= FEATURE_COUNT) {
        ESP_LOGW(TAG, "request_stop rejected: invalid id %d", (int)id);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = take_mutex();
    if (rc != ESP_OK) {
        return rc;
    }

    if (s_active != id) {
        /* Not active — silently OK. */
        give_mutex();
        return ESP_OK;
    }

    const feature_descriptor_t *current = s_registry[id];
    if (current == NULL) {
        /* Should be impossible — active without a registered descriptor. */
        ESP_LOGE(TAG, "internal: active feature %d has no registered descriptor", (int)id);
        s_active = FEATURE_NONE;
        give_mutex();
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "stopping active feature %s", current->name);
    esp_err_t stop_rc = current->stop();
    if (stop_rc != ESP_OK) {
        ESP_LOGE(TAG, "stop() of %s returned rc=%d; active unchanged",
                 current->name, (int)stop_rc);
        give_mutex();
        return stop_rc;
    }
    s_active = FEATURE_NONE;
    ESP_LOGI(TAG, "feature %s stopped; no feature active", current->name);
    give_mutex();
    return ESP_OK;
}

feature_id_t feature_manager_active(void) {
    if (!s_initialized) {
        return FEATURE_NONE;
    }
    if (take_mutex() != ESP_OK) {
        return FEATURE_NONE;
    }
    feature_id_t id = s_active;
    give_mutex();
    return id;
}

const char *feature_manager_active_name(void) {
    if (!s_initialized) {
        return "uninitialized";
    }
    if (take_mutex() != ESP_OK) {
        return "(busy)";
    }
    const char *name;
    if (s_active == FEATURE_NONE) {
        name = "none";
    } else {
        const feature_descriptor_t *d = s_registry[s_active];
        name = (d != NULL && d->name != NULL) ? d->name : "unknown";
    }
    give_mutex();
    return name;
}
