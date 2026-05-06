#ifndef SBF_DOWNLOADER_H
#define SBF_DOWNLOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sbf_downloader — pulls the device's currently-assigned SBF from
 * the cloud's existing /api/v1/device/calibration endpoint and
 * writes it to /storage/sbf/stage<N>.sbf for the loader to consume.
 *
 * Per Sean's Q-F directive, the downloader carries its own
 * sbf_http_iface_t — separate from license_http_iface_t — so host
 * tests can mock it cleanly. Auth token is read from NVS at the
 * call site (using LICENSE_NVS_AUTH_TOKEN_KEY which the license
 * module persists).
 *
 * The cloud endpoint takes no `?stage=` parameter today; v1
 * downloads the single assigned SBF and stores it under the
 * caller's chosen stage filename. Multi-stage rotation requires a
 * cloud-side change deferred to a follow-on prompt.
 */

/* Synchronous HTTP GET with optional Bearer auth. Same signature
 * shape as license_http_get_fn_t for ergonomic parity, but the
 * sbf_downloader does NOT reuse the license module's iface — every
 * feature with HTTP needs gets its own injected iface so each test
 * mocks per-feature. */
typedef int (*sbf_http_get_fn_t)(const char *url,
                                 const char *bearer,
                                 uint8_t    *body_out,
                                 size_t      body_cap,
                                 size_t     *body_len_out,
                                 uint32_t    timeout_ms,
                                 void       *user_ctx);

typedef struct {
    sbf_http_get_fn_t get;
    void             *user_ctx;
} sbf_http_iface_t;

/* Filesystem write — same shape as wot_uploader_fs_iface_t.write_file
 * but stand-alone so the downloader doesn't depend on wot_uploader's
 * header. */
typedef esp_err_t (*sbf_fs_write_fn_t)(const char    *path,
                                       const uint8_t *data,
                                       size_t         len,
                                       void          *user_ctx);

typedef struct {
    sbf_fs_write_fn_t write_file;
    void             *user_ctx;
} sbf_fs_iface_t;

/* Auth-token source — typically loads LICENSE_NVS_AUTH_TOKEN_KEY
 * from NVS. Returns ESP_OK + populated buffer when present;
 * ESP_ERR_NOT_FOUND or empty string when not enrolled. */
typedef esp_err_t (*sbf_auth_token_fn_t)(char *out, size_t cap, void *user_ctx);

typedef struct {
    sbf_http_iface_t       http;
    sbf_fs_iface_t         fs;
    sbf_auth_token_fn_t    auth_token;
    void                  *auth_user_ctx;
} sbf_downloader_config_t;

esp_err_t sbf_downloader_init(const sbf_downloader_config_t *cfg);
void      sbf_downloader_deinit(void);

/* Fetch the assigned SBF from the cloud and write it to
 * /storage/sbf/stage<stage>.sbf. Returns ESP_OK on a 2xx that
 * yielded a non-empty body and a successful write. err_out (if
 * non-NULL) carries a human-readable failure message. */
esp_err_t sbf_downloader_fetch_to_cache(uint8_t stage,
                                        char   *err_out,
                                        size_t  err_cap);

/* Compose the cache path for `stage` into out. Exposed so the
 * orchestrator can pass the same path to the loader. Pure function;
 * no I/O. */
void sbf_downloader_cache_path(uint8_t stage, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SBF_DOWNLOADER_H */
