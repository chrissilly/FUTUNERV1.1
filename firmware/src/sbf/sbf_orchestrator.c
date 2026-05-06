#include "sbf_orchestrator.h"
#include "sbf_config.h"
#include "sbf_loader.h"
#include "sbf_applier.h"
#include "sbf_variants.h"
#include "sbf_downloader.h"
#include "license.h"
#include "feature_manager.h"

#include "esp_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// sbf_orchestrator — see sbf_orchestrator.h. State machine + small
// queue; license-gate at start; emits WS events on every meaningful
// transition. Worker drained by drain_one(); on target a FreeRTOS
// task loops over drain_one + a wait. On host the test calls
// drain_one directly.

static const char *TAG = "SBF_ORCH";

typedef enum {
    SBF_REQ_LOAD_AND_APPLY,   // load (+download if needed) then apply
    SBF_REQ_REAPPLY,          // re-apply with new params; SBF stays loaded
    SBF_REQ_UNLOAD,           // drop loaded SBF; transition to IDLE
} sbf_req_kind_t;

typedef struct {
    sbf_req_kind_t kind;
    uint8_t        stage;
    uint8_t        ethanol_pct;
} sbf_req_t;

typedef struct {
    bool                              initialized;
    bool                              fm_running;        // active in feature_manager?
    sbf_state_t                       state;

    // Config (deps).
    sbf_orchestrator_config_t         cfg;

    // Loaded SBF + provenance.
    sbf_loaded_t                     *loaded;
    uint8_t                           current_stage;
    uint8_t                           current_ethanol_pct;
    char                              sbf_filename[SBF_FILENAME_MAX];

    // Worker queue. Latest-wins for REAPPLY collapses; UNLOAD wins
    // over everything if enqueued.
    sbf_req_t                         queue[SBF_WORKER_QUEUE_DEPTH];
    size_t                            queue_len;

    // Diagnostics.
    uint32_t                          last_apply_ms;
    uint32_t                          last_apply_elapsed_ms;
    char                              last_error[SBF_LAST_ERROR_MAX];
} sbf_ctx_t;

static sbf_ctx_t s_ctx;

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static void copy_err(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == (size_t)0 || src == NULL) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - (size_t)1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void emit_event(const char *fmt, ...) {
    if (s_ctx.cfg.event_sink == NULL) return;
    char buf[SBF_EVENT_JSON_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_ctx.cfg.event_sink(buf, s_ctx.cfg.event_sink_ctx);
}

static void set_state(sbf_state_t next) {
    if (s_ctx.state == next) return;
    ESP_LOGI(TAG, "state %d → %d", (int)s_ctx.state, (int)next);
    s_ctx.state = next;
}

static bool stage_in_range(uint8_t stage) {
    return stage >= (uint8_t)SBF_STAGE_MIN && stage <= (uint8_t)SBF_STAGE_MAX;
}
static bool ethanol_in_range(uint8_t pct) {
    return pct <= (uint8_t)SBF_ETHANOL_MAX_PCT;
}

// Latest-wins enqueue: if last entry is REAPPLY, replace it instead
// of growing the queue. UNLOAD bumps to head (drains first).
static esp_err_t enqueue(sbf_req_kind_t kind, uint8_t stage, uint8_t eth) {
    if (kind == SBF_REQ_REAPPLY && s_ctx.queue_len > (size_t)0 &&
        s_ctx.queue[s_ctx.queue_len - (size_t)1].kind == SBF_REQ_REAPPLY) {
        s_ctx.queue[s_ctx.queue_len - (size_t)1].stage       = stage;
        s_ctx.queue[s_ctx.queue_len - (size_t)1].ethanol_pct = eth;
        return ESP_OK;
    }
    if (s_ctx.queue_len >= (size_t)SBF_WORKER_QUEUE_DEPTH) {
        return ESP_ERR_NO_MEM;
    }
    s_ctx.queue[s_ctx.queue_len].kind        = kind;
    s_ctx.queue[s_ctx.queue_len].stage       = stage;
    s_ctx.queue[s_ctx.queue_len].ethanol_pct = eth;
    s_ctx.queue_len++;
    return ESP_OK;
}

static void clear_queue(void) {
    s_ctx.queue_len = (size_t)0;
}

// ------------------------------------------------------------------
// feature_manager descriptor callbacks
// ------------------------------------------------------------------

static esp_err_t fm_start_cb(void) {
    s_ctx.fm_running = true;
    ESP_LOGI(TAG, "feature_manager slot acquired");
    return ESP_OK;
}
static esp_err_t fm_stop_cb(void) {
    s_ctx.fm_running = false;
    ESP_LOGI(TAG, "feature_manager slot released");
    return ESP_OK;
}
bool sbf_orchestrator_is_running(void) {
    return s_ctx.fm_running;
}

static const feature_descriptor_t k_descriptor = {
    .id         = FEATURE_LIVE_TUNE,
    .name       = "live_tune",
    .start      = fm_start_cb,
    .stop       = fm_stop_cb,
    .is_running = sbf_orchestrator_is_running,
};

esp_err_t sbf_orchestrator_register_with_feature_manager(void) {
    esp_err_t rc = feature_manager_register(&k_descriptor);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) return rc;
    return ESP_OK;
}

// ------------------------------------------------------------------
// Init / deinit
// ------------------------------------------------------------------

esp_err_t sbf_orchestrator_init(const sbf_orchestrator_config_t *cfg) {
    if (s_ctx.initialized) return ESP_OK;
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->loader.load == NULL || cfg->loader.free == NULL ||
        cfg->loader.map_count == NULL || cfg->loader.map_info == NULL ||
        cfg->loader.blend_axes == NULL || cfg->loader.map_buffers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->ecu_write == NULL || cfg->clock_now_ms == NULL ||
        cfg->vin_source == NULL || cfg->boxcode_source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg         = *cfg;
    s_ctx.state       = SBF_STATE_IDLE;
    s_ctx.initialized = true;

    esp_err_t rc = sbf_orchestrator_register_with_feature_manager();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "register_with_feature_manager rc=%d", (int)rc);
        return rc;
    }
    ESP_LOGI(TAG, "sbf_orchestrator initialized");
    return ESP_OK;
}

void sbf_orchestrator_deinit(void) {
    if (s_ctx.loaded != NULL && s_ctx.cfg.loader.free != NULL) {
        s_ctx.cfg.loader.free(s_ctx.loaded, s_ctx.cfg.loader.user_ctx);
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
}

// ------------------------------------------------------------------
// Public ops
// ------------------------------------------------------------------

esp_err_t sbf_orchestrator_live_tune_start(uint8_t stage, uint8_t ethanol_pct,
                                           char *err_out, size_t err_cap) {
    if (err_out != NULL && err_cap > (size_t)0) err_out[0] = '\0';
    if (!s_ctx.initialized) {
        copy_err(err_out, err_cap, "orchestrator not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!stage_in_range(stage)) {
        copy_err(err_out, err_cap, "stage out of range (1..3)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!ethanol_in_range(ethanol_pct)) {
        copy_err(err_out, err_cap, "ethanol_pct out of range (0..100)");
        return ESP_ERR_INVALID_ARG;
    }

    // License gate. The vin_source may return NULL if the ECU VIN
    // isn't known yet — license_can_run_feature handles that
    // gracefully (degrades to present+paid+!revoked-only check).
    const char *vin = s_ctx.cfg.vin_source != NULL ? s_ctx.cfg.vin_source() : NULL;
    char gate_reason[128] = {0};
    if (!license_can_run_feature(FEATURE_LIVE_TUNE, vin,
                                 gate_reason, sizeof(gate_reason))) {
        copy_err(err_out, err_cap,
                 gate_reason[0] != '\0' ? gate_reason : "license gate refused");
        return ESP_ERR_INVALID_STATE;
    }

    // Acquire feature_manager slot if not already held.
    if (!s_ctx.fm_running) {
        char fm_err[128] = {0};
        esp_err_t fm_rc = feature_manager_request_start(FEATURE_LIVE_TUNE,
                                                        fm_err, sizeof(fm_err));
        if (fm_rc != ESP_OK) {
            copy_err(err_out, err_cap,
                     fm_err[0] != '\0' ? fm_err : "feature_manager rejected start");
            return fm_rc;
        }
    }

    esp_err_t qrc = enqueue(SBF_REQ_LOAD_AND_APPLY, stage, ethanol_pct);
    if (qrc != ESP_OK) {
        copy_err(err_out, err_cap, "worker queue full");
        return qrc;
    }
    set_state(SBF_STATE_LOADING);
    emit_event("{\"event\":\"apply_started\",\"stage\":%u,\"ethanol_pct\":%u}",
               (unsigned)stage, (unsigned)ethanol_pct);
    return ESP_OK;
}

esp_err_t sbf_orchestrator_live_tune_set(uint8_t stage, uint8_t ethanol_pct,
                                         char *err_out, size_t err_cap) {
    if (err_out != NULL && err_cap > (size_t)0) err_out[0] = '\0';
    if (!s_ctx.initialized) {
        copy_err(err_out, err_cap, "orchestrator not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.state != SBF_STATE_ACTIVE && s_ctx.state != SBF_STATE_APPLYING) {
        copy_err(err_out, err_cap, "live_tune_set only valid while active; call live_tune_start first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!stage_in_range(stage) || !ethanol_in_range(ethanol_pct)) {
        copy_err(err_out, err_cap, "params out of range");
        return ESP_ERR_INVALID_ARG;
    }
    // Per Sean's Q-B directive: stage must match the cached SBF in
    // v1. Multi-stage rotation is a follow-on prompt that requires a
    // cloud-side `?stage=N` query string.
    if (s_ctx.current_stage != (uint8_t)0 && stage != s_ctx.current_stage) {
        copy_err(err_out, err_cap,
                 "stage mismatch: re-fetch via live_tune_start(N) — "
                 "multi-stage rotation requires admin re-assign");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t qrc = enqueue(SBF_REQ_REAPPLY, stage, ethanol_pct);
    if (qrc != ESP_OK) {
        copy_err(err_out, err_cap, "worker queue full");
        return qrc;
    }
    emit_event("{\"event\":\"apply_started\",\"stage\":%u,\"ethanol_pct\":%u}",
               (unsigned)stage, (unsigned)ethanol_pct);
    return ESP_OK;
}

esp_err_t sbf_orchestrator_live_tune_stop(char *err_out, size_t err_cap) {
    if (err_out != NULL && err_cap > (size_t)0) err_out[0] = '\0';
    if (!s_ctx.initialized) {
        copy_err(err_out, err_cap, "orchestrator not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    /* Drain any pending requests, drop the loaded SBF, release slot. */
    clear_queue();
    if (s_ctx.loaded != NULL && s_ctx.cfg.loader.free != NULL) {
        s_ctx.cfg.loader.free(s_ctx.loaded, s_ctx.cfg.loader.user_ctx);
        s_ctx.loaded = NULL;
    }
    s_ctx.current_stage = (uint8_t)0;
    s_ctx.sbf_filename[0] = '\0';
    set_state(SBF_STATE_IDLE);
    emit_event("{\"event\":\"unload\"}");

    if (s_ctx.fm_running) {
        feature_manager_request_stop(FEATURE_LIVE_TUNE);
    }
    return ESP_OK;
}

void sbf_orchestrator_live_tune_status(sbf_status_snapshot_t *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->state                 = s_ctx.state;
    out->current_stage         = s_ctx.current_stage;
    out->current_ethanol_pct   = s_ctx.current_ethanol_pct;
    out->last_apply_ms         = s_ctx.last_apply_ms;
    out->last_apply_elapsed_ms = s_ctx.last_apply_elapsed_ms;
    strncpy(out->sbf_filename, s_ctx.sbf_filename, sizeof(out->sbf_filename) - (size_t)1);
    out->sbf_filename[sizeof(out->sbf_filename) - (size_t)1] = '\0';
    strncpy(out->last_error, s_ctx.last_error, sizeof(out->last_error) - (size_t)1);
    out->last_error[sizeof(out->last_error) - (size_t)1] = '\0';
}

size_t sbf_orchestrator_queue_depth(void) {
    return s_ctx.queue_len;
}

// ------------------------------------------------------------------
// Worker drive
// ------------------------------------------------------------------

static void apply_progress_cb(uint32_t maps_done, uint32_t maps_total, void *ctx) {
    (void)ctx;
    emit_event("{\"event\":\"apply_progress\",\"maps_done\":%u,\"maps_total\":%u}",
               (unsigned)maps_done, (unsigned)maps_total);
}

/* Adapt the orchestrator's no-arg clock fn to the applier's
 * (void *ctx)-clock fn signature. The applier passes user_ctx
 * through; we ignore it and call the bound clock. */
static uint32_t clock_thunk(void *ctx) {
    (void)ctx;
    return s_ctx.cfg.clock_now_ms != NULL ? s_ctx.cfg.clock_now_ms() : (uint32_t)0;
}

// Run a single apply pass. Called from the LOAD_AND_APPLY and
// REAPPLY paths. Updates state on outcome.
static esp_err_t run_apply(uint8_t stage, uint8_t ethanol_pct) {
    if (s_ctx.loaded == NULL) return ESP_ERR_INVALID_STATE;

    sbf_variant_entry_t var;
    const char *boxcode = s_ctx.cfg.boxcode_source != NULL
                              ? s_ctx.cfg.boxcode_source() : NULL;
    if (boxcode == NULL || boxcode[0] == '\0' ||
        !sbf_variants_lookup(boxcode, &var)) {
        snprintf(s_ctx.last_error, sizeof(s_ctx.last_error),
                 "unknown variant; boxcode='%s' not in sbf_variants table",
                 boxcode != NULL ? boxcode : "(null)");
        emit_event("{\"event\":\"apply_failed\",\"reason\":\"unknown variant\"}");
        return ESP_ERR_NOT_FOUND;
    }

    sbf_applier_deps_t deps = {
        .write        = s_ctx.cfg.ecu_write,
        .clock_now_ms = clock_thunk,
        .progress     = apply_progress_cb,
        .user_ctx     = s_ctx.cfg.ecu_write_ctx,
    };

    set_state(SBF_STATE_APPLYING);
    sbf_apply_result_t res = {0};
    esp_err_t rc = sbf_applier_apply(s_ctx.loaded, &s_ctx.cfg.loader,
                                     ethanol_pct, var.mid_byte,
                                     var.address_offset, &deps, &res);
    s_ctx.last_apply_ms         = s_ctx.cfg.clock_now_ms();
    s_ctx.last_apply_elapsed_ms = res.elapsed_ms;

    if (rc == ESP_OK && res.elapsed_ms > (uint32_t)SBF_APPLY_HARD_CAP_MS) {
        snprintf(s_ctx.last_error, sizeof(s_ctx.last_error),
                 "apply exceeded hard cap %u ms (took %u ms)",
                 (unsigned)SBF_APPLY_HARD_CAP_MS, (unsigned)res.elapsed_ms);
        rc = ESP_ERR_TIMEOUT;
    }

    if (rc != ESP_OK) {
        if (s_ctx.last_error[0] == '\0') {
            strncpy(s_ctx.last_error, res.failure_reason,
                    sizeof(s_ctx.last_error) - (size_t)1);
            s_ctx.last_error[sizeof(s_ctx.last_error) - (size_t)1] = '\0';
        }
        emit_event("{\"event\":\"apply_failed\",\"elapsed_ms\":%u,\"maps_applied\":%u}",
                   (unsigned)res.elapsed_ms, (unsigned)res.maps_applied);
        set_state(SBF_STATE_ERROR);
        return rc;
    }

    s_ctx.last_error[0] = '\0';
    s_ctx.current_stage       = stage;
    s_ctx.current_ethanol_pct = ethanol_pct;
    emit_event("{\"event\":\"apply_completed\",\"elapsed_ms\":%u,\"maps_applied\":%u,\"bytes\":%u}",
               (unsigned)res.elapsed_ms, (unsigned)res.maps_applied,
               (unsigned)res.total_bytes_written);
    set_state(SBF_STATE_ACTIVE);
    return ESP_OK;
}

// LOAD_AND_APPLY path: open the cached SBF (or treat as missing on
// failure) then run apply.
static esp_err_t run_load_and_apply(uint8_t stage, uint8_t ethanol_pct) {
    char path[SBF_PATH_MAX];
    sbf_downloader_cache_path(stage, path, sizeof(path));

    /* Drop any prior SBF. */
    if (s_ctx.loaded != NULL) {
        s_ctx.cfg.loader.free(s_ctx.loaded, s_ctx.cfg.loader.user_ctx);
        s_ctx.loaded = NULL;
    }

    set_state(SBF_STATE_LOADING);
    sbf_loaded_t *l = NULL;
    esp_err_t rc = s_ctx.cfg.loader.load(path, &l, s_ctx.cfg.loader.user_ctx);
    if (rc != ESP_OK || l == NULL) {
        /* %.64s bounds the embedded path so snprintf can't be flagged
         * by -Werror=format-truncation= when path == sizeof(buf). */
        snprintf(s_ctx.last_error, sizeof(s_ctx.last_error),
                 "loader.load rc=%d path=%.64s", (int)rc, path);
        emit_event("{\"event\":\"apply_failed\",\"reason\":\"load failed\"}");
        set_state(SBF_STATE_ERROR);
        return rc != ESP_OK ? rc : ESP_FAIL;
    }
    s_ctx.loaded = l;
    strncpy(s_ctx.sbf_filename, path, sizeof(s_ctx.sbf_filename) - (size_t)1);
    s_ctx.sbf_filename[sizeof(s_ctx.sbf_filename) - (size_t)1] = '\0';

    return run_apply(stage, ethanol_pct);
}

// ------------------------------------------------------------------
// On-target integration (compiled out under SBF_HOST_BUILD)
// ------------------------------------------------------------------

#ifndef SBF_HOST_BUILD

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "wifi/wifi_ap.h"
#include "state_machine/connection_manager.h"
#include "ecu_write/ecu_write.h"
#include "nvs/nvs_manager.h"
#include "websocket/ws_server.h"
#include "license_config.h"

/* On-target ecu_write_iface adapter: the frozen ecu_write_data is
 * async with a callback. We bridge to the synchronous applier write
 * fn by giving a binary semaphore in the callback. */

static SemaphoreHandle_t s_target_write_sem = NULL;
static volatile bool     s_target_write_ok  = false;

static void target_write_cb(bool success, void *user_data) {
    (void)user_data;
    s_target_write_ok = success;
    if (s_target_write_sem != NULL) {
        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(s_target_write_sem, &hpw);
    }
}

static esp_err_t target_ecu_write(uint32_t ecu_address,
                                  const uint8_t *bytes, size_t byte_count,
                                  uint8_t mid_byte, uint32_t address_offset,
                                  uint32_t per_write_timeout_ms,
                                  void *user_ctx) {
    (void)user_ctx;
    if (s_target_write_sem == NULL) {
        s_target_write_sem = xSemaphoreCreateBinary();
        if (s_target_write_sem == NULL) return ESP_ERR_NO_MEM;
    }
    s_target_write_ok = false;
    esp_err_t rc = ecu_write_data(ecu_address, bytes, byte_count,
                                  mid_byte, address_offset,
                                  target_write_cb, NULL);
    if (rc != ESP_OK) return rc;
    if (xSemaphoreTake(s_target_write_sem, pdMS_TO_TICKS(per_write_timeout_ms)) != pdTRUE) {
        ecu_write_cancel();
        return ESP_ERR_TIMEOUT;
    }
    return s_target_write_ok ? ESP_OK : ESP_FAIL;
}

static const char *target_vin_source(void)     { return connection_manager_get_vin(); }
static const char *target_boxcode_source(void) { return connection_manager_get_boxcode(); }

static uint32_t target_clock_now_ms(void) {
    return (uint32_t)((uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS);
}

static void target_event_sink(const char *json, void *ctx) {
    (void)ctx;
    if (json != NULL) ws_server_broadcast_text(json);
}

/* Worker task wrapper around drain_one. Uses a queue semaphore so
 * enqueuing wakes the task immediately. */
static SemaphoreHandle_t s_target_wake = NULL;

static void target_worker_task(void *arg) {
    (void)arg;
    while (true) {
        if (s_target_wake != NULL) {
            xSemaphoreTake(s_target_wake, pdMS_TO_TICKS((uint32_t)100));
        } else {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)100));
        }
        sbf_orchestrator_drain_one();
    }
}

/* HTTP/FS adapters for sbf_downloader. Same shape as license/vin_pairing. */
static int target_http_get(const char *url, const char *bearer,
                           uint8_t *body_out, size_t body_cap, size_t *body_len_out,
                           uint32_t timeout_ms, void *ctx) {
    (void)ctx;
    if (body_len_out != NULL) *body_len_out = (size_t)0;
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET,
        .timeout_ms = (int)timeout_ms,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) return (int)ESP_FAIL;
    if (bearer != NULL && bearer[0] != '\0') {
        char auth[LICENSE_AUTH_HEADER_MAX];
        snprintf(auth, sizeof(auth), "%s%s", LICENSE_BEARER_PREFIX, bearer);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    esp_err_t open_rc = esp_http_client_open(cli, (int)0);
    if (open_rc != ESP_OK) {
        esp_http_client_cleanup(cli);
        return (int)open_rc;
    }
    (void)esp_http_client_fetch_headers(cli);
    int status = (int)esp_http_client_get_status_code(cli);
    if (body_out != NULL && body_cap > (size_t)0) {
        int read_n = esp_http_client_read_response(cli, (char *)body_out, (int)body_cap);
        if (read_n > (int)0 && body_len_out != NULL) {
            *body_len_out = (size_t)read_n;
        }
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return status;
}

#include <sys/stat.h>

static esp_err_t target_fs_write(const char *path, const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    mkdir(SBF_CACHE_DIR_PATH, (mode_t)SBF_CACHE_DIR_MODE);
    FILE *f = fopen(path, "wb");
    if (f == NULL) return ESP_FAIL;
    size_t w = fwrite(data, (size_t)1, len, f);
    fclose(f);
    return w == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t target_auth_token(char *out, size_t cap, void *ctx) {
    (void)ctx;
    if (out == NULL || cap == (size_t)0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    if (nvs_manager_load_string(LICENSE_NVS_AUTH_TOKEN_KEY, out, cap) != ESP_OK ||
        out[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t main_init_sbf_orchestrator(void) {
    sbf_downloader_config_t dl_cfg = {
        .http        = { .get = target_http_get, .user_ctx = NULL },
        .fs          = { .write_file = target_fs_write, .user_ctx = NULL },
        .auth_token  = target_auth_token,
        .auth_user_ctx = NULL,
    };
    esp_err_t rc = sbf_downloader_init(&dl_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "sbf_downloader_init rc=%d", (int)rc);
        return rc;
    }

    sbf_orchestrator_config_t cfg = {
        .loader         = sbf_loader_target_iface(),
        .ecu_write      = target_ecu_write,
        .ecu_write_ctx  = NULL,
        .event_sink     = target_event_sink,
        .event_sink_ctx = NULL,
        .vin_source     = target_vin_source,
        .boxcode_source = target_boxcode_source,
        .clock_now_ms   = target_clock_now_ms,
    };
    rc = sbf_orchestrator_init(&cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "sbf_orchestrator_init rc=%d", (int)rc);
        return rc;
    }

    /* Spawn the worker task. */
    s_target_wake = xSemaphoreCreateBinary();
    BaseType_t tret = xTaskCreatePinnedToCore(
        target_worker_task, "sbf_worker",
        (uint32_t)SBF_WORKER_TASK_STACK_BYTES,
        NULL, (UBaseType_t)SBF_WORKER_TASK_PRIORITY, NULL,
        (BaseType_t)SBF_WORKER_TASK_CORE);
    if (tret != pdPASS) {
        ESP_LOGE(TAG, "sbf_worker xTaskCreate failed");
        return ESP_FAIL;
    }

    /* Prompt 4 follow-up: install VIN source on wot_uploader so its
     * license gate uses the live ECU VIN. */
    extern void wot_uploader_set_vin_source(const char *(*fn)(void));
    wot_uploader_set_vin_source(connection_manager_get_vin);

    ESP_LOGI(TAG, "sbf_orchestrator wired (worker task spawned)");
    return ESP_OK;
}

#endif /* !SBF_HOST_BUILD */

esp_err_t sbf_orchestrator_drain_one(void) {
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (s_ctx.queue_len == (size_t)0) return ESP_OK;

    sbf_req_t req = s_ctx.queue[0];
    /* Shift queue. */
    for (size_t i = (size_t)1; i < s_ctx.queue_len; i++) {
        s_ctx.queue[i - (size_t)1] = s_ctx.queue[i];
    }
    s_ctx.queue_len--;

    switch (req.kind) {
        case SBF_REQ_LOAD_AND_APPLY:
            return run_load_and_apply(req.stage, req.ethanol_pct);
        case SBF_REQ_REAPPLY:
            return run_apply(req.stage, req.ethanol_pct);
        case SBF_REQ_UNLOAD:
            sbf_orchestrator_live_tune_stop(NULL, (size_t)0);
            return ESP_OK;
        default:
            return ESP_ERR_INVALID_STATE;
    }
}
