/*
 * test_sbf_orchestrator.c — host-runnable unit tests for the SBF
 * live-tune orchestrator.
 *
 * Required scenarios per Prompt 5 acceptance:
 *   1. test_start_refuses_when_unpaid     — license gate refuses
 *   2. test_start_paid_applies_in_budget  — apply elapsed_ms < 2000
 *   3. test_set_ethanol_triggers_reapply  — ethanol change re-applies
 *   4. test_set_stage_triggers_reapply    — stage match re-applies;
 *                                           stage mismatch refused
 *   5. test_malformed_sbf_returns_idle    — load failure → ERROR/IDLE
 *   6. test_swap_from_dtc                 — arbitration via fm
 *   7. test_apply_progress_events         — progress events emitted
 *   8. test_unload_drains_queue           — stop drains queue
 *
 * Mocking pattern: real feature_manager + real license module +
 * mock loader iface + mock ecu_write + mock vin/boxcode/clock +
 * captured WS event sink.
 *
 * Frozen scal_file / bdef_file / ecu_write .c files are NOT linked
 * into the host runner (per FROZEN_MODULES.md). The mock loader
 * iface produces synthetic flex maps; the mock write fn captures
 * each call without touching CAN.
 */

#include "license.h"
#include "license_config.h"
#include "feature_manager.h"
#include "sbf_loader.h"
#include "sbf_applier.h"
#include "sbf_orchestrator.h"
#include "sbf_variants.h"
#include "sbf_config.h"
#include "blend_engine.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tiny EXPECT framework                                                */
/* ------------------------------------------------------------------ */

static int g_failures = 0;
#define EXPECT(cond, msg) do {                                              \
    if (!(cond)) {                                                          \
        fprintf(stderr, "  FAIL  %s — %s (line %d)\n", __func__, (msg), __LINE__); \
        g_failures++;                                                       \
    } else {                                                                \
        fprintf(stdout, "  PASS  %s — %s\n", __func__, (msg));              \
    }                                                                       \
} while (0)

/* ------------------------------------------------------------------ */
/* Mock NVS for license module                                          */
/* ------------------------------------------------------------------ */

#define NVS_MAX 32
#define NVS_KEYMAX 32
#define NVS_VALMAX 128

typedef struct {
    bool used; bool is_u32; char k[NVS_KEYMAX];
    char s[NVS_VALMAX]; uint32_t u;
} nvs_entry_t;
static nvs_entry_t g_nvs[NVS_MAX];

static void nvs_reset(void) { memset(g_nvs, 0, sizeof(g_nvs)); }

static int nvs_find(const char *k) {
    for (int i = 0; i < NVS_MAX; i++) if (g_nvs[i].used && strcmp(g_nvs[i].k, k) == 0) return i;
    return -1;
}
static int nvs_alloc(const char *k) {
    int s = nvs_find(k);
    if (s >= 0) return s;
    for (int i = 0; i < NVS_MAX; i++) {
        if (!g_nvs[i].used) {
            g_nvs[i].used = true;
            strncpy(g_nvs[i].k, k, NVS_KEYMAX - 1);
            return i;
        }
    }
    return -1;
}
static esp_err_t mock_nvs_save_str(const char *k, const char *v, void *c) {
    (void)c;
    int s = nvs_alloc(k);
    if (s < 0) return ESP_ERR_NO_MEM;
    g_nvs[s].is_u32 = false;
    strncpy(g_nvs[s].s, v ? v : "", NVS_VALMAX - 1);
    return ESP_OK;
}
static esp_err_t mock_nvs_load_str(const char *k, char *o, size_t cap, void *c) {
    (void)c;
    int s = nvs_find(k);
    if (s < 0 || g_nvs[s].is_u32) { o[0] = '\0'; return ESP_ERR_NOT_FOUND; }
    strncpy(o, g_nvs[s].s, cap - 1); o[cap - 1] = '\0';
    return ESP_OK;
}
static esp_err_t mock_nvs_save_u32(const char *k, uint32_t v, void *c) {
    (void)c;
    int s = nvs_alloc(k); if (s < 0) return ESP_ERR_NO_MEM;
    g_nvs[s].is_u32 = true; g_nvs[s].u = v;
    return ESP_OK;
}
static esp_err_t mock_nvs_load_u32(const char *k, uint32_t *o, void *c) {
    (void)c;
    int s = nvs_find(k); if (s < 0 || !g_nvs[s].is_u32) return ESP_ERR_NOT_FOUND;
    *o = g_nvs[s].u; return ESP_OK;
}

/* HTTP mocks (license module requires them; tests don't use them). */
static int dummy_http_get(const char *u, const char *b, uint8_t *o, size_t c, size_t *l, uint32_t t, void *x) {
    (void)u;(void)b;(void)o;(void)c;(void)l;(void)t;(void)x; return -1;
}
static int dummy_http_post(const char *u, const char *b, const uint8_t *body, size_t bl, uint8_t *o, size_t c, size_t *l, uint32_t t, void *x) {
    (void)u;(void)b;(void)body;(void)bl;(void)o;(void)c;(void)l;(void)t;(void)x; return -1;
}

/* ------------------------------------------------------------------ */
/* Test-controllable clock                                              */
/* ------------------------------------------------------------------ */

static uint32_t g_now_ms = 0;
static uint32_t test_clock(void) { return g_now_ms; }
static void clock_advance(uint32_t d) { g_now_ms += d; }
static void clock_reset(void) { g_now_ms = 0; }

/* ------------------------------------------------------------------ */
/* Mock VIN / boxcode sources                                           */
/* ------------------------------------------------------------------ */

static const char *g_vin     = "WAUZZZ4M9PA000005";
static const char *g_boxcode = "4K0907557G__0003";
static const char *test_vin(void)     { return g_vin; }
static const char *test_boxcode(void) { return g_boxcode; }

/* ------------------------------------------------------------------ */
/* Mock loader iface                                                    */
/* ------------------------------------------------------------------ */

#define MOCK_MAP_COUNT_DEFAULT 12
#define MOCK_CELL_COUNT_PER_MAP 4

typedef struct {
    bool          loaded;
    bool          load_should_fail;
    uint32_t      map_count;
    /* Synthetic per-map data. */
    uint32_t      orig_addrs[64];
    uint8_t       gas_data[64][MOCK_CELL_COUNT_PER_MAP];
    uint8_t       eth_data[64][MOCK_CELL_COUNT_PER_MAP];
    uint16_t      blend_x[SBF_BLEND_MAP_POINTS];
    uint16_t      blend_z[SBF_BLEND_MAP_POINTS];
    uint8_t       last_ethanol_observed;
} mock_loaded_t;

static mock_loaded_t g_loaded;

/* sbf_loaded_t is forward-declared opaque; we typedef-it to our
 * mock by aliasing the struct in this TU only. */
struct sbf_loaded_s { mock_loaded_t *m; };

static esp_err_t mock_loader_load(const char *path, sbf_loaded_t **out, void *ctx) {
    (void)ctx;
    if (path == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (g_loaded.load_should_fail) return ESP_FAIL;
    static struct sbf_loaded_s s_handle;
    s_handle.m = &g_loaded;
    *out = &s_handle;
    g_loaded.loaded = true;
    return ESP_OK;
}
static void mock_loader_free(sbf_loaded_t *l, void *ctx) {
    (void)ctx;
    if (l == NULL || l->m == NULL) return;
    l->m->loaded = false;
}
static uint32_t mock_loader_map_count(sbf_loaded_t *l, void *ctx) {
    (void)ctx;
    return (l != NULL && l->m != NULL) ? l->m->map_count : (uint32_t)0;
}
static esp_err_t mock_loader_map_info(sbf_loaded_t *l, uint32_t idx,
                                      sbf_loader_map_info_t *out, void *ctx) {
    (void)ctx;
    if (l == NULL || out == NULL || idx >= l->m->map_count) return ESP_ERR_INVALID_ARG;
    out->original_address = l->m->orig_addrs[idx];
    out->x_dim            = (uint32_t)2;
    out->y_dim            = (uint32_t)2;
    out->dtype            = BLEND_DTYPE_U8;
    out->big_endian       = false;
    return ESP_OK;
}
static esp_err_t mock_loader_blend_axes(sbf_loaded_t *l, uint32_t idx,
                                        uint16_t *xo, uint16_t *zo,
                                        size_t pc, void *ctx) {
    (void)ctx; (void)idx;
    if (l == NULL || xo == NULL || zo == NULL) return ESP_ERR_INVALID_ARG;
    if (pc != (size_t)SBF_BLEND_MAP_POINTS) return ESP_ERR_INVALID_SIZE;
    for (size_t i = (size_t)0; i < pc; i++) {
        xo[i] = l->m->blend_x[i];
        zo[i] = l->m->blend_z[i];
    }
    return ESP_OK;
}
static esp_err_t mock_loader_map_buffers(sbf_loaded_t *l, uint32_t idx,
                                         const uint8_t **gas_out, const uint8_t **eth_out,
                                         size_t *bc_out, void *ctx) {
    (void)ctx;
    if (l == NULL || idx >= l->m->map_count) return ESP_ERR_INVALID_ARG;
    *gas_out = l->m->gas_data[idx];
    *eth_out = l->m->eth_data[idx];
    *bc_out  = (size_t)MOCK_CELL_COUNT_PER_MAP;
    return ESP_OK;
}
static sbf_loader_iface_t mock_loader_iface(void) {
    sbf_loader_iface_t i = {
        .load        = mock_loader_load,
        .free        = mock_loader_free,
        .map_count   = mock_loader_map_count,
        .map_info    = mock_loader_map_info,
        .blend_axes  = mock_loader_blend_axes,
        .map_buffers = mock_loader_map_buffers,
        .user_ctx    = NULL,
    };
    return i;
}

/* ------------------------------------------------------------------ */
/* Mock ECU write                                                       */
/* ------------------------------------------------------------------ */

static int g_write_calls = 0;
static uint32_t g_write_advance_ms = 50; /* simulate per-write time */

static esp_err_t mock_ecu_write(uint32_t addr, const uint8_t *bytes, size_t n,
                                uint8_t mid, uint32_t off, uint32_t to_ms, void *ctx) {
    (void)addr; (void)bytes; (void)n; (void)mid; (void)off; (void)to_ms; (void)ctx;
    g_write_calls++;
    clock_advance(g_write_advance_ms);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* WS event sink (capture)                                              */
/* ------------------------------------------------------------------ */

#define EVT_LOG_MAX 32
static char g_events[EVT_LOG_MAX][SBF_EVENT_JSON_MAX];
static int g_event_count = 0;

static void evt_reset(void) { g_event_count = 0; memset(g_events, 0, sizeof(g_events)); }
static void mock_event_sink(const char *json, void *ctx) {
    (void)ctx;
    if (g_event_count < EVT_LOG_MAX && json != NULL) {
        strncpy(g_events[g_event_count], json, SBF_EVENT_JSON_MAX - 1);
        g_event_count++;
    }
}
static int evt_count_with(const char *needle) {
    int n = 0;
    for (int i = 0; i < g_event_count; i++) if (strstr(g_events[i], needle) != NULL) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Per-test setup                                                      */
/* ------------------------------------------------------------------ */

static void install_license_mock_state(bool paid, bool revoked, const char *vin) {
    license_state_t seed = { .present = true, .paid = paid, .revoked = revoked };
    if (vin != NULL) {
        strncpy(seed.vin, vin, sizeof(seed.vin) - 1);
        seed.vin[sizeof(seed.vin) - 1] = '\0';
    }
    license_test_seed(&seed);
}

static void install_synthetic_sbf(uint32_t map_count) {
    memset(&g_loaded, 0, sizeof(g_loaded));
    if (map_count > (uint32_t)64) map_count = (uint32_t)64;
    g_loaded.map_count = map_count;
    /* Linear x-axis 0..FULL_SCALE; linear z 0..FULL_SCALE. */
    for (size_t i = (size_t)0; i < (size_t)SBF_BLEND_MAP_POINTS; i++) {
        g_loaded.blend_x[i] = (uint16_t)((uint32_t)i * (uint32_t)SBF_BLEND_FACTOR_FULL_SCALE /
                                          (uint32_t)(SBF_BLEND_MAP_POINTS - 1));
        g_loaded.blend_z[i] = (uint16_t)((uint32_t)i * (uint32_t)SBF_BLEND_FACTOR_FULL_SCALE /
                                          (uint32_t)(SBF_BLEND_MAP_POINTS - 1));
    }
    for (uint32_t m = (uint32_t)0; m < map_count; m++) {
        g_loaded.orig_addrs[m] = (uint32_t)0x100000 + m * (uint32_t)0x10;
        for (int c = 0; c < MOCK_CELL_COUNT_PER_MAP; c++) {
            g_loaded.gas_data[m][c] = (uint8_t)(c * 10);
            g_loaded.eth_data[m][c] = (uint8_t)(c * 10 + 50);
        }
    }
}

static void install_orchestrator(void) {
    sbf_orchestrator_deinit();
    sbf_orchestrator_config_t cfg = {
        .loader         = mock_loader_iface(),
        .ecu_write      = mock_ecu_write,
        .ecu_write_ctx  = NULL,
        .event_sink     = mock_event_sink,
        .event_sink_ctx = NULL,
        .vin_source     = test_vin,
        .boxcode_source = test_boxcode,
        .clock_now_ms   = test_clock,
    };
    EXPECT(sbf_orchestrator_init(&cfg) == ESP_OK, "sbf_orchestrator_init");
}

static void install_license(void) {
    license_module_config_t lic = {
        .http = { .get = dummy_http_get, .post = dummy_http_post, .user_ctx = NULL },
        .nvs  = {
            .save_string = mock_nvs_save_str,
            .load_string = mock_nvs_load_str,
            .save_uint32 = mock_nvs_save_u32,
            .load_uint32 = mock_nvs_load_u32,
            .user_ctx = NULL,
        },
    };
    license_deinit();
    EXPECT(license_init(&lic) == ESP_OK, "license_init");
}

static void install_variants(void) {
    static const sbf_variant_entry_t entries[] = {
        { .boxcode = "4K0907557G__0003", .mid_byte = 0x80, .address_offset = 0x80000000 },
    };
    sbf_variants_test_override(entries, (size_t)1);
}

static void test_setup(void) {
    /* Drain any active feature. */
    feature_id_t a = feature_manager_active();
    if (a != FEATURE_NONE) feature_manager_request_stop(a);

    nvs_reset();
    clock_reset();
    evt_reset();
    g_write_calls = 0;
    g_loaded.load_should_fail = false;

    install_license();
    install_orchestrator();
    install_variants();
    install_synthetic_sbf((uint32_t)MOCK_MAP_COUNT_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* Test 1 — start refuses when unpaid                                   */
/* ------------------------------------------------------------------ */
static void test_start_refuses_when_unpaid(void) {
    test_setup();
    install_license_mock_state(false, false, "WAUZZZ4M9PA000005");

    char err[SBF_ERR_BUF_MAX] = {0};
    esp_err_t rc = sbf_orchestrator_live_tune_start((uint8_t)1, (uint8_t)0, err, sizeof(err));
    EXPECT(rc == ESP_ERR_INVALID_STATE, "start refused when license unpaid");
    EXPECT(err[0] != '\0', "err_out populated");
    EXPECT(g_write_calls == 0, "no ECU writes attempted");
}

/* ------------------------------------------------------------------ */
/* Test 2 — start paid → applies in budget                              */
/* ------------------------------------------------------------------ */
static void test_start_paid_applies_in_budget(void) {
    test_setup();
    install_license_mock_state(true, false, "WAUZZZ4M9PA000005");

    char err[SBF_ERR_BUF_MAX] = {0};
    esp_err_t rc = sbf_orchestrator_live_tune_start((uint8_t)1, (uint8_t)50, err, sizeof(err));
    EXPECT(rc == ESP_OK, "start enqueues OK with paid license");

    /* Drain the worker queue. */
    rc = sbf_orchestrator_drain_one();
    EXPECT(rc == ESP_OK, "drain_one OK");

    sbf_status_snapshot_t st;
    sbf_orchestrator_live_tune_status(&st);
    EXPECT(st.state == SBF_STATE_ACTIVE, "state ACTIVE after apply");
    EXPECT(st.last_apply_elapsed_ms < (uint32_t)SBF_APPLY_HARD_CAP_MS,
           "elapsed_ms < SBF_APPLY_HARD_CAP_MS budget");
    EXPECT(g_write_calls == (int)MOCK_MAP_COUNT_DEFAULT,
           "ecu_write called once per map");

    feature_manager_request_stop(FEATURE_LIVE_TUNE);
}

/* ------------------------------------------------------------------ */
/* Test 3 — set ethanol triggers re-apply                               */
/* ------------------------------------------------------------------ */
static void test_set_ethanol_triggers_reapply(void) {
    test_setup();
    install_license_mock_state(true, false, "WAUZZZ4M9PA000005");

    char err[SBF_ERR_BUF_MAX] = {0};
    EXPECT(sbf_orchestrator_live_tune_start((uint8_t)1, (uint8_t)0, err, sizeof(err)) == ESP_OK, "start");
    EXPECT(sbf_orchestrator_drain_one() == ESP_OK, "drain initial apply");
    int writes_after_initial = g_write_calls;

    /* Same stage, different ethanol. */
    EXPECT(sbf_orchestrator_live_tune_set((uint8_t)1, (uint8_t)50, err, sizeof(err)) == ESP_OK,
           "set ethanol enqueued");
    EXPECT(sbf_orchestrator_queue_depth() == (size_t)1, "queue has one re-apply pending");
    EXPECT(sbf_orchestrator_drain_one() == ESP_OK, "drain re-apply");
    EXPECT(g_write_calls > writes_after_initial, "more writes after re-apply");

    sbf_status_snapshot_t st;
    sbf_orchestrator_live_tune_status(&st);
    EXPECT(st.current_ethanol_pct == (uint8_t)50, "current_ethanol_pct reflects new value");

    feature_manager_request_stop(FEATURE_LIVE_TUNE);
}

/* ------------------------------------------------------------------ */
/* Test 4 — set stage triggers re-apply (matched stage) +
 *           refuses on stage mismatch                                  */
/* ------------------------------------------------------------------ */
static void test_set_stage_triggers_reapply(void) {
    test_setup();
    install_license_mock_state(true, false, "WAUZZZ4M9PA000005");

    char err[SBF_ERR_BUF_MAX] = {0};
    EXPECT(sbf_orchestrator_live_tune_start((uint8_t)1, (uint8_t)0, err, sizeof(err)) == ESP_OK, "start");
    EXPECT(sbf_orchestrator_drain_one() == ESP_OK, "drain initial");

    /* Same stage = re-apply allowed. */
    EXPECT(sbf_orchestrator_live_tune_set((uint8_t)1, (uint8_t)25, err, sizeof(err)) == ESP_OK,
           "matching-stage set OK");
    EXPECT(sbf_orchestrator_drain_one() == ESP_OK, "drain re-apply");

    /* Different stage = refused per Q-B. */
    err[0] = '\0';
    esp_err_t rc = sbf_orchestrator_live_tune_set((uint8_t)2, (uint8_t)0, err, sizeof(err));
    EXPECT(rc == ESP_ERR_NOT_SUPPORTED, "stage mismatch returns NOT_SUPPORTED");
    EXPECT(strstr(err, "stage") != NULL || strstr(err, "live_tune_start") != NULL,
           "err mentions stage / re-fetch");

    feature_manager_request_stop(FEATURE_LIVE_TUNE);
}

/* ------------------------------------------------------------------ */
/* Test 5 — malformed SBF returns to ERROR/IDLE                          */
/* ------------------------------------------------------------------ */
static void test_malformed_sbf_returns_idle(void) {
    test_setup();
    install_license_mock_state(true, false, "WAUZZZ4M9PA000005");

    g_loaded.load_should_fail = true;

    char err[SBF_ERR_BUF_MAX] = {0};
    EXPECT(sbf_orchestrator_live_tune_start((uint8_t)1, (uint8_t)0, err, sizeof(err)) == ESP_OK,
           "start enqueues even when load will fail");
    esp_err_t rc = sbf_orchestrator_drain_one();
    EXPECT(rc != ESP_OK, "drain returns error when malformed SBF");

    sbf_status_snapshot_t st;
    sbf_orchestrator_live_tune_status(&st);
    EXPECT(st.state == SBF_STATE_ERROR, "state == ERROR after malformed-SBF load");
    EXPECT(strstr(st.last_error, "load") != NULL || strstr(st.last_error, "rc=") != NULL,
           "last_error mentions load failure");

    /* Stop returns to IDLE. */
    EXPECT(sbf_orchestrator_live_tune_stop(NULL, 0) == ESP_OK, "stop OK");
    sbf_orchestrator_live_tune_status(&st);
    EXPECT(st.state == SBF_STATE_IDLE, "state IDLE after stop");
}

/* ------------------------------------------------------------------ */
/* Test 6 — swap from DTC via feature_manager                           */
/* ------------------------------------------------------------------ */
static int g_dtc_start_calls = 0;
static int g_dtc_stop_calls  = 0;
static bool g_dtc_running = false;
static esp_err_t mock_dtc_start(void) { g_dtc_start_calls++; g_dtc_running = true;  return ESP_OK; }
static esp_err_t mock_dtc_stop(void)  { g_dtc_stop_calls++;  g_dtc_running = false; return ESP_OK; }
static bool      mock_dtc_is_running(void) { return g_dtc_running; }

static const feature_descriptor_t k_mock_dtc_desc = {
    .id = FEATURE_DTC, .name = "mock_dtc",
    .start = mock_dtc_start, .stop = mock_dtc_stop, .is_running = mock_dtc_is_running,
};

static void test_swap_from_dtc(void) {
    test_setup();
    install_license_mock_state(true, false, "WAUZZZ4M9PA000005");
    g_dtc_start_calls = 0; g_dtc_stop_calls = 0; g_dtc_running = false;

    esp_err_t reg = feature_manager_register(&k_mock_dtc_desc);
    EXPECT(reg == ESP_OK || reg == ESP_ERR_INVALID_STATE, "DTC mock registered");

    char err[SBF_ERR_BUF_MAX] = {0};
    EXPECT(feature_manager_request_start(FEATURE_DTC, err, sizeof(err)) == ESP_OK, "DTC starts");
    EXPECT(feature_manager_active() == FEATURE_DTC, "DTC active");

    /* Now ask for live_tune_start — fm must arbitrate (stop DTC, start LIVE_TUNE). */
    err[0] = '\0';
    esp_err_t rc = sbf_orchestrator_live_tune_start((uint8_t)1, (uint8_t)0, err, sizeof(err));
    EXPECT(rc == ESP_OK, "live_tune_start succeeds via arbitration");
    EXPECT(g_dtc_stop_calls >= 1, "DTC stop() called by arbitrator");
    EXPECT(feature_manager_active() == FEATURE_LIVE_TUNE, "LIVE_TUNE active");

    EXPECT(sbf_orchestrator_drain_one() == ESP_OK, "drain apply");
    feature_manager_request_stop(FEATURE_LIVE_TUNE);
}

/* ------------------------------------------------------------------ */
/* Test 7 — apply progress events                                       */
/* ------------------------------------------------------------------ */
static void test_apply_progress_events(void) {
    test_setup();
    install_license_mock_state(true, false, "WAUZZZ4M9PA000005");

    /* Use a larger map count so SBF_PROGRESS_EVENT_EVERY_N_MAPS (10)
     * fires at least once. */
    install_synthetic_sbf((uint32_t)25);
    evt_reset();

    char err[SBF_ERR_BUF_MAX] = {0};
    EXPECT(sbf_orchestrator_live_tune_start((uint8_t)1, (uint8_t)0, err, sizeof(err)) == ESP_OK, "start");
    EXPECT(sbf_orchestrator_drain_one() == ESP_OK, "drain apply");

    EXPECT(evt_count_with("apply_started")   >= 1, "apply_started event emitted");
    EXPECT(evt_count_with("apply_progress")  >= 1, "apply_progress event emitted");
    EXPECT(evt_count_with("apply_completed") >= 1, "apply_completed event emitted");
    EXPECT(evt_count_with("elapsed_ms")      >= 1, "elapsed_ms appears in events");

    feature_manager_request_stop(FEATURE_LIVE_TUNE);
}

/* ------------------------------------------------------------------ */
/* Test 8 — unload drains queue                                         */
/* ------------------------------------------------------------------ */
static void test_unload_drains_queue(void) {
    test_setup();
    install_license_mock_state(true, false, "WAUZZZ4M9PA000005");

    char err[SBF_ERR_BUF_MAX] = {0};
    EXPECT(sbf_orchestrator_live_tune_start((uint8_t)1, (uint8_t)0, err, sizeof(err)) == ESP_OK, "start");
    EXPECT(sbf_orchestrator_drain_one() == ESP_OK, "drain initial apply");

    /* Queue several re-applies WITHOUT draining. The latest-wins
     * collapse means the queue stays at depth 1 even after multiple
     * sets. */
    EXPECT(sbf_orchestrator_live_tune_set((uint8_t)1, (uint8_t)10, err, sizeof(err)) == ESP_OK, "set 1");
    EXPECT(sbf_orchestrator_live_tune_set((uint8_t)1, (uint8_t)25, err, sizeof(err)) == ESP_OK, "set 2");
    EXPECT(sbf_orchestrator_live_tune_set((uint8_t)1, (uint8_t)40, err, sizeof(err)) == ESP_OK, "set 3");
    EXPECT(sbf_orchestrator_queue_depth() >= (size_t)1, "queue has at least one pending");

    int writes_before_stop = g_write_calls;
    EXPECT(sbf_orchestrator_live_tune_stop(NULL, 0) == ESP_OK, "stop");
    EXPECT(sbf_orchestrator_queue_depth() == (size_t)0, "queue drained on stop");

    /* Calling drain_one after stop should be a no-op (queue empty). */
    EXPECT(sbf_orchestrator_drain_one() == ESP_OK, "drain_one no-op after stop");
    EXPECT(g_write_calls == writes_before_stop, "no further writes after stop");

    sbf_status_snapshot_t st;
    sbf_orchestrator_live_tune_status(&st);
    EXPECT(st.state == SBF_STATE_IDLE, "IDLE after stop");
}

/* ------------------------------------------------------------------ */
/* Bonus: blend_engine sanity                                           */
/* ------------------------------------------------------------------ */
static void test_blend_engine_basics(void) {
    EXPECT(blend_dtype_size(BLEND_DTYPE_U8)  == (size_t)1, "u8 size");
    EXPECT(blend_dtype_size(BLEND_DTYPE_U16) == (size_t)2, "u16 size");
    EXPECT(blend_dtype_size(BLEND_DTYPE_U32) == (size_t)4, "u32 size");

    /* 50% ethanol on a 0..FULL_SCALE linear blend factor map should
     * produce roughly FULL_SCALE/2. */
    uint16_t x[SBF_BLEND_MAP_POINTS];
    uint16_t z[SBF_BLEND_MAP_POINTS];
    for (size_t i = (size_t)0; i < (size_t)SBF_BLEND_MAP_POINTS; i++) {
        x[i] = (uint16_t)((uint32_t)i * (uint32_t)SBF_BLEND_FACTOR_FULL_SCALE / (uint32_t)(SBF_BLEND_MAP_POINTS - 1));
        z[i] = x[i];
    }
    uint16_t f = blend_engine_compute_factor(x, z, (size_t)SBF_BLEND_MAP_POINTS, (uint8_t)50);
    uint32_t expected_half = (uint32_t)SBF_BLEND_FACTOR_FULL_SCALE / (uint32_t)2;
    EXPECT((uint32_t)f >= expected_half - (uint32_t)2000 &&
           (uint32_t)f <= expected_half + (uint32_t)2000,
           "compute_factor at 50% ≈ FULL_SCALE/2");

    /* Per-cell interpolation of u8 with 50% factor: gas=0, eth=100 → ~50. */
    uint8_t gas = 0, eth = 100, out = 0xFF;
    EXPECT(blend_engine_interpolate_cell(&gas, &eth, &out, BLEND_DTYPE_U8, false,
                                         (uint16_t)(SBF_BLEND_FACTOR_FULL_SCALE / 2)) == ESP_OK,
           "interpolate_cell u8 OK");
    EXPECT(out >= (uint8_t)45 && out <= (uint8_t)55, "u8 interp at 50% ≈ 50");
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(void) {
    fprintf(stdout, "=== sbf_orchestrator host unit tests ===\n");

    if (feature_manager_init() != ESP_OK) {
        fprintf(stderr, "feature_manager_init failed\n");
        return 1;
    }

    test_blend_engine_basics();
    test_start_refuses_when_unpaid();
    test_start_paid_applies_in_budget();
    test_set_ethanol_triggers_reapply();
    test_set_stage_triggers_reapply();
    test_malformed_sbf_returns_idle();
    test_swap_from_dtc();
    test_apply_progress_events();
    test_unload_drains_queue();

    if (g_failures == 0) {
        fprintf(stdout, "\n=== OK: all sbf_orchestrator unit tests passed ===\n");
        return 0;
    }
    fprintf(stderr, "\n=== FAIL: %d unit test assertions failed ===\n", g_failures);
    return 1;
}
