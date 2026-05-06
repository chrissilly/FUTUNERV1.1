#ifndef WOT_UPLOADER_H
#define WOT_UPLOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * wot_uploader — flash queue + HTTP POST + retry loop.
 *
 * Hands gzipped log bytes from wot_recorder onto a per-device
 * flash queue, then asynchronously POSTs them to the configured
 * cloud endpoint. On a 2xx response the local file is deleted.
 * On any non-2xx (or transport error) the file is retained for the
 * next retry cycle.
 *
 * The HTTP transport and the filesystem are both injected as
 * function-pointer interfaces so the unit test can drive 200 vs.
 * 5xx vs. transport-error behavior deterministically and verify
 * delete-on-200 / retain-on-5xx without spinning up real HTTP or
 * mounting a real partition.
 *
 * Tick model: the uploader does not own a task. The host calls
 * wot_uploader_tick(now_ms) periodically (1 Hz is plenty). On each
 * tick the uploader checks whether the retry interval has elapsed
 * and Wi-Fi STA is connected; if so, it processes one queued log.
 */

/* HTTP transport interface.
 *
 * post(): synchronous POST. Returns the HTTP status code on success
 * (e.g. 200, 503). Returns a negative value on transport error
 * (DNS, connect, TLS, timeout). Caller frees nothing — the body
 * pointer is borrowed for the duration of the call. */
typedef int (*wot_http_post_fn_t)(const char    *url,
                                  const uint8_t *body,
                                  size_t         body_len,
                                  uint32_t       timeout_ms,
                                  void          *user_ctx);

typedef struct {
    wot_http_post_fn_t post;
    void              *user_ctx;
} wot_uploader_http_iface_t;

/* Wi-Fi liveness probe. Returns true if STA is connected and the
 * uploader can attempt a POST. On target this wraps
 * wifi_client_is_connected(); in tests it's a fixture flag. */
typedef bool (*wot_uploader_wifi_ready_fn_t)(void);

/* Filesystem interface. Lets the test back the queue with an
 * in-memory implementation. On target the impl wraps fopen/fwrite/
 * remove/opendir. */
typedef struct {
    /* Append-and-create. Returns ESP_OK on success. */
    esp_err_t (*write_file)(const char    *path,
                            const uint8_t *data,
                            size_t         len,
                            void          *user_ctx);

    /* Read entire file into a caller-provided buffer. *out_len is
     * IN: buffer capacity, OUT: bytes read. Returns ESP_OK on full
     * read, ESP_ERR_NO_MEM if file is larger than capacity. */
    esp_err_t (*read_file)(const char *path,
                           uint8_t    *out,
                           size_t     *in_out_len,
                           void       *user_ctx);

    /* Delete a file. Returns ESP_OK if file existed and was
     * removed (or if it did not exist — idempotent). */
    esp_err_t (*delete_file)(const char *path, void *user_ctx);

    /* Iterate queued files in the WOT queue dir. Each call returns
     * the next filename (relative to the queue dir) into `out` of
     * capacity `out_cap`, plus its size in `*size`. Returns ESP_OK
     * on iteration produced an entry, ESP_ERR_NOT_FOUND on end of
     * iteration. The iterator state is opaque to the caller — the
     * impl manages its own cursor; passing reset=true rewinds it. */
    esp_err_t (*iter_next)(bool      reset,
                           char     *out,
                           size_t    out_cap,
                           size_t   *size,
                           void     *user_ctx);

    void *user_ctx;
} wot_uploader_fs_iface_t;

typedef struct {
    wot_uploader_http_iface_t   http;
    wot_uploader_fs_iface_t     fs;
    wot_uploader_wifi_ready_fn_t wifi_ready;
    /* Full URL; uploader does not concatenate. Caller built it by
     * resolving WOT_UPLOAD_DEFAULT_HOST or NVS override + the
     * WOT_UPLOAD_ENDPOINT_PATH. */
    const char                 *upload_url;
} wot_uploader_config_t;

esp_err_t wot_uploader_init(const wot_uploader_config_t *cfg);
void      wot_uploader_deinit(void);

/* Lifecycle for the feature_manager start/stop hooks. */
esp_err_t wot_uploader_start(void);
esp_err_t wot_uploader_stop(void);
bool      wot_uploader_is_running(void);

/* Enqueue a freshly-finished gzipped recording. Writes the bytes
 * to a new file under WOT_QUEUE_DIR_PATH using a synthesized
 * filename. Returns ESP_OK on enqueue, ESP_ERR_NO_MEM if the
 * queue would exceed WOT_UPLOAD_MAX_QUEUE_BYTES even after
 * dropping the oldest log (FIFO). */
esp_err_t wot_uploader_enqueue(const uint8_t *gzip_buf,
                               size_t         gzip_len,
                               uint32_t       now_ms);

/* Periodic tick. Drives the retry loop. now_ms is the current
 * monotonic millisecond counter; the uploader compares against
 * the previous attempt timestamp and the retry interval. */
void      wot_uploader_tick(uint32_t now_ms);

/* Diagnostics — number of files currently queued on disk. */
uint32_t  wot_uploader_queue_count(void);

#ifdef __cplusplus
}
#endif

#endif /* WOT_UPLOADER_H */
