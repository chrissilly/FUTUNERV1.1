/*
 * test_vin_pairing.c — host-runnable unit tests for the VIN pairing
 * + license modules.
 *
 * Built into firmware/test/vin_pairing/host_test_runner via the
 * Makefile in that directory and exercised by
 * firmware/test/vin_pairing/eval.sh.
 *
 * Required scenarios (per the kickoff prompt):
 *   1. Cold pairing — no Wi-Fi, no auth_token cached.
 *   2. Warm pairing — Wi-Fi up but no token cached.
 *   3. VIN match — cache present, ECU VIN matches → can_run = true.
 *   4. VIN mismatch — cache present, ECU VIN differs → can_run = false
 *      for gated features, true for diagnostic features.
 *   5. License revoked — cloud returns revoked=true → can_run = false.
 *   6. Wi-Fi unavailable — cached license forever, no transport call.
 *   + register 409 → license fetch skipped, error surfaced.
 *   + license 401 → cache cleared, next sync forced.
 *   + license 5xx / network → cache untouched (offline grace).
 *   + VIN normalization (whitespace + uppercase ISO 3779).
 *   + arg validation.
 */

#include "license.h"
#include "license_config.h"
#include "vin_pairing.h"
#include "vin_pairing_config.h"
#include "feature_manager.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tiny EXPECT framework                                               */
/* ------------------------------------------------------------------ */

static int g_failures = 0;

#define EXPECT(cond, msg) do {                                              \
    if (!(cond)) {                                                          \
        fprintf(stderr, "  FAIL  %s — %s (line %d)\n",                      \
                __func__, (msg), __LINE__);                                 \
        g_failures++;                                                       \
    } else {                                                                \
        fprintf(stdout, "  PASS  %s — %s\n", __func__, (msg));              \
    }                                                                       \
} while (0)

/* ------------------------------------------------------------------ */
/* Mock NVS — in-memory key/value store                                */
/* ------------------------------------------------------------------ */

#define MOCK_NVS_MAX_ENTRIES 32
#define MOCK_NVS_KEY_MAX     32
#define MOCK_NVS_VAL_MAX     128

typedef struct {
    bool     in_use;
    bool     is_uint32;
    char     key[MOCK_NVS_KEY_MAX];
    char     str_val[MOCK_NVS_VAL_MAX];
    uint32_t u32_val;
} mock_nvs_entry_t;

static mock_nvs_entry_t g_nvs[MOCK_NVS_MAX_ENTRIES];

static void mock_nvs_reset(void) { memset(g_nvs, 0, sizeof(g_nvs)); }

static int mock_nvs_find(const char *key) {
    for (int i = 0; i < MOCK_NVS_MAX_ENTRIES; i++) {
        if (g_nvs[i].in_use && strcmp(g_nvs[i].key, key) == 0) return i;
    }
    return -1;
}
static int mock_nvs_alloc(const char *key) {
    int slot = mock_nvs_find(key);
    if (slot >= 0) return slot;
    for (int i = 0; i < MOCK_NVS_MAX_ENTRIES; i++) {
        if (!g_nvs[i].in_use) {
            g_nvs[i].in_use = true;
            strncpy(g_nvs[i].key, key, MOCK_NVS_KEY_MAX - 1);
            g_nvs[i].key[MOCK_NVS_KEY_MAX - 1] = '\0';
            return i;
        }
    }
    return -1;
}

static esp_err_t mock_nvs_save_str(const char *key, const char *value, void *ctx) {
    (void)ctx;
    int slot = mock_nvs_alloc(key);
    if (slot < 0) return ESP_ERR_NO_MEM;
    g_nvs[slot].is_uint32 = false;
    strncpy(g_nvs[slot].str_val, value != NULL ? value : "", MOCK_NVS_VAL_MAX - 1);
    g_nvs[slot].str_val[MOCK_NVS_VAL_MAX - 1] = '\0';
    return ESP_OK;
}
static esp_err_t mock_nvs_load_str(const char *key, char *out, size_t cap, void *ctx) {
    (void)ctx;
    if (out == NULL || cap == 0) return ESP_ERR_INVALID_ARG;
    int slot = mock_nvs_find(key);
    if (slot < 0 || g_nvs[slot].is_uint32) {
        out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    strncpy(out, g_nvs[slot].str_val, cap - 1);
    out[cap - 1] = '\0';
    return ESP_OK;
}
static esp_err_t mock_nvs_save_u32(const char *key, uint32_t value, void *ctx) {
    (void)ctx;
    int slot = mock_nvs_alloc(key);
    if (slot < 0) return ESP_ERR_NO_MEM;
    g_nvs[slot].is_uint32 = true;
    g_nvs[slot].u32_val = value;
    return ESP_OK;
}
static esp_err_t mock_nvs_load_u32(const char *key, uint32_t *out, void *ctx) {
    (void)ctx;
    int slot = mock_nvs_find(key);
    if (slot < 0 || !g_nvs[slot].is_uint32) return ESP_ERR_NOT_FOUND;
    *out = g_nvs[slot].u32_val;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Mock HTTP — programmable per-call status + body                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int      status;          /* HTTP status to return; <0 = transport err */
    char     body[1024];      /* response body */
    size_t   body_len;
} mock_http_resp_t;

typedef struct {
    /* Programmable: each call queue dispenses next response. */
    mock_http_resp_t register_resp;   /* used for /api/v1/device/register */
    mock_http_resp_t license_resp;    /* used for /api/v1/license */

    /* Capture: last call's URL + bearer + body. */
    char             last_register_url[256];
    char             last_register_bearer[64];
    char             last_register_body[512];
    char             last_license_url[256];
    char             last_license_bearer[64];
    int              register_calls;
    int              license_calls;
} mock_http_t;

static mock_http_t g_http;

static void mock_http_reset(void) {
    memset(&g_http, 0, sizeof(g_http));
}

static bool url_ends_with(const char *url, const char *suffix) {
    size_t a = strlen(url), b = strlen(suffix);
    return a >= b && strcmp(url + a - b, suffix) == 0;
}

static int mock_http_get(const char *url, const char *bearer,
                         uint8_t *body_out, size_t body_cap, size_t *body_len_out,
                         uint32_t timeout_ms, void *ctx) {
    (void)timeout_ms; (void)ctx;
    if (url_ends_with(url, LICENSE_LICENSE_PATH)) {
        g_http.license_calls++;
        strncpy(g_http.last_license_url, url, sizeof(g_http.last_license_url) - 1);
        g_http.last_license_url[sizeof(g_http.last_license_url) - 1] = '\0';
        if (bearer != NULL) {
            strncpy(g_http.last_license_bearer, bearer, sizeof(g_http.last_license_bearer) - 1);
            g_http.last_license_bearer[sizeof(g_http.last_license_bearer) - 1] = '\0';
        }
        size_t n = g_http.license_resp.body_len;
        if (n > body_cap) n = body_cap;
        if (body_out != NULL && n > 0) memcpy(body_out, g_http.license_resp.body, n);
        if (body_len_out != NULL) *body_len_out = n;
        return g_http.license_resp.status;
    }
    /* Unknown URL — return transport error. */
    return -42;
}

static int mock_http_post(const char *url, const char *bearer,
                          const uint8_t *body, size_t body_len,
                          uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out,
                          uint32_t timeout_ms, void *ctx) {
    (void)timeout_ms; (void)ctx;
    if (url_ends_with(url, LICENSE_REGISTER_PATH)) {
        g_http.register_calls++;
        strncpy(g_http.last_register_url, url, sizeof(g_http.last_register_url) - 1);
        g_http.last_register_url[sizeof(g_http.last_register_url) - 1] = '\0';
        if (bearer != NULL) {
            strncpy(g_http.last_register_bearer, bearer, sizeof(g_http.last_register_bearer) - 1);
            g_http.last_register_bearer[sizeof(g_http.last_register_bearer) - 1] = '\0';
        }
        size_t cap = sizeof(g_http.last_register_body) - 1;
        size_t n = body_len < cap ? body_len : cap;
        if (body != NULL && n > 0) memcpy(g_http.last_register_body, body, n);
        g_http.last_register_body[n] = '\0';

        size_t rn = g_http.register_resp.body_len;
        if (rn > resp_cap) rn = resp_cap;
        if (resp_out != NULL && rn > 0) memcpy(resp_out, g_http.register_resp.body, rn);
        if (resp_len_out != NULL) *resp_len_out = rn;
        return g_http.register_resp.status;
    }
    return -42;
}

/* ------------------------------------------------------------------ */
/* Mock VIN / boxcode / wifi sources                                    */
/* ------------------------------------------------------------------ */

static const char *g_ecu_vin     = "";
static const char *g_ecu_boxcode = "";
static bool        g_wifi_up     = false;

static const char *test_vin_source(void)     { return g_ecu_vin; }
static const char *test_boxcode_source(void) { return g_ecu_boxcode; }
static bool        test_wifi_ready(void)     { return g_wifi_up; }

static const char *TEST_DEVICE_MAC = "AA:BB:CC:DD:EE:01";
static const char *TEST_AUTH_TOKEN = "0123456789abcdef0123456789abcdef";

/* ------------------------------------------------------------------ */
/* Per-test setup                                                      */
/* ------------------------------------------------------------------ */

static void install_license_module(void) {
    license_module_config_t lic_cfg = {
        .http = { .get = mock_http_get, .post = mock_http_post, .user_ctx = NULL },
        .nvs = {
            .save_string = mock_nvs_save_str,
            .load_string = mock_nvs_load_str,
            .save_uint32 = mock_nvs_save_u32,
            .load_uint32 = mock_nvs_load_u32,
            .user_ctx    = NULL,
        },
    };
    license_deinit();
    EXPECT(license_init(&lic_cfg) == ESP_OK, "license_init");
}

static void install_vin_pairing(void) {
    vin_pairing_config_t cfg = {
        .vin_source     = test_vin_source,
        .boxcode_source = test_boxcode_source,
        .wifi_ready     = test_wifi_ready,
        .device_mac     = TEST_DEVICE_MAC,
    };
    vin_pairing_deinit();
    EXPECT(vin_pairing_init(&cfg) == ESP_OK, "vin_pairing_init");
}

static void test_setup(void) {
    /* Drain any active feature from a prior test cleanly. */
    feature_id_t active = feature_manager_active();
    if (active != FEATURE_NONE) feature_manager_request_stop(active);

    mock_nvs_reset();
    mock_http_reset();
    g_ecu_vin = "";
    g_ecu_boxcode = "";
    g_wifi_up = false;

    install_license_module();
    install_vin_pairing();
}

/* Helpers to set canned cloud responses. */
static void mock_register_set(int status, const char *body) {
    g_http.register_resp.status = status;
    g_http.register_resp.body_len = body != NULL ? strlen(body) : 0;
    if (body != NULL) {
        strncpy(g_http.register_resp.body, body, sizeof(g_http.register_resp.body) - 1);
        g_http.register_resp.body[sizeof(g_http.register_resp.body) - 1] = '\0';
    }
}
static void mock_license_set(int status, const char *body) {
    g_http.license_resp.status = status;
    g_http.license_resp.body_len = body != NULL ? strlen(body) : 0;
    if (body != NULL) {
        strncpy(g_http.license_resp.body, body, sizeof(g_http.license_resp.body) - 1);
        g_http.license_resp.body[sizeof(g_http.license_resp.body) - 1] = '\0';
    }
}
static void install_auth_token(const char *token) {
    mock_nvs_save_str(LICENSE_NVS_AUTH_TOKEN_KEY, token, NULL);
}

/* ------------------------------------------------------------------ */
/* Test 1 — VIN normalization (ISO 3779)                                */
/* ------------------------------------------------------------------ */
static void test_vin_normalize(void) {
    test_setup();
    char out[LICENSE_VIN_BUF_LEN];

    license_normalize_vin("WAUZZZ4M9PA000001", out, sizeof(out));
    EXPECT(strcmp(out, "WAUZZZ4M9PA000001") == 0, "uppercase passthrough");

    license_normalize_vin("wauzzz4m9pa000002", out, sizeof(out));
    EXPECT(strcmp(out, "WAUZZZ4M9PA000002") == 0, "lowercase → upper");

    license_normalize_vin("  WAUZZZ4M9PA000003 ", out, sizeof(out));
    EXPECT(strcmp(out, "WAUZZZ4M9PA000003") == 0, "leading + trailing whitespace trimmed");

    license_normalize_vin("\t \n wauzzz4m9pa000004 \r ", out, sizeof(out));
    EXPECT(strcmp(out, "WAUZZZ4M9PA000004") == 0, "mixed whitespace trimmed + uppercased");

    license_normalize_vin(NULL, out, sizeof(out));
    EXPECT(out[0] == '\0', "NULL input → empty output");

    license_normalize_vin("   ", out, sizeof(out));
    EXPECT(out[0] == '\0', "whitespace-only → empty output");
}

/* ------------------------------------------------------------------ */
/* Test 2 — cold pairing (no Wi-Fi, no token)                           */
/* ------------------------------------------------------------------ */
static void test_cold_pairing(void) {
    test_setup();
    g_ecu_vin = "WAUZZZ4M9PA000001";
    g_ecu_boxcode = "4K0907557G__0003";
    g_wifi_up = false;
    /* No auth_token installed. */

    char err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t rc = vin_pairing_run_now(err, sizeof(err));
    EXPECT(rc != ESP_OK, "cold pairing fails");
    EXPECT(g_http.register_calls == 0 && g_http.license_calls == 0,
           "no HTTP calls when Wi-Fi down");
    EXPECT(strstr(err, "Wi-Fi") != NULL, "err mentions Wi-Fi");
    EXPECT(!license_get_state()->present, "no license cached after cold-pair attempt");
}

/* ------------------------------------------------------------------ */
/* Test 3 — warm pairing without token enrolled                         */
/* ------------------------------------------------------------------ */
static void test_warm_pairing_no_token(void) {
    test_setup();
    g_ecu_vin = "WAUZZZ4M9PA000002";
    g_ecu_boxcode = "4K0907557G__0003";
    g_wifi_up = true;
    /* No auth_token installed. */

    char err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t rc = vin_pairing_run_now(err, sizeof(err));
    EXPECT(rc == ESP_ERR_NOT_FOUND, "warm pairing without token returns NOT_FOUND");
    EXPECT(g_http.register_calls == 0, "no /register call when token missing");
    EXPECT(strstr(err, "auth_token") != NULL || strstr(err, "not enrolled") != NULL,
           "err mentions auth_token / enrollment");
}

/* ------------------------------------------------------------------ */
/* Test 4 — full happy-path: register 200 + license 200 paid             */
/* ------------------------------------------------------------------ */
static void test_pair_happy_path(void) {
    test_setup();
    g_ecu_vin = "WAUZZZ4M9PA000003";
    g_ecu_boxcode = "4K0907557G__0003";
    g_wifi_up = true;
    install_auth_token(TEST_AUTH_TOKEN);

    mock_register_set(200, "{\"ok\":true,\"server_time\":1700000000}");
    mock_license_set(200,
        "{\"paid\":true,\"vin\":\"WAUZZZ4M9PA000003\",\"revoked\":false,\"revoked_reason\":null}");

    char err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t rc = vin_pairing_run_now(err, sizeof(err));
    EXPECT(rc == ESP_OK, "happy path returns ESP_OK");
    EXPECT(g_http.register_calls == 1, "register POST issued exactly once");
    EXPECT(g_http.license_calls == 1, "license GET issued exactly once");
    EXPECT(strcmp(g_http.last_register_bearer, TEST_AUTH_TOKEN) == 0,
           "register Bearer carries auth_token");
    EXPECT(strstr(g_http.last_register_body, "WAUZZZ4M9PA000003") != NULL,
           "register body carries VIN");
    EXPECT(strstr(g_http.last_register_body, "4K0907557G__0003") != NULL,
           "register body carries boxcode");
    EXPECT(strstr(g_http.last_register_body, TEST_DEVICE_MAC) != NULL,
           "register body carries MAC");

    const license_state_t *st = license_get_state();
    EXPECT(st->present, "license cache present");
    EXPECT(st->paid, "paid=true");
    EXPECT(!st->revoked, "revoked=false");
    EXPECT(strcmp(st->vin, "WAUZZZ4M9PA000003") == 0, "cached VIN matches cloud");
    EXPECT(feature_manager_active() == FEATURE_NONE,
           "feature_manager released after vin_pairing_run_now returns");
}

/* ------------------------------------------------------------------ */
/* Test 5 — VIN match → can_run_feature = true for gated features        */
/* ------------------------------------------------------------------ */
static void test_can_run_vin_match(void) {
    test_setup();
    license_state_t seed = {
        .present = true, .paid = true, .revoked = false,
        .vin = "WAUZZZ4M9PA000010",
    };
    license_test_seed(&seed);

    char r[128];
    EXPECT(license_can_run_feature(FEATURE_WOT_LOGGING, "WAUZZZ4M9PA000010", r, sizeof(r)),
           "WOT_LOGGING allowed when VIN matches");
    EXPECT(license_can_run_feature(FEATURE_LIVE_TUNE,   "WAUZZZ4M9PA000010", r, sizeof(r)),
           "LIVE_TUNE allowed when VIN matches");
    EXPECT(license_can_run_feature(FEATURE_PHASE2_FLASH, "WAUZZZ4M9PA000010", r, sizeof(r)),
           "PHASE2_FLASH allowed when VIN matches");

    /* Normalization: cached uppercase, ECU emits messy lowercase + whitespace. */
    EXPECT(license_can_run_feature(FEATURE_WOT_LOGGING, "  wauzzz4m9pa000010 ", r, sizeof(r)),
           "WOT_LOGGING allowed when VIN matches under ISO-3779 normalization");
}

/* ------------------------------------------------------------------ */
/* Test 6 — VIN mismatch → diagnostic OK, gated refused                  */
/* ------------------------------------------------------------------ */
static void test_can_run_vin_mismatch(void) {
    test_setup();
    license_state_t seed = {
        .present = true, .paid = true, .revoked = false,
        .vin = "WAUZZZ4M9PA000011",
    };
    license_test_seed(&seed);

    char r[128] = {0};
    EXPECT(!license_can_run_feature(FEATURE_WOT_LOGGING,  "WAUZZZ4M9PA999999", r, sizeof(r)),
           "WOT_LOGGING refused on VIN mismatch");
    EXPECT(strstr(r, "VIN mismatch") != NULL || strstr(r, "mismatch") != NULL,
           "reason mentions mismatch");
    EXPECT(!license_can_run_feature(FEATURE_LIVE_TUNE,    "WAUZZZ4M9PA999999", r, sizeof(r)),
           "LIVE_TUNE refused on VIN mismatch");
    EXPECT(!license_can_run_feature(FEATURE_PHASE2_FLASH, "WAUZZZ4M9PA999999", r, sizeof(r)),
           "PHASE2_FLASH refused on VIN mismatch");

    /* Diagnostic features remain allowed even on VIN mismatch. */
    EXPECT(license_can_run_feature(FEATURE_DTC,           "WAUZZZ4M9PA999999", r, sizeof(r)),
           "FEATURE_DTC stays allowed on VIN mismatch (diagnostic)");
    EXPECT(license_can_run_feature(FEATURE_VIN_PAIRING,   "WAUZZZ4M9PA999999", r, sizeof(r)),
           "FEATURE_VIN_PAIRING stays allowed on VIN mismatch");
}

/* ------------------------------------------------------------------ */
/* Test 7 — license revoked → all gated features refused                 */
/* ------------------------------------------------------------------ */
static void test_can_run_revoked(void) {
    test_setup();
    license_state_t seed = {
        .present = true, .paid = true, .revoked = true,
        .vin = "WAUZZZ4M9PA000012",
        .revoked_reason = "chargeback",
    };
    license_test_seed(&seed);

    char r[128] = {0};
    EXPECT(!license_can_run_feature(FEATURE_WOT_LOGGING, "WAUZZZ4M9PA000012", r, sizeof(r)),
           "revoked → WOT_LOGGING refused");
    EXPECT(strstr(r, "revoked") != NULL, "reason mentions revoked");
    EXPECT(strstr(r, "chargeback") != NULL, "reason includes revoked_reason");
    EXPECT(!license_can_run_feature(FEATURE_LIVE_TUNE,    "WAUZZZ4M9PA000012", r, sizeof(r)),
           "revoked → LIVE_TUNE refused");
    EXPECT(!license_can_run_feature(FEATURE_PHASE2_FLASH, "WAUZZZ4M9PA000012", r, sizeof(r)),
           "revoked → PHASE2_FLASH refused");

    /* Diagnostic features remain allowed even on revoke (per §6.4
     * dongle still reads diagnostics). */
    EXPECT(license_can_run_feature(FEATURE_DTC, "WAUZZZ4M9PA000012", r, sizeof(r)),
           "revoked → FEATURE_DTC still allowed");
}

/* ------------------------------------------------------------------ */
/* Test 8 — register 409 → license fetch skipped, error surfaced         */
/* ------------------------------------------------------------------ */
static void test_register_409_skips_license(void) {
    test_setup();
    g_ecu_vin = "WAUZZZ4M9PA000020";
    g_ecu_boxcode = "4K0907557G__0003";
    g_wifi_up = true;
    install_auth_token(TEST_AUTH_TOKEN);

    mock_register_set(409, "{\"detail\":\"VIN already paired\"}");
    mock_license_set(200,  "{\"paid\":true,\"vin\":\"X\",\"revoked\":false}");

    char err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t rc = vin_pairing_run_now(err, sizeof(err));
    EXPECT(rc != ESP_OK, "409 from register propagates as error");
    EXPECT(g_http.register_calls == 1, "register attempted once");
    EXPECT(g_http.license_calls == 0, "license NOT fetched when register 409");
    EXPECT(strstr(err, "409") != NULL || strstr(err, "VIN already paired") != NULL,
           "err mentions 409 or 'VIN already paired'");
    EXPECT(!license_get_state()->present, "license cache untouched on 409");
}

/* ------------------------------------------------------------------ */
/* Test 9 — license 401 → in-memory cache cleared                        */
/* ------------------------------------------------------------------ */
static void test_license_401_clears_cache(void) {
    test_setup();
    /* Pre-seed a cache so we can observe it being cleared. */
    license_state_t seed = {
        .present = true, .paid = true, .revoked = false,
        .vin = "WAUZZZ4M9PA000030",
    };
    license_test_seed(&seed);
    license_save_cache();
    EXPECT(license_get_state()->present, "precondition: cache present");

    g_ecu_vin = "WAUZZZ4M9PA000030";
    g_ecu_boxcode = "4K0907557G__0003";
    g_wifi_up = true;
    install_auth_token(TEST_AUTH_TOKEN);

    mock_register_set(200, "{\"ok\":true}");
    mock_license_set(401, "{\"detail\":\"Unknown token\"}");

    char err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t rc = vin_pairing_run_now(err, sizeof(err));
    EXPECT(rc != ESP_OK, "401 surfaces as error");
    EXPECT(!license_get_state()->present, "401 clears in-memory cache");
    EXPECT(strstr(err, "401") != NULL || strstr(err, "token") != NULL,
           "err mentions 401 or token");
}

/* ------------------------------------------------------------------ */
/* Test 10 — license 5xx / network → cache untouched (offline grace)     */
/* ------------------------------------------------------------------ */
static void test_license_5xx_keeps_cache(void) {
    test_setup();
    license_state_t seed = {
        .present = true, .paid = true, .revoked = false,
        .vin = "WAUZZZ4M9PA000040",
    };
    license_test_seed(&seed);
    license_save_cache();

    g_ecu_vin = "WAUZZZ4M9PA000040";
    g_ecu_boxcode = "4K0907557G__0003";
    g_wifi_up = true;
    install_auth_token(TEST_AUTH_TOKEN);

    mock_register_set(200, "{\"ok\":true}");
    mock_license_set(503, "{\"detail\":\"Service Unavailable\"}");

    char err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t rc = vin_pairing_run_now(err, sizeof(err));
    EXPECT(rc != ESP_OK, "5xx surfaces as error");
    const license_state_t *st = license_get_state();
    EXPECT(st->present && st->paid && !st->revoked,
           "5xx leaves cache untouched (offline grace per §6.3)");
    EXPECT(strcmp(st->vin, "WAUZZZ4M9PA000040") == 0,
           "cached VIN preserved across 5xx");
}

/* ------------------------------------------------------------------ */
/* Test 11 — Wi-Fi unavailable on boot, cached license still queryable   */
/* ------------------------------------------------------------------ */
static void test_wifi_unavailable_uses_cache(void) {
    test_setup();
    license_state_t seed = {
        .present = true, .paid = true, .revoked = false,
        .vin = "WAUZZZ4M9PA000050",
    };
    license_test_seed(&seed);
    license_save_cache();

    g_ecu_vin = "WAUZZZ4M9PA000050";
    g_ecu_boxcode = "4K0907557G__0003";
    g_wifi_up = false;          /* Wi-Fi never came up. */
    install_auth_token(TEST_AUTH_TOKEN);

    /* Cache query still works without sync. */
    char r[128];
    EXPECT(license_can_run_feature(FEATURE_WOT_LOGGING, g_ecu_vin, r, sizeof(r)),
           "cached license honored offline (no sync needed)");
    EXPECT(g_http.register_calls == 0 && g_http.license_calls == 0,
           "no HTTP calls when Wi-Fi down");
}

/* ------------------------------------------------------------------ */
/* Test 12 — license cache round-trip via NVS                            */
/* ------------------------------------------------------------------ */
static void test_cache_persists_across_reload(void) {
    test_setup();
    license_state_t seed = {
        .present = true, .paid = true, .revoked = true,
        .vin = "WAUZZZ4M9PA000060",
        .revoked_reason = "fraud",
        .last_sync_ms = 12345678,
    };
    license_test_seed(&seed);
    EXPECT(license_save_cache() == ESP_OK, "save_cache OK");

    /* Wipe in-memory state by re-init (NVS retains values). */
    install_license_module();   /* license_init also calls license_load_cache */

    const license_state_t *st = license_get_state();
    EXPECT(st->present, "present persisted");
    EXPECT(st->paid, "paid persisted");
    EXPECT(st->revoked, "revoked persisted");
    EXPECT(strcmp(st->vin, "WAUZZZ4M9PA000060") == 0, "VIN persisted");
    EXPECT(strcmp(st->revoked_reason, "fraud") == 0, "revoked_reason persisted");
    EXPECT(st->last_sync_ms == 12345678, "last_sync_ms persisted");
}

/* ------------------------------------------------------------------ */
/* Test 13 — feature_manager arbitration around vin_pairing              */
/* ------------------------------------------------------------------ */
typedef struct {
    int  start_count;
    int  stop_count;
    bool running;
} mock_feature_state_t;

static mock_feature_state_t g_mockf;

static esp_err_t mockf_start(void) { g_mockf.start_count++; g_mockf.running = true;  return ESP_OK; }
static esp_err_t mockf_stop(void)  { g_mockf.stop_count++;  g_mockf.running = false; return ESP_OK; }
static bool      mockf_is_running(void) { return g_mockf.running; }

/* Slot a mock feature into FEATURE_WOT_LOGGING (the host harness does
 * not compile wot_logger.c, so the slot is unregistered). */
static const feature_descriptor_t mock_wot_desc = {
    .id = FEATURE_WOT_LOGGING, .name = "mock_wot",
    .start = mockf_start, .stop = mockf_stop, .is_running = mockf_is_running,
};

static void test_arbitration_swap_into_vin_pairing(void) {
    test_setup();
    memset(&g_mockf, 0, sizeof(g_mockf));

    esp_err_t reg_rc = feature_manager_register(&mock_wot_desc);
    EXPECT(reg_rc == ESP_OK || reg_rc == ESP_ERR_INVALID_STATE,
           "mock WOT feature registered (or already)");

    /* Start the mock. */
    char fmerr[64] = {0};
    EXPECT(feature_manager_request_start(FEATURE_WOT_LOGGING, fmerr, sizeof(fmerr)) == ESP_OK,
           "mock WOT started");
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING, "mock WOT active");

    /* Now run vin_pairing, which should preempt + release. */
    g_ecu_vin = "WAUZZZ4M9PA000070";
    g_ecu_boxcode = "4K0907557G__0003";
    g_wifi_up = true;
    install_auth_token(TEST_AUTH_TOKEN);
    mock_register_set(200, "{\"ok\":true}");
    mock_license_set(200,
        "{\"paid\":true,\"vin\":\"WAUZZZ4M9PA000070\",\"revoked\":false}");

    char err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t rc = vin_pairing_run_now(err, sizeof(err));
    EXPECT(rc == ESP_OK, "vin_pairing succeeds through arbitration");
    EXPECT(g_mockf.stop_count >= 1, "mock WOT stop() called by arbitrator");
    EXPECT(feature_manager_active() == FEATURE_NONE,
           "no feature active after vin_pairing returns");
}

/* ------------------------------------------------------------------ */
/* Test 14 — auth_token Bearer header carries through                    */
/* ------------------------------------------------------------------ */
static void test_auth_token_bearer_passthrough(void) {
    test_setup();
    g_ecu_vin = "WAUZZZ4M9PA000080";
    g_ecu_boxcode = "4K0907557G__0003";
    g_wifi_up = true;
    install_auth_token("abcdef0123");
    mock_register_set(200, "{\"ok\":true}");
    mock_license_set(200,
        "{\"paid\":true,\"vin\":\"WAUZZZ4M9PA000080\",\"revoked\":false}");

    char err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    EXPECT(vin_pairing_run_now(err, sizeof(err)) == ESP_OK, "happy path OK");
    EXPECT(strcmp(g_http.last_register_bearer, "abcdef0123") == 0,
           "register Bearer = installed token");
    EXPECT(strcmp(g_http.last_license_bearer, "abcdef0123") == 0,
           "license Bearer = installed token");
}

/* ------------------------------------------------------------------ */
/* Test 15 — argument validation                                         */
/* ------------------------------------------------------------------ */
static void test_argument_validation(void) {
    test_setup();
    /* Both modules are initialized by test_setup; the init functions
     * have an "already initialized → ESP_OK" idempotent shortcut.
     * Deinit them first so the validation paths actually run. */
    license_deinit();
    vin_pairing_deinit();

    license_module_config_t bad = {0};
    EXPECT(license_init(&bad) == ESP_ERR_INVALID_ARG, "license_init NULL members rejected");

    vin_pairing_config_t cfg = {
        .vin_source     = test_vin_source,
        .boxcode_source = test_boxcode_source,
        .wifi_ready     = test_wifi_ready,
        .device_mac     = NULL,
    };
    EXPECT(vin_pairing_init(&cfg) == ESP_ERR_INVALID_ARG,
           "vin_pairing_init NULL device_mac rejected");

    /* Restore valid state so any post-test cleanup doesn't trip. */
    install_license_module();
    install_vin_pairing();
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */
int main(void) {
    fprintf(stdout, "=== vin_pairing host unit tests ===\n");

    if (feature_manager_init() != ESP_OK) {
        fprintf(stderr, "feature_manager_init failed\n");
        return 1;
    }

    test_vin_normalize();
    test_cold_pairing();
    test_warm_pairing_no_token();
    test_pair_happy_path();
    test_can_run_vin_match();
    test_can_run_vin_mismatch();
    test_can_run_revoked();
    test_register_409_skips_license();
    test_license_401_clears_cache();
    test_license_5xx_keeps_cache();
    test_wifi_unavailable_uses_cache();
    test_cache_persists_across_reload();
    test_arbitration_swap_into_vin_pairing();
    test_auth_token_bearer_passthrough();
    test_argument_validation();

    if (g_failures == 0) {
        fprintf(stdout, "\n=== OK: all vin_pairing unit tests passed ===\n");
        return 0;
    }
    fprintf(stderr, "\n=== FAIL: %d unit test assertions failed ===\n", g_failures);
    return 1;
}
