#include "vin_pairing.h"
#include "vin_pairing_config.h"
#include "license.h"
#include "license_config.h"
#include "feature_manager.h"

#include "esp_log.h"

#ifndef VIN_PAIRING_HOST_BUILD
#  include "esp_http_client.h"
#  include "cloud/cloud_client.h"
#  include "esp_mac.h"
#  include "esp_err.h"
#  include "nvs/nvs_manager.h"
#  include "state_machine/connection_manager.h"
#  include "wifi/wifi_ap.h"
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// vin_pairing — see vin_pairing.h. The whole module is glue: the
// substantive work lives in license_post_register() and
// license_fetch(). vin_pairing's job is the orchestration:
//   - the "is everything ready?" preconditions
//   - the feature_manager bracketing
//   - the conditional skip of /license when /register returns 409
//   - the err_out wording the WS UI shows the user

static const char *TAG = "VIN_PAIR";

typedef struct {
    bool                              initialized;
    bool                              running;
    vin_pairing_vin_source_fn_t       vin_source;
    vin_pairing_boxcode_source_fn_t   boxcode_source;
    vin_pairing_wifi_ready_fn_t       wifi_ready;
    char                              device_mac[VIN_PAIRING_MAC_STRING_MAX];
} vin_pairing_ctx_t;

static vin_pairing_ctx_t s_ctx;

// ------------------------------------------------------------------
// Internal helpers
// ------------------------------------------------------------------

static void copy_err(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == (size_t)0 || src == NULL) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - (size_t)1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// ------------------------------------------------------------------
// feature_manager descriptor callbacks
// ------------------------------------------------------------------

static esp_err_t vin_pairing_start_cb(void) {
    s_ctx.running = true;
    ESP_LOGI(TAG, "VIN pairing slot acquired");
    return ESP_OK;
}

static esp_err_t vin_pairing_stop_cb(void) {
    s_ctx.running = false;
    ESP_LOGI(TAG, "VIN pairing slot released");
    return ESP_OK;
}

bool vin_pairing_is_running(void) {
    return s_ctx.running;
}

static const feature_descriptor_t k_descriptor = {
    .id         = FEATURE_VIN_PAIRING,
    .name       = VIN_PAIRING_FEATURE_NAME,
    .start      = vin_pairing_start_cb,
    .stop       = vin_pairing_stop_cb,
    .is_running = vin_pairing_is_running,
};

esp_err_t vin_pairing_register_with_feature_manager(void) {
    esp_err_t rc = feature_manager_register(&k_descriptor);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        // INVALID_STATE = already registered → idempotent; treat as OK.
        return rc;
    }
    return ESP_OK;
}

// ------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------

esp_err_t vin_pairing_init(const vin_pairing_config_t *cfg) {
    if (s_ctx.initialized) return ESP_OK;
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->vin_source == NULL || cfg->boxcode_source == NULL || cfg->wifi_ready == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->device_mac == NULL || cfg->device_mac[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.vin_source     = cfg->vin_source;
    s_ctx.boxcode_source = cfg->boxcode_source;
    s_ctx.wifi_ready     = cfg->wifi_ready;
    strncpy(s_ctx.device_mac, cfg->device_mac, sizeof(s_ctx.device_mac) - (size_t)1);
    s_ctx.device_mac[sizeof(s_ctx.device_mac) - (size_t)1] = '\0';
    s_ctx.initialized = true;

    esp_err_t rc = vin_pairing_register_with_feature_manager();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "feature_manager_register rc=%d", (int)rc);
        return rc;
    }
    ESP_LOGI(TAG, "vin_pairing initialized (mac=%s)", s_ctx.device_mac);
    return ESP_OK;
}

void vin_pairing_deinit(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
}

// ------------------------------------------------------------------
// Pair-or-refresh sequence
// ------------------------------------------------------------------

esp_err_t vin_pairing_run_now(char *err_out, size_t err_cap) {
    if (err_out != NULL && err_cap > (size_t)0) err_out[0] = '\0';
    if (!s_ctx.initialized) {
        copy_err(err_out, err_cap, "vin_pairing not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Pre-flight checks (no feature_manager slot needed yet).
    const char *vin     = s_ctx.vin_source();
    const char *boxcode = s_ctx.boxcode_source();
    if (vin == NULL || vin[0] == '\0') {
        copy_err(err_out, err_cap, "ECU VIN not yet read; wait for connection_manager to discover");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_ctx.wifi_ready()) {
        copy_err(err_out, err_cap, "Wi-Fi STA not connected; pair via AP first or wait for STA reconnect");
        return ESP_ERR_INVALID_STATE;
    }
    if (!license_has_auth_token()) {
        copy_err(err_out, err_cap,
                 "device not enrolled (auth_token missing); admin must run /admin/devices then set_auth_token");
        return ESP_ERR_NOT_FOUND;
    }

    // Acquire the feature_manager slot. This will preempt any active
    // feature per the standard arbitration semantics.
    char fm_err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t fm_rc = feature_manager_request_start(FEATURE_VIN_PAIRING, fm_err, sizeof(fm_err));
    if (fm_rc != ESP_OK) {
        ESP_LOGW(TAG, "feature_manager request_start rc=%d (%s)", (int)fm_rc, fm_err);
        copy_err(err_out, err_cap, fm_err[0] != '\0' ? fm_err : "feature_manager rejected vin_pairing start");
        return fm_rc;
    }

    int        register_status = (int)0;
    char       op_err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t  reg_rc = license_post_register(s_ctx.device_mac,
                                              vin,
                                              boxcode != NULL ? boxcode : "",
                                              &register_status,
                                              op_err, sizeof(op_err));

    esp_err_t outcome = ESP_OK;
    if (reg_rc != ESP_OK) {
        // 409 (VIN-already-paired) is special-cased so we DO NOT
        // proceed to /license — the cloud says this dongle isn't
        // ours for this VIN.
        if (register_status == (int)LICENSE_HTTP_STATUS_CONFLICT) {
            ESP_LOGW(TAG, "register returned 409; license fetch skipped");
            copy_err(err_out, err_cap, op_err);
            outcome = ESP_ERR_INVALID_STATE;
        } else {
            ESP_LOGE(TAG, "register failed rc=%d (%s)", (int)reg_rc, op_err);
            copy_err(err_out, err_cap, op_err);
            outcome = reg_rc;
        }
    } else {
        // Register OK. Now fetch the license.
        int license_status = (int)0;
        op_err[0] = '\0';
        esp_err_t lic_rc = license_fetch(&license_status, op_err, sizeof(op_err));
        if (lic_rc != ESP_OK) {
            ESP_LOGW(TAG, "license_fetch rc=%d status=%d (%s)",
                     (int)lic_rc, license_status, op_err);
            copy_err(err_out, err_cap, op_err);
            outcome = lic_rc;
        } else {
            ESP_LOGI(TAG, "vin_pairing OK; license refreshed");
#ifndef VIN_PAIRING_HOST_BUILD
            /* P-58: persist ECU pair record to NVS so the dongle reports
             * paired=true on the NEXT boot. Without this, the cloud-side
             * license cache persisted (license_status.present=true) but
             * the local ECU pair record never landed in NVS, so on the
             * next power cycle connection_manager's CHECK_PAIRING handler
             * found "No valid ECU info found in NVS" and reverted to
             * paired=false. The customer expectation is "pair this dongle
             * with my car" = both cloud license AND local pairing.
             *
             * connection_manager_pair_vehicle() writes ecu_info to NVS
             * via nvs_manager_save_ecu_info() and flips is_paired=true,
             * then forces a clean disconnect+reconnect to apply the
             * paired state cleanly. The brief reconnect is acceptable
             * UX for a one-time pairing action; matches existing
             * cmd_pair_ecu admin-path behavior.
             *
             * Treat persistence failure as non-fatal: cloud side already
             * succeeded; user can retry pair_ecu manually. */
            esp_err_t pair_rc = connection_manager_pair_vehicle();
            if (pair_rc != ESP_OK && pair_rc != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG,
                         "P-58: connection_manager_pair_vehicle rc=%d "
                         "(license OK, ECU NVS pair not persisted)",
                         (int)pair_rc);
            }
#endif
        }
    }

    // Always release the slot, regardless of outcome.
    esp_err_t stop_rc = feature_manager_request_stop(FEATURE_VIN_PAIRING);
    if (stop_rc != ESP_OK) {
        ESP_LOGE(TAG, "feature_manager request_stop rc=%d (vin_pairing release)", (int)stop_rc);
    }
    return outcome;
}

// ------------------------------------------------------------------
// On-target adapters + integration entry point (compiled out under
// VIN_PAIRING_HOST_BUILD so the host unit test wires its own mocks)
// ------------------------------------------------------------------

#ifndef VIN_PAIRING_HOST_BUILD

/* Static storage for the resolved device MAC string used by both the
 * on-target VIN-pair flow and the integration entry point. */
static char s_target_mac[VIN_PAIRING_MAC_STRING_MAX];

static const char *target_vin_source(void)     { return connection_manager_get_vin(); }
static const char *target_boxcode_source(void) { return connection_manager_get_boxcode(); }
static bool        target_wifi_ready(void)     { return wifi_client_is_connected(); }

/* HTTP transport adapters around esp_http_client. The license module
 * passes us the URL, optional Bearer token (without the "Bearer "
 * prefix), and a body buffer; we wire those into the IDF API. */
static int build_auth_header(const char *bearer, char *out, size_t out_cap) {
    if (bearer == NULL || bearer[0] == '\0') return (int)0;
    snprintf(out, out_cap, "%s%s", LICENSE_BEARER_PREFIX, bearer);
    return (int)1;
}

static int target_http_get(const char *url, const char *bearer,
                           uint8_t *body_out, size_t body_cap, size_t *body_len_out,
                           uint32_t timeout_ms, void *ctx) {
    (void)ctx;
    if (body_len_out != NULL) *body_len_out = (size_t)0;
    /* P-49: TLS config centralized in cloud_client. */
    esp_http_client_handle_t cli = cloud_client_https_init(url, HTTP_METHOD_GET, (int)timeout_ms);
    if (cli == NULL) return (int)ESP_FAIL;

    char auth[LICENSE_AUTH_HEADER_MAX];
    if (build_auth_header(bearer, auth, sizeof(auth)) != (int)0) {
        esp_http_client_set_header(cli, "Authorization", auth);
    }

    esp_err_t open_rc = esp_http_client_open(cli, (int)0);
    if (open_rc != ESP_OK) {
        esp_http_client_cleanup(cli);
        return (int)open_rc;
    }
    int content_len = (int)esp_http_client_fetch_headers(cli);
    int status = (int)esp_http_client_get_status_code(cli);
    if (content_len > (int)0 && body_out != NULL && body_cap > (size_t)0) {
        int read_n = esp_http_client_read_response(cli, (char *)body_out, (int)body_cap);
        if (read_n > (int)0 && body_len_out != NULL) {
            *body_len_out = (size_t)read_n;
        }
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return status;
}

static int target_http_post(const char *url, const char *bearer,
                            const uint8_t *body, size_t body_len,
                            uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out,
                            uint32_t timeout_ms, void *ctx) {
    (void)ctx;
    if (resp_len_out != NULL) *resp_len_out = (size_t)0;
    /* P-49: TLS config centralized in cloud_client. */
    esp_http_client_handle_t cli = cloud_client_https_init(url, HTTP_METHOD_POST, (int)timeout_ms);
    if (cli == NULL) return (int)ESP_FAIL;

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    char auth[LICENSE_AUTH_HEADER_MAX];
    if (build_auth_header(bearer, auth, sizeof(auth)) != (int)0) {
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    esp_http_client_set_post_field(cli, (const char *)body, (int)body_len);

    esp_err_t perform_rc = esp_http_client_perform(cli);
    int status;
    if (perform_rc == ESP_OK) {
        status = (int)esp_http_client_get_status_code(cli);
        if (resp_out != NULL && resp_cap > (size_t)0) {
            int read_n = esp_http_client_read_response(cli, (char *)resp_out, (int)resp_cap);
            if (read_n > (int)0 && resp_len_out != NULL) {
                *resp_len_out = (size_t)read_n;
            }
        }
    } else {
        status = (int)perform_rc;
    }
    esp_http_client_cleanup(cli);
    return status;
}

/* NVS adapters — the license module's iface is structurally identical
 * to nvs_manager's API except for the user_ctx slot we don't use. */
static esp_err_t target_nvs_save_str(const char *key, const char *value, void *ctx) {
    (void)ctx;
    return nvs_manager_save_string(key, value);
}
static esp_err_t target_nvs_load_str(const char *key, char *value, size_t max_len, void *ctx) {
    (void)ctx;
    return nvs_manager_load_string(key, value, max_len);
}
static esp_err_t target_nvs_save_u32(const char *key, uint32_t value, void *ctx) {
    (void)ctx;
    return nvs_manager_save_uint32(key, value);
}
static esp_err_t target_nvs_load_u32(const char *key, uint32_t *value, void *ctx) {
    (void)ctx;
    return nvs_manager_load_uint32(key, value);
}

/* Integration entry point. Called from main.c once at boot. */
esp_err_t main_init_license_and_vin_pairing(void) {
    /* Resolve device MAC (formatted "AA:BB:CC:DD:EE:FF"). */
    uint8_t mac_raw[6] = {0};
    esp_efuse_mac_get_default(mac_raw);
    snprintf(s_target_mac, sizeof(s_target_mac),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_raw[0], mac_raw[1], mac_raw[2], mac_raw[3], mac_raw[4], mac_raw[5]);

    license_module_config_t lic_cfg = {
        .http = {
            .get      = target_http_get,
            .post     = target_http_post,
            .user_ctx = NULL,
        },
        .nvs = {
            .save_string = target_nvs_save_str,
            .load_string = target_nvs_load_str,
            .save_uint32 = target_nvs_save_u32,
            .load_uint32 = target_nvs_load_u32,
            .user_ctx    = NULL,
        },
    };
    esp_err_t rc = license_init(&lic_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "license_init rc=%d", (int)rc);
        return rc;
    }

    vin_pairing_config_t vp_cfg = {
        .vin_source     = target_vin_source,
        .boxcode_source = target_boxcode_source,
        .wifi_ready     = target_wifi_ready,
        .device_mac     = s_target_mac,
    };
    rc = vin_pairing_init(&vp_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "vin_pairing_init rc=%d", (int)rc);
        return rc;
    }
    ESP_LOGI(TAG, "license + vin_pairing wired (mac=%s)", s_target_mac);
    return ESP_OK;
}

#endif /* !VIN_PAIRING_HOST_BUILD */
