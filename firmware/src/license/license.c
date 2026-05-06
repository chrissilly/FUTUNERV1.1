#include "license.h"
#include "license_config.h"

#include "esp_log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// license — see license.h for the contract.
//
// The module owns the in-memory license_state_t plus the function-
// pointer transports for HTTP and NVS. Init copies the iface structs
// into module state so the caller's stack frame can go away.
//
// Diagnostic-feature gating: FEATURE_DTC and FEATURE_VIN_PAIRING are
// always allowed regardless of license state, per
// docs/SCALE_ARCHITECTURE_PROPOSAL.md §6.4 ("The dongle simply
// refuses any further Phase 1 changes" — diagnostic reads are not
// "Phase 1 changes"). Future read-only features (live gauge stream
// when promoted to a feature_manager slot) would extend this list.

static const char *TAG = "LICENSE";

typedef struct {
    bool                     initialized;
    license_state_t          state;
    license_http_iface_t     http;
    license_nvs_iface_t      nvs;
} license_ctx_t;

static license_ctx_t s_ctx;

// ------------------------------------------------------------------
// VIN normalization (ISO 3779: uppercase, trimmed)
// ------------------------------------------------------------------

void license_normalize_vin(const char *in, char *out, size_t out_cap) {
    if (out == NULL || out_cap == (size_t)0) return;
    out[0] = '\0';
    if (in == NULL) return;

    // Skip leading whitespace.
    while (*in != '\0' && isspace((unsigned char)*in)) in++;
    if (*in == '\0') return;

    // Find end (last non-whitespace).
    const char *end = in + strlen(in);
    while (end > in && isspace((unsigned char)end[-1])) end--;

    size_t copy = (size_t)(end - in);
    if (copy >= out_cap) copy = out_cap - (size_t)1;
    for (size_t i = (size_t)0; i < copy; i++) {
        out[i] = (char)toupper((unsigned char)in[i]);
    }
    out[copy] = '\0';
}

// True if the two VINs compare equal under normalization. Treats
// either side empty/NULL as "no VIN" — comparison-equal only if BOTH
// are empty.
static bool vin_match(const char *a, const char *b) {
    char na[LICENSE_VIN_BUF_LEN];
    char nb[LICENSE_VIN_BUF_LEN];
    license_normalize_vin(a, na, sizeof(na));
    license_normalize_vin(b, nb, sizeof(nb));
    return strcmp(na, nb) == (int)0;
}

static void set_reason(char *out, size_t cap, const char *fmt, ...) {
    if (out == NULL || cap == (size_t)0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

// ------------------------------------------------------------------
// Diagnostic feature membership
// ------------------------------------------------------------------

static bool feature_is_diagnostic(feature_id_t id) {
    switch (id) {
        case FEATURE_DTC:
        case FEATURE_VIN_PAIRING:
            return true;
        default:
            return false;
    }
}

// ------------------------------------------------------------------
// NVS helpers (delegate to the injected iface)
// ------------------------------------------------------------------

static esp_err_t nvs_save_str(const char *key, const char *value) {
    if (s_ctx.nvs.save_string == NULL) return ESP_ERR_INVALID_STATE;
    return s_ctx.nvs.save_string(key, value, s_ctx.nvs.user_ctx);
}

static esp_err_t nvs_load_str(const char *key, char *value, size_t max_len) {
    if (s_ctx.nvs.load_string == NULL) return ESP_ERR_INVALID_STATE;
    return s_ctx.nvs.load_string(key, value, max_len, s_ctx.nvs.user_ctx);
}

static esp_err_t nvs_save_u32(const char *key, uint32_t value) {
    if (s_ctx.nvs.save_uint32 == NULL) return ESP_ERR_INVALID_STATE;
    return s_ctx.nvs.save_uint32(key, value, s_ctx.nvs.user_ctx);
}

static esp_err_t nvs_load_u32(const char *key, uint32_t *value) {
    if (s_ctx.nvs.load_uint32 == NULL) return ESP_ERR_INVALID_STATE;
    return s_ctx.nvs.load_uint32(key, value, s_ctx.nvs.user_ctx);
}

bool license_has_auth_token(void) {
    if (!s_ctx.initialized) return false;
    char buf[LICENSE_AUTH_TOKEN_MAX] = {0};
    if (nvs_load_str(LICENSE_NVS_AUTH_TOKEN_KEY, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    return buf[0] != '\0';
}

// ------------------------------------------------------------------
// URL building
// ------------------------------------------------------------------

static void resolve_host(char *out, size_t cap) {
    char nvs_host[LICENSE_URL_MAX];
    if (nvs_load_str(LICENSE_NVS_CLOUD_HOST_KEY, nvs_host, sizeof(nvs_host)) == ESP_OK &&
        nvs_host[0] != '\0') {
        strncpy(out, nvs_host, cap - (size_t)1);
        out[cap - (size_t)1] = '\0';
        return;
    }
    strncpy(out, LICENSE_DEFAULT_HOST, cap - (size_t)1);
    out[cap - (size_t)1] = '\0';
}

static void build_url(const char *path, char *out, size_t cap) {
    char host[LICENSE_URL_MAX];
    resolve_host(host, sizeof(host));
    snprintf(out, cap, "%s%s", host, path);
}

static esp_err_t load_auth_token(char *out, size_t cap) {
    if (nvs_load_str(LICENSE_NVS_AUTH_TOKEN_KEY, out, cap) != ESP_OK ||
        out[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

// ------------------------------------------------------------------
// Persistence
// ------------------------------------------------------------------

esp_err_t license_load_cache(void) {
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;

    license_state_t st;
    memset(&st, 0, sizeof(st));

    uint32_t u = 0;
    if (nvs_load_u32(LICENSE_NVS_PRESENT_KEY, &u) == ESP_OK) {
        st.present = (u != (uint32_t)0);
    }
    if (nvs_load_u32(LICENSE_NVS_PAID_KEY, &u) == ESP_OK) {
        st.paid = (u != (uint32_t)0);
    }
    if (nvs_load_u32(LICENSE_NVS_REVOKED_KEY, &u) == ESP_OK) {
        st.revoked = (u != (uint32_t)0);
    }
    nvs_load_str(LICENSE_NVS_VIN_KEY, st.vin, sizeof(st.vin));
    nvs_load_str(LICENSE_NVS_REVOKED_REASON_KEY, st.revoked_reason, sizeof(st.revoked_reason));
    nvs_load_u32(LICENSE_NVS_LAST_SYNC_KEY, &st.last_sync_ms);

    s_ctx.state = st;
    ESP_LOGI(TAG, "cache loaded: present=%d paid=%d revoked=%d vin='%s'",
             (int)st.present, (int)st.paid, (int)st.revoked, st.vin);
    return ESP_OK;
}

esp_err_t license_save_cache(void) {
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    nvs_save_u32(LICENSE_NVS_PRESENT_KEY, s_ctx.state.present ? (uint32_t)1 : (uint32_t)0);
    nvs_save_u32(LICENSE_NVS_PAID_KEY,    s_ctx.state.paid    ? (uint32_t)1 : (uint32_t)0);
    nvs_save_u32(LICENSE_NVS_REVOKED_KEY, s_ctx.state.revoked ? (uint32_t)1 : (uint32_t)0);
    nvs_save_str(LICENSE_NVS_VIN_KEY,            s_ctx.state.vin);
    nvs_save_str(LICENSE_NVS_REVOKED_REASON_KEY, s_ctx.state.revoked_reason);
    nvs_save_u32(LICENSE_NVS_LAST_SYNC_KEY,      s_ctx.state.last_sync_ms);
    return ESP_OK;
}

// ------------------------------------------------------------------
// Init / deinit
// ------------------------------------------------------------------

esp_err_t license_init(const license_module_config_t *cfg) {
    if (s_ctx.initialized) return ESP_OK;
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->http.get == NULL || cfg->http.post == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->nvs.save_string == NULL || cfg->nvs.load_string == NULL ||
        cfg->nvs.save_uint32 == NULL || cfg->nvs.load_uint32 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.http        = cfg->http;
    s_ctx.nvs         = cfg->nvs;
    s_ctx.initialized = true;

    license_load_cache();
    ESP_LOGI(TAG, "license module initialized");
    return ESP_OK;
}

void license_deinit(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
}

void license_test_seed(const license_state_t *seed) {
    if (!s_ctx.initialized || seed == NULL) return;
    s_ctx.state = *seed;
}

const license_state_t *license_get_state(void) {
    return &s_ctx.state;
}

// ------------------------------------------------------------------
// Tiny JSON helpers (no dependency on cJSON for the host build)
// ------------------------------------------------------------------

// Find the value following "key": in src. Returns pointer to the
// first character of the value (after the colon, with whitespace
// trimmed), or NULL. The key is matched as a literal, not a regex.
static const char *json_find_key(const char *src, const char *key) {
    if (src == NULL || key == NULL) return NULL;
    char needle[LICENSE_JSON_NEEDLE_BUF_MAX];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(src, needle);
    if (p == NULL) return NULL;
    p += strlen(needle);
    while (*p != '\0' && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p != ':') return NULL;
    p++;
    while (*p != '\0' && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return *p != '\0' ? p : NULL;
}

// Read a JSON boolean keyed by `key`. Returns true on parse success
// (and writes *out), false if the key is missing or value is neither
// `true`/`false`/0/1.
static bool json_get_bool(const char *src, const char *key, bool *out) {
    const char *v = json_find_key(src, key);
    if (v == NULL) return false;
    if (strncmp(v, "true",  (size_t)4) == (int)0) { *out = true;  return true; }
    if (strncmp(v, "false", (size_t)5) == (int)0) { *out = false; return true; }
    if (*v == '1') { *out = true;  return true; }
    if (*v == '0') { *out = false; return true; }
    return false;
}

// Read a JSON string keyed by `key` into out (NUL-terminated, clamped
// to cap). Treats JSON `null` as empty string. Returns true on parse
// success, false if the key is missing.
static bool json_get_string(const char *src, const char *key, char *out, size_t cap) {
    if (out == NULL || cap == (size_t)0) return false;
    out[0] = '\0';
    const char *v = json_find_key(src, key);
    if (v == NULL) return false;
    if (strncmp(v, "null", (size_t)4) == (int)0) {
        return true; // explicit null → empty
    }
    if (*v != '"') return false;
    v++;
    size_t i = (size_t)0;
    while (*v != '\0' && *v != '"' && i + (size_t)1 < cap) {
        // Minimal escape handling: \" \\ \n \t. Other escapes are passed through.
        if (*v == '\\' && v[1] != '\0') {
            char esc = v[1];
            char ch = esc;
            switch (esc) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '"':
                case '\\':
                case '/':  ch = esc; break;
                default:   ch = esc; break;
            }
            out[i++] = ch;
            v += LICENSE_JSON_ESCAPE_CHAR_BYTES;
        } else {
            out[i++] = *v++;
        }
    }
    out[i] = '\0';
    return true;
}

// ------------------------------------------------------------------
// Cloud round-trips
// ------------------------------------------------------------------

esp_err_t license_post_register(const char *mac,
                                const char *vin,
                                const char *boxcode,
                                int        *http_status_out,
                                char       *err_out,
                                size_t      err_cap) {
    if (http_status_out != NULL) *http_status_out = (int)0;
    if (!s_ctx.initialized) {
        set_reason(err_out, err_cap, "license module not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (mac == NULL) {
        set_reason(err_out, err_cap, "register: mac required");
        return ESP_ERR_INVALID_ARG;
    }

    char token[LICENSE_AUTH_TOKEN_MAX] = {0};
    if (load_auth_token(token, sizeof(token)) != ESP_OK) {
        set_reason(err_out, err_cap, "device not enrolled (auth_token missing)");
        return ESP_ERR_NOT_FOUND;
    }

    char url[LICENSE_URL_MAX];
    build_url(LICENSE_REGISTER_PATH, url, sizeof(url));

    // Build JSON body. Fields are passed straight through; cloud
    // does its own normalize at the comparison site.
    char body[LICENSE_REGISTER_BODY_MAX];
    snprintf(body, sizeof(body),
             "{\"mac\":\"%s\",\"vin\":\"%s\",\"boxcode\":\"%s\"}",
             mac,
             vin != NULL ? vin : "",
             boxcode != NULL ? boxcode : "");

    uint8_t resp[LICENSE_RESPONSE_BUF_MAX];
    size_t  resp_len = (size_t)0;
    int status = s_ctx.http.post(url, token,
                                 (const uint8_t *)body, strlen(body),
                                 resp, sizeof(resp), &resp_len,
                                 (uint32_t)LICENSE_HTTP_TIMEOUT_MS,
                                 s_ctx.http.user_ctx);
    if (http_status_out != NULL) *http_status_out = status;

    if (status < 0) {
        set_reason(err_out, err_cap, "register: transport error rc=%d", status);
        return ESP_FAIL;
    }
    if (status == (int)LICENSE_HTTP_STATUS_CONFLICT) {
        set_reason(err_out, err_cap,
                   "register: VIN already paired on cloud (HTTP 409)");
        return ESP_ERR_INVALID_STATE;
    }
    if (status < (int)LICENSE_HTTP_OK_MIN || status > (int)LICENSE_HTTP_OK_MAX) {
        set_reason(err_out, err_cap, "register: HTTP %d", status);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "register OK (HTTP %d)", status);
    return ESP_OK;
}

esp_err_t license_fetch(int    *http_status_out,
                        char   *err_out,
                        size_t  err_cap) {
    if (http_status_out != NULL) *http_status_out = (int)0;
    if (!s_ctx.initialized) {
        set_reason(err_out, err_cap, "license module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    char token[LICENSE_AUTH_TOKEN_MAX] = {0};
    if (load_auth_token(token, sizeof(token)) != ESP_OK) {
        set_reason(err_out, err_cap, "device not enrolled (auth_token missing)");
        return ESP_ERR_NOT_FOUND;
    }

    char url[LICENSE_URL_MAX];
    build_url(LICENSE_LICENSE_PATH, url, sizeof(url));

    uint8_t resp[LICENSE_RESPONSE_BUF_MAX];
    size_t  resp_len = (size_t)0;
    int status = s_ctx.http.get(url, token, resp, sizeof(resp), &resp_len,
                                (uint32_t)LICENSE_HTTP_TIMEOUT_MS,
                                s_ctx.http.user_ctx);
    if (http_status_out != NULL) *http_status_out = status;

    if (status < 0) {
        // Network / DNS / TLS / timeout. Per §6.3 the cache is forever
        // in this case — leave state untouched.
        set_reason(err_out, err_cap, "license: transport error rc=%d (cache retained)", status);
        return ESP_FAIL;
    }
    if (status == (int)LICENSE_HTTP_STATUS_UNAUTHORIZED) {
        // Token invalid; clear in-memory state so refresh is forced.
        // Persisted cache also flipped to present=false so a future
        // re-enroll resets the state cleanly.
        memset(&s_ctx.state, 0, sizeof(s_ctx.state));
        license_save_cache();
        set_reason(err_out, err_cap, "license: HTTP 401 — token rejected, cache cleared");
        return ESP_ERR_INVALID_STATE;
    }
    if (status < (int)LICENSE_HTTP_OK_MIN || status > (int)LICENSE_HTTP_OK_MAX) {
        set_reason(err_out, err_cap, "license: HTTP %d (cache retained)", status);
        return ESP_FAIL;
    }

    // 2xx — parse the JSON response.
    if (resp_len == (size_t)0) {
        set_reason(err_out, err_cap, "license: empty response body");
        return ESP_FAIL;
    }
    // Ensure NUL-termination for the JSON parser.
    if (resp_len >= sizeof(resp)) resp_len = sizeof(resp) - (size_t)1;
    resp[resp_len] = (uint8_t)'\0';
    const char *json = (const char *)resp;

    license_state_t next;
    memset(&next, 0, sizeof(next));
    next.present = true;
    next.last_sync_ms = s_ctx.state.last_sync_ms; // unchanged by parse; bumped below

    if (!json_get_bool(json, "paid", &next.paid)) {
        set_reason(err_out, err_cap, "license: response missing 'paid' field");
        return ESP_FAIL;
    }
    if (!json_get_bool(json, "revoked", &next.revoked)) {
        // Treat missing as not-revoked rather than failing — older
        // server versions may not emit it.
        next.revoked = false;
    }
    json_get_string(json, "vin",            next.vin,            sizeof(next.vin));
    json_get_string(json, "revoked_reason", next.revoked_reason, sizeof(next.revoked_reason));

    s_ctx.state = next;
    license_save_cache();

    ESP_LOGI(TAG, "license refreshed: paid=%d revoked=%d vin='%s' reason='%s'",
             (int)next.paid, (int)next.revoked, next.vin, next.revoked_reason);
    return ESP_OK;
}

// ------------------------------------------------------------------
// Query API
// ------------------------------------------------------------------

bool license_can_run_feature(feature_id_t  feature_id,
                             const char   *current_ecu_vin,
                             char         *reason_out,
                             size_t        reason_cap) {
    if (reason_out != NULL && reason_cap > (size_t)0) reason_out[0] = '\0';

    if (feature_is_diagnostic(feature_id)) {
        return true;
    }
    if (!s_ctx.initialized) {
        set_reason(reason_out, reason_cap, "license module not initialized");
        return false;
    }
    if (!s_ctx.state.present) {
        set_reason(reason_out, reason_cap,
                   "no license cached — run vin_pair_now while online");
        return false;
    }
    if (s_ctx.state.revoked) {
        if (s_ctx.state.revoked_reason[0] != '\0') {
            set_reason(reason_out, reason_cap,
                       "license revoked: %s", s_ctx.state.revoked_reason);
        } else {
            set_reason(reason_out, reason_cap, "license revoked");
        }
        return false;
    }
    if (!s_ctx.state.paid) {
        set_reason(reason_out, reason_cap, "device not paid for this VIN");
        return false;
    }

    // VIN-match check — only if the caller supplied a current ECU VIN.
    if (current_ecu_vin != NULL && current_ecu_vin[0] != '\0') {
        if (!vin_match(current_ecu_vin, s_ctx.state.vin)) {
            char ecu_n[LICENSE_VIN_BUF_LEN];
            char cache_n[LICENSE_VIN_BUF_LEN];
            license_normalize_vin(current_ecu_vin, ecu_n, sizeof(ecu_n));
            license_normalize_vin(s_ctx.state.vin, cache_n, sizeof(cache_n));
            set_reason(reason_out, reason_cap,
                       "VIN mismatch: cached=%s ecu=%s", cache_n, ecu_n);
            return false;
        }
    }

    return true;
}
