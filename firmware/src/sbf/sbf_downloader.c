#include "sbf_downloader.h"
#include "sbf_config.h"
#include "license_config.h"

#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// sbf_downloader — see sbf_downloader.h. Synchronous fetch + write;
// the orchestrator calls it from the worker task.

static const char *TAG = "SBF_DL";

typedef struct {
    bool                    initialized;
    sbf_http_iface_t        http;
    sbf_fs_iface_t          fs;
    sbf_auth_token_fn_t     auth_token;
    void                   *auth_user_ctx;
} sbf_downloader_ctx_t;

static sbf_downloader_ctx_t s_ctx;

static void copy_err(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == (size_t)0 || src == NULL) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - (size_t)1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void format_err(char *dst, size_t cap, const char *fmt, int v) {
    if (dst == NULL || cap == (size_t)0) return;
    snprintf(dst, cap, fmt, v);
}

void sbf_downloader_cache_path(uint8_t stage, char *out, size_t cap) {
    if (out == NULL || cap == (size_t)0) return;
    snprintf(out, cap, SBF_CACHE_FILENAME_TEMPLATE, SBF_CACHE_DIR_PATH,
             (unsigned)stage);
}

esp_err_t sbf_downloader_init(const sbf_downloader_config_t *cfg) {
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->http.get == NULL || cfg->fs.write_file == NULL || cfg->auth_token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.http          = cfg->http;
    s_ctx.fs            = cfg->fs;
    s_ctx.auth_token    = cfg->auth_token;
    s_ctx.auth_user_ctx = cfg->auth_user_ctx;
    s_ctx.initialized   = true;
    ESP_LOGI(TAG, "downloader initialized");
    return ESP_OK;
}

void sbf_downloader_deinit(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
}

esp_err_t sbf_downloader_fetch_to_cache(uint8_t stage, char *err_out, size_t err_cap) {
    if (err_out != NULL && err_cap > (size_t)0) err_out[0] = '\0';
    if (!s_ctx.initialized) {
        copy_err(err_out, err_cap, "downloader not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (stage < (uint8_t)SBF_STAGE_MIN || stage > (uint8_t)SBF_STAGE_MAX) {
        copy_err(err_out, err_cap, "stage out of range");
        return ESP_ERR_INVALID_ARG;
    }

    char token[LICENSE_AUTH_TOKEN_MAX] = {0};
    esp_err_t trc = s_ctx.auth_token(token, sizeof(token), s_ctx.auth_user_ctx);
    if (trc != ESP_OK || token[0] == '\0') {
        copy_err(err_out, err_cap, "device not enrolled (auth_token missing)");
        return ESP_ERR_NOT_FOUND;
    }

    char url[SBF_URL_MAX];
    snprintf(url, sizeof(url), "%s%s", SBF_DEFAULT_HOST, SBF_DOWNLOAD_PATH);

    uint8_t *buf = (uint8_t *)malloc((size_t)SBF_DOWNLOAD_BUF_MAX);
    if (buf == NULL) {
        copy_err(err_out, err_cap, "OOM allocating download buffer");
        return ESP_ERR_NO_MEM;
    }
    size_t body_len = (size_t)0;
    int status = s_ctx.http.get(url, token, buf, (size_t)SBF_DOWNLOAD_BUF_MAX,
                                &body_len, (uint32_t)SBF_DOWNLOAD_HTTP_TIMEOUT_MS,
                                s_ctx.http.user_ctx);
    if (status < 0) {
        format_err(err_out, err_cap, "transport error rc=%d", status);
        free(buf);
        return ESP_FAIL;
    }
    if (status < (int)SBF_HTTP_OK_MIN || status > (int)SBF_HTTP_OK_MAX) {
        format_err(err_out, err_cap, "HTTP %d", status);
        free(buf);
        return ESP_FAIL;
    }
    if (body_len == (size_t)0) {
        copy_err(err_out, err_cap, "empty body");
        free(buf);
        return ESP_FAIL;
    }

    char path[SBF_PATH_MAX];
    sbf_downloader_cache_path(stage, path, sizeof(path));
    esp_err_t wrc = s_ctx.fs.write_file(path, buf, body_len, s_ctx.fs.user_ctx);
    free(buf);
    if (wrc != ESP_OK) {
        format_err(err_out, err_cap, "fs.write_file rc=%d", (int)wrc);
        return wrc;
    }
    ESP_LOGI(TAG, "fetched %u bytes to %s (HTTP %d)",
             (unsigned)body_len, path, status);
    return ESP_OK;
}
