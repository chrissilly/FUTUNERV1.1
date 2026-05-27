#include "wot_logger.h"
#include "wot_recorder.h"
#include "wot_uploader.h"
#include "wot_logger_config.h"

#include "feature_manager.h"
#include "esp_log.h"

#ifndef WOT_LOGGER_HOST_BUILD
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"
#  include "esp_http_client.h"
#  include "cloud/cloud_client.h"
#  include "wifi/wifi_ap.h"
#  include "logger/logger_manager.h"
#  include "logger/logger_profile.h"
#  include "nvs/nvs_manager.h"
#  include <sys/stat.h>
#  include <dirent.h>
#  include <stdio.h>
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// wot_logger — see wot_logger.h. Owns the recorder + uploader sub-
// modules, the on-target HTTP and FS adapters, and the feature_manager
// descriptor. No detection logic lives here; the recorder owns that.
//
// Init flow (called once from main.c after feature_manager_init and
// logger_manager_init):
//   1. wot_recorder_init   — sample buffer + gzip output buffer
//   2. wot_uploader_init   — queue stats refresh + URL resolution
//   3. wot_logger_register_with_feature_manager — descriptor handed
//      to the arbiter; defaults to NOT-running (per ON/OFF rule).
//
// start() arms recorder, starts uploader. stop() disarms recorder
// (which flushes any in-progress recording), stops uploader.
// is_running() reflects the running-flag set by start/stop.

static const char *TAG = "WOT_LOG";

static bool s_running = false;
static bool s_initialized = false;
/* P-28: tracks early-init completion (feature descriptor registered,
 * uploader initialized, on_apply callback registered). The recorder
 * init is deferred until logger_profile_apply() fires our callback,
 * at which point logger_manager has the variable list we need to
 * snapshot. s_initialized only flips true after the late init lands. */
static bool s_early_initialized = false;
static char s_resolved_url[WOT_UPLOAD_URL_MAX];

// ------------------------------------------------------------------
// On-target adapters (compiled out in host test build)
// ------------------------------------------------------------------

#ifndef WOT_LOGGER_HOST_BUILD

static uint32_t target_clock_now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool target_wifi_ready(void) {
    return wifi_client_is_connected();
}

// HTTP POST adapter using esp_http_client. Returns the HTTP status
// code on transport success, or a negative esp_err_t on transport
// failure (DNS, TLS, connect, timeout). The uploader only treats
// HTTP_OK_MIN..MAX as "delete the file" — anything else (negative
// or non-2xx) keeps the file queued.
static int target_http_post(const char    *url,
                            const uint8_t *body,
                            size_t         body_len,
                            uint32_t       timeout_ms,
                            void          *user_ctx) {
    (void)user_ctx;
    /* P-49: TLS config centralized in cloud_client. */
    esp_http_client_handle_t client = cloud_client_https_init(url, HTTP_METHOD_POST, (int)timeout_ms);
    if (client == NULL) {
        return (int)ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/gzip");
    esp_http_client_set_post_field(client, (const char *)body, (int)body_len);
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK)
        ? (int)esp_http_client_get_status_code(client)
        : (int)err;
    esp_http_client_cleanup(client);
    return status;
}

static esp_err_t target_fs_write(const char *path, const uint8_t *data,
                                 size_t len, void *user_ctx) {
    (void)user_ctx;
    mkdir(WOT_QUEUE_DIR_PATH, (mode_t)WOT_QUEUE_DIR_MODE);
    FILE *f = fopen(path, "wb");
    if (f == NULL) return ESP_FAIL;
    size_t written = fwrite(data, (size_t)1, len, f);
    fclose(f);
    return (written == len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t target_fs_read(const char *path, uint8_t *out,
                                size_t *in_out_len, void *user_ctx) {
    (void)user_ctx;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return ESP_FAIL;
    size_t cap = *in_out_len;
    size_t read = fread(out, (size_t)1, cap, f);
    fclose(f);
    *in_out_len = read;
    return ESP_OK;
}

static esp_err_t target_fs_delete(const char *path, void *user_ctx) {
    (void)user_ctx;
    remove(path);
    return ESP_OK;
}

static DIR *s_target_dir = NULL;

static esp_err_t target_fs_iter(bool reset, char *out, size_t out_cap,
                                size_t *size, void *user_ctx) {
    (void)user_ctx;
    if (reset) {
        if (s_target_dir != NULL) {
            closedir(s_target_dir);
            s_target_dir = NULL;
        }
        s_target_dir = opendir(WOT_QUEUE_DIR_PATH);
        if (s_target_dir == NULL) return ESP_ERR_NOT_FOUND;
    }
    if (s_target_dir == NULL) return ESP_ERR_NOT_FOUND;
    struct dirent *de;
    while ((de = readdir(s_target_dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        // Bound the basename to WOT_QUEUE_FILENAME_MAX up front so
        // the subsequent path concatenation cannot truncate.
        char trimmed[WOT_QUEUE_FILENAME_MAX];
        strncpy(trimmed, de->d_name, sizeof(trimmed) - (size_t)1);
        trimmed[sizeof(trimmed) - (size_t)1] = '\0';
        strncpy(out, trimmed, out_cap - (size_t)1);
        out[out_cap - (size_t)1] = '\0';
        char full[WOT_QUEUE_FULL_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", WOT_QUEUE_DIR_PATH, trimmed);
        struct stat st;
        if (stat(full, &st) == 0) {
            *size = (size_t)st.st_size;
        } else {
            *size = 0;
        }
        return ESP_OK;
    }
    closedir(s_target_dir);
    s_target_dir = NULL;
    return ESP_ERR_NOT_FOUND;
}

// On-finish: hand bytes from recorder to uploader queue.
static void target_on_finish(const uint8_t *gzip_buf, size_t gzip_len, void *ctx) {
    (void)ctx;
    uint32_t now = target_clock_now_ms();
    esp_err_t rc = wot_uploader_enqueue(gzip_buf, gzip_len, now);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "enqueue failed rc=%d; dropping recording", (int)rc);
    }
}

// Variable-name array used by the recorder's CSV header. Backed by
// logger_manager's name table. We snapshot pointers at start() time
// rather than rebuild every flush.
static const char *s_var_name_storage[WOT_RECORDER_MAX_VARS_PER_SAMPLE];
static const char * const *s_var_names = (const char * const *)s_var_name_storage;
static uint8_t s_var_count = 0;
static uint8_t s_trigger_index = WOT_RECORDER_TRIGGER_VAR_NONE;

static void snapshot_logger_profile(void) {
    s_var_count = logger_manager_get_variable_count();
    if (s_var_count > (uint8_t)WOT_RECORDER_MAX_VARS_PER_SAMPLE) {
        s_var_count = (uint8_t)WOT_RECORDER_MAX_VARS_PER_SAMPLE;
    }
    s_trigger_index = WOT_RECORDER_TRIGGER_VAR_NONE;
    for (uint8_t i = 0; i < s_var_count; i++) {
        s_var_name_storage[i] = logger_manager_get_variable_name(i);
        if (s_var_name_storage[i] != NULL &&
            strcmp(s_var_name_storage[i], WOT_TRIGGER_VARIABLE_NAME) == (int)0) {
            s_trigger_index = i;
        }
    }
}

// logger_manager-side data callback. Invoked after each ECU poll
// completes. Forwards into the recorder.
static void on_logger_data(const float *values, uint8_t count) {
    (void)count;
    wot_recorder_feed_sample(values);
}

// Resolve the upload URL: NVS override (host) + endpoint path, or
// default host + endpoint path if NVS not set.
static void resolve_upload_url(char *out, size_t cap) {
    char host[WOT_UPLOAD_URL_MAX];
    if (nvs_manager_load_string(WOT_UPLOAD_HOST_NVS_KEY, host, sizeof(host)) != ESP_OK ||
        host[0] == '\0') {
        strncpy(host, WOT_UPLOAD_DEFAULT_HOST, sizeof(host) - (size_t)1);
        host[sizeof(host) - (size_t)1] = '\0';
    }
    snprintf(out, cap, "%s%s", host, WOT_UPLOAD_ENDPOINT_PATH);
}

#endif // !WOT_LOGGER_HOST_BUILD

// ------------------------------------------------------------------
// Feature descriptor
// ------------------------------------------------------------------

static const feature_descriptor_t k_descriptor = {
    .id         = FEATURE_WOT_LOGGING,
    .name       = "wot_logger",
    .start      = wot_logger_start,
    .stop       = wot_logger_stop,
    .is_running = wot_logger_is_running,
};

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

esp_err_t wot_logger_register_with_feature_manager(void) {
    esp_err_t rc = feature_manager_register(&k_descriptor);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        // INVALID_STATE = already registered → idempotent; treat as OK.
        return rc;
    }
    return ESP_OK;
}

#ifndef WOT_LOGGER_HOST_BUILD
/* P-28: late-init callback fired by logger_profile_apply() once
 * logger_manager has been populated with the boxcode's variables.
 * snapshot_logger_profile() now sees a non-zero var_count, so
 * wot_recorder_init succeeds and FEATURE_WOT_LOGGING becomes fully
 * usable. Earlier impl ran this whole chain at boot from main.c
 * before the ECU had even been discovered, which is why
 * wot_recorder_init failed with var_count=0 and the feature never
 * registered. Idempotent — subsequent fires no-op once s_initialized. */
static void wot_logger_on_logger_profile_applied(const char *boxcode) {
    (void)boxcode;
    if (s_initialized) return;
    if (!s_early_initialized) {
        ESP_LOGW(TAG, "on_logger_profile_applied before early init — ignoring");
        return;
    }
    snapshot_logger_profile();
    if (s_var_count == 0) {
        ESP_LOGW(TAG, "on_logger_profile_applied: logger_manager has 0 vars — "
                      "recorder init deferred until next apply");
        return;
    }
    wot_recorder_config_t rec_cfg = {
        .clock_now_ms          = target_clock_now_ms,
        .on_finish             = target_on_finish,
        .user_ctx              = NULL,
        .trigger_var_index     = s_trigger_index,
        .variables_per_sample  = s_var_count,
        .variable_names        = s_var_names,
    };
    esp_err_t rc = wot_recorder_init(&rec_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "recorder init rc=%d (will retry on next profile apply)", (int)rc);
        return;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "WOT_LOG: recorder init OK (vars=%u, trigger=%s)",
             (unsigned)s_var_count, WOT_TRIGGER_VARIABLE_NAME);
}
#endif

esp_err_t wot_logger_init(void) {
    if (s_initialized || s_early_initialized) {
        return ESP_OK;
    }

#ifdef WOT_LOGGER_HOST_BUILD
    // Host build: the test harness wires recorder+uploader directly
    // and skips this init entirely (the lifecycle layer's responsibility
    // is target-specific resource setup). Still register the feature
    // descriptor so feature_manager interop is exercised.
    esp_err_t rc = wot_logger_register_with_feature_manager();
    if (rc != ESP_OK) return rc;
    s_initialized = true;
    return ESP_OK;
#else
    /* P-28: split init into early (boot-time) and late (post logger
     * profile apply) phases. Early registers the feature descriptor
     * and the uploader, neither of which depend on logger variables.
     * Late init is in wot_logger_on_logger_profile_applied(). */

    esp_err_t rc = wot_logger_register_with_feature_manager();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "feature_manager register rc=%d", (int)rc);
        return rc;
    }

    resolve_upload_url(s_resolved_url, sizeof(s_resolved_url));

    wot_uploader_config_t up_cfg = {
        .http       = { .post = target_http_post, .user_ctx = NULL },
        .fs         = {
            .write_file = target_fs_write,
            .read_file  = target_fs_read,
            .delete_file = target_fs_delete,
            .iter_next  = target_fs_iter,
            .user_ctx   = NULL,
        },
        .wifi_ready = target_wifi_ready,
        .upload_url = s_resolved_url,
    };
    rc = wot_uploader_init(&up_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "uploader init rc=%d", (int)rc);
        return rc;
    }

    rc = logger_profile_register_on_apply(wot_logger_on_logger_profile_applied);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "logger_profile_register_on_apply rc=%d "
                      "(recorder init will be skipped)", (int)rc);
    }

    s_early_initialized = true;
    ESP_LOGI(TAG, "wot_logger early init OK (url=%s, trigger_var=%s); "
                  "recorder init deferred to logger profile apply",
             s_resolved_url, WOT_TRIGGER_VARIABLE_NAME);
    return ESP_OK;
#endif
}

esp_err_t wot_logger_start(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        return ESP_OK;
    }
    esp_err_t rc = wot_recorder_arm();
    if (rc != ESP_OK) return rc;
    rc = wot_uploader_start();
    if (rc != ESP_OK) {
        wot_recorder_disarm();
        return rc;
    }
#ifndef WOT_LOGGER_HOST_BUILD
    logger_manager_set_data_callback(on_logger_data);
#endif
    s_running = true;
    ESP_LOGI(TAG, "wot_logger started");
    return ESP_OK;
}

esp_err_t wot_logger_stop(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_running) {
        return ESP_OK;
    }
#ifndef WOT_LOGGER_HOST_BUILD
    logger_manager_set_data_callback(NULL);
#endif
    wot_recorder_disarm();
    wot_uploader_stop();
    s_running = false;
    ESP_LOGI(TAG, "wot_logger stopped");
    return ESP_OK;
}

bool wot_logger_is_running(void) {
    return s_running;
}

/* P-66: drive the uploader's periodic tick. wot_uploader_tick() is the
 * only thing that actually attempts a queued-log upload, and nothing
 * in production was calling it (the "call at 1 Hz" contract in
 * wot_uploader.h had no caller — the tick was exercised only by the
 * host unit test). Wire this into the main can_task loop so the
 * uploader can drain the queue while the WOT feature is active.
 * Safe to call unconditionally: wot_uploader_tick() no-ops when the
 * uploader is not running and self-rate-limits to
 * WOT_UPLOAD_RETRY_INTERVAL_MS internally. */
void wot_logger_tick(void) {
#ifndef WOT_LOGGER_HOST_BUILD
    wot_uploader_tick(target_clock_now_ms());
#endif
}
