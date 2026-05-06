#include "wot_uploader.h"
#include "wot_logger_config.h"

#include "esp_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// wot_uploader — see wot_uploader.h for the contract.
//
// Tick-driven retry loop (no internal task). On each tick the
// uploader checks: armed? Wi-Fi STA up? retry interval elapsed?
// If all yes, it iterates one queued file, POSTs it, and applies
// delete-on-2xx / retain-on-non-2xx.

static const char *TAG = "WOT_UP";

typedef struct {
    bool                          initialized;
    bool                          running;
    wot_uploader_http_iface_t     http;
    wot_uploader_fs_iface_t       fs;
    wot_uploader_wifi_ready_fn_t  wifi_ready;
    char                          upload_url[WOT_UPLOAD_URL_MAX];
    uint32_t                      last_attempt_ms;
    bool                          first_tick;
    uint32_t                      queue_count;
    uint32_t                      queue_bytes;
} up_ctx_t;

static up_ctx_t s_ctx;

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static bool http_status_is_ok(int status) {
    return status >= (int)WOT_UPLOAD_HTTP_OK_MIN
        && status <= (int)WOT_UPLOAD_HTTP_OK_MAX;
}

// Synthesize a queue filename from a monotonic timestamp. Format:
// "<queue_dir>/wot_<ts>.csv.gz". Buffer must be at least
// WOT_QUEUE_FILENAME_MAX bytes.
static void make_queue_filename(uint32_t now_ms, char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s/wot_%u.csv.gz",
             WOT_QUEUE_DIR_PATH, (unsigned)now_ms);
}

// Recompute queue stats from the iter_next interface. Used after
// enqueue/delete so wot_uploader_queue_count() returns truth without
// the caller maintaining its own counter.
static void refresh_queue_stats(void) {
    if (s_ctx.fs.iter_next == NULL) {
        s_ctx.queue_count = 0;
        s_ctx.queue_bytes = 0;
        return;
    }
    char name[WOT_QUEUE_FILENAME_MAX];
    size_t size = 0;
    uint32_t count = 0;
    uint32_t bytes = 0;
    bool first = true;
    while (s_ctx.fs.iter_next(first, name, sizeof(name), &size, s_ctx.fs.user_ctx) == ESP_OK) {
        first = false;
        count++;
        bytes += (uint32_t)size;
    }
    s_ctx.queue_count = count;
    s_ctx.queue_bytes = bytes;
}

// FIFO drop oldest file(s) until `incoming_bytes` would fit beneath
// the WOT_UPLOAD_MAX_QUEUE_BYTES ceiling. Returns ESP_OK if room was
// made (or the file fits already), ESP_ERR_NO_MEM if not even
// dropping every queued file would make room (i.e. incoming_bytes
// alone exceeds the ceiling).
static esp_err_t make_room_for(size_t incoming_bytes) {
    if (incoming_bytes > (size_t)WOT_UPLOAD_MAX_QUEUE_BYTES) {
        return ESP_ERR_NO_MEM;
    }
    while ((uint32_t)(s_ctx.queue_bytes + incoming_bytes) >
           (uint32_t)WOT_UPLOAD_MAX_QUEUE_BYTES) {
        char victim[WOT_QUEUE_FILENAME_MAX];
        size_t victim_size = 0;
        if (s_ctx.fs.iter_next(true, victim, sizeof(victim),
                               &victim_size, s_ctx.fs.user_ctx) != ESP_OK) {
            // Queue empty but room still insufficient — incoming
            // somehow grew between checks. Treat as failure.
            return ESP_ERR_NO_MEM;
        }
        char victim_path[WOT_QUEUE_FULL_PATH_MAX];
        snprintf(victim_path, sizeof(victim_path), "%s/%s",
                 WOT_QUEUE_DIR_PATH, victim);
        ESP_LOGW(TAG, "queue full; dropping oldest %s (%u bytes)",
                 victim, (unsigned)victim_size);
        s_ctx.fs.delete_file(victim_path, s_ctx.fs.user_ctx);
        if (s_ctx.queue_bytes >= (uint32_t)victim_size) {
            s_ctx.queue_bytes -= (uint32_t)victim_size;
        } else {
            s_ctx.queue_bytes = 0;
        }
        if (s_ctx.queue_count > 0) s_ctx.queue_count--;
    }
    return ESP_OK;
}

// Attempt to upload one queued file. Picks the iterator's first
// entry. Deletes on success, retains on failure. Returns ESP_OK if
// the queue had at least one file (regardless of upload outcome),
// ESP_ERR_NOT_FOUND if the queue is empty.
static esp_err_t try_one_upload(void) {
    if (s_ctx.fs.iter_next == NULL || s_ctx.fs.read_file == NULL ||
        s_ctx.fs.delete_file == NULL || s_ctx.http.post == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char name[WOT_QUEUE_FILENAME_MAX];
    size_t size = 0;
    if (s_ctx.fs.iter_next(true, name, sizeof(name), &size, s_ctx.fs.user_ctx) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[WOT_QUEUE_FULL_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", WOT_QUEUE_DIR_PATH, name);

    if (size == 0) {
        // Empty file — drop it.
        ESP_LOGW(TAG, "queue entry %s is empty; dropping", name);
        s_ctx.fs.delete_file(path, s_ctx.fs.user_ctx);
        refresh_queue_stats();
        return ESP_OK;
    }

    uint8_t *body = (uint8_t *)malloc(size);
    if (body == NULL) {
        ESP_LOGE(TAG, "alloc %u bytes for upload failed", (unsigned)size);
        return ESP_ERR_NO_MEM;
    }
    size_t in_out_len = size;
    esp_err_t rc = s_ctx.fs.read_file(path, body, &in_out_len, s_ctx.fs.user_ctx);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "read %s rc=%d; will retry next interval", path, (int)rc);
        free(body);
        return ESP_OK;
    }

    int status = s_ctx.http.post(s_ctx.upload_url, body, in_out_len,
                                 (uint32_t)WOT_UPLOAD_HTTP_TIMEOUT_MS,
                                 s_ctx.http.user_ctx);
    free(body);

    if (http_status_is_ok(status)) {
        ESP_LOGI(TAG, "upload OK (status=%d) — deleting %s", status, name);
        s_ctx.fs.delete_file(path, s_ctx.fs.user_ctx);
        refresh_queue_stats();
    } else {
        ESP_LOGW(TAG, "upload failed (status=%d) — retaining %s for retry",
                 status, name);
    }
    return ESP_OK;
}

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

esp_err_t wot_uploader_init(const wot_uploader_config_t *cfg) {
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg == NULL || cfg->upload_url == NULL ||
        cfg->wifi_ready == NULL || cfg->http.post == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->fs.write_file == NULL || cfg->fs.read_file == NULL ||
        cfg->fs.delete_file == NULL || cfg->fs.iter_next == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.http       = cfg->http;
    s_ctx.fs         = cfg->fs;
    s_ctx.wifi_ready = cfg->wifi_ready;
    strncpy(s_ctx.upload_url, cfg->upload_url, sizeof(s_ctx.upload_url) - (size_t)1);
    s_ctx.upload_url[sizeof(s_ctx.upload_url) - (size_t)1] = '\0';
    s_ctx.first_tick = true;
    s_ctx.initialized = true;

    refresh_queue_stats();
    ESP_LOGI(TAG, "uploader initialized (url=%s, queued=%u files / %u bytes)",
             s_ctx.upload_url, (unsigned)s_ctx.queue_count, (unsigned)s_ctx.queue_bytes);
    return ESP_OK;
}

void wot_uploader_deinit(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
}

esp_err_t wot_uploader_start(void) {
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        return ESP_OK;
    }
    s_ctx.running = true;
    s_ctx.first_tick = true;
    ESP_LOGI(TAG, "uploader started");
    return ESP_OK;
}

esp_err_t wot_uploader_stop(void) {
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_ctx.running) {
        return ESP_OK;
    }
    s_ctx.running = false;
    ESP_LOGI(TAG, "uploader stopped");
    return ESP_OK;
}

bool wot_uploader_is_running(void) {
    return s_ctx.initialized && s_ctx.running;
}

esp_err_t wot_uploader_enqueue(const uint8_t *gzip_buf, size_t gzip_len, uint32_t now_ms) {
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gzip_buf == NULL || gzip_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (make_room_for(gzip_len) != ESP_OK) {
        ESP_LOGE(TAG, "incoming %u bytes exceeds queue ceiling %u",
                 (unsigned)gzip_len, (unsigned)WOT_UPLOAD_MAX_QUEUE_BYTES);
        return ESP_ERR_NO_MEM;
    }
    char path[WOT_QUEUE_FULL_PATH_MAX];
    make_queue_filename(now_ms, path, sizeof(path));
    esp_err_t rc = s_ctx.fs.write_file(path, gzip_buf, gzip_len, s_ctx.fs.user_ctx);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "queue write %s rc=%d", path, (int)rc);
        return rc;
    }
    s_ctx.queue_count++;
    s_ctx.queue_bytes += (uint32_t)gzip_len;
    ESP_LOGI(TAG, "enqueued %s (%u bytes; queue %u files / %u bytes)",
             path, (unsigned)gzip_len,
             (unsigned)s_ctx.queue_count, (unsigned)s_ctx.queue_bytes);
    return ESP_OK;
}

void wot_uploader_tick(uint32_t now_ms) {
    if (!s_ctx.initialized || !s_ctx.running) {
        return;
    }
    if (s_ctx.first_tick) {
        s_ctx.last_attempt_ms = now_ms;
        s_ctx.first_tick = false;
        return;
    }
    if ((now_ms - s_ctx.last_attempt_ms) < (uint32_t)WOT_UPLOAD_RETRY_INTERVAL_MS) {
        return;
    }
    s_ctx.last_attempt_ms = now_ms;
    if (s_ctx.queue_count == 0) {
        return;
    }
    if (s_ctx.wifi_ready != NULL && !s_ctx.wifi_ready()) {
        ESP_LOGD(TAG, "wifi not ready; deferring upload attempt");
        return;
    }
    try_one_upload();
}

uint32_t wot_uploader_queue_count(void) {
    return s_ctx.initialized ? s_ctx.queue_count : (uint32_t)0;
}
