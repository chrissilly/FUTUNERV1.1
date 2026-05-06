/*
 * test_wot_logger.c — host-runnable unit tests for the WOT logger
 * pipeline (lifecycle layer + recorder + uploader).
 *
 * Built into firmware/test/wot_logger/host_test_runner via the
 * Makefile in that directory and exercised by
 * firmware/test/wot_logger/eval.sh.
 *
 * Required scenarios (per CLAUDE_CODE_KICKOFF.md Prompt 2):
 *   1. start when no other feature active → WOT logger active
 *   2. start when another feature active → arbitrated correctly via
 *      feature_manager (mock feature)
 *   3. trigger crosses threshold → recording begins
 *   4. 60s elapsed → recording auto-ends, log queued (hard cap)
 *   5. upload success → local copy deleted (delete on 200)
 *   6. upload 5xx → local copy retained, retry on next attempt
 *   + structural gzip header / footer check (0x1F 0x8B 0x08)
 *   + test-controllable clock (fast-forward instead of wall-clock 60s)
 *   + arbitration: WOT swap from a mock feature
 */

#include "wot_recorder.h"
#include "wot_uploader.h"
#include "wot_logger.h"
#include "wot_logger_config.h"
#include "feature_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tiny EXPECT framework (same shape as test_feature_manager.c)        */
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
/* Test-controllable clock                                             */
/* ------------------------------------------------------------------ */

static uint32_t g_now_ms = 0;

static uint32_t test_clock_now_ms(void) { return g_now_ms; }
static void     test_clock_fast_forward(uint32_t delta_ms) { g_now_ms += delta_ms; }
static void     test_clock_reset(void) { g_now_ms = 0; }

/* ------------------------------------------------------------------ */
/* Mock filesystem (in-memory)                                         */
/* ------------------------------------------------------------------ */

#define MOCK_FS_MAX_FILES 16
#define MOCK_FS_MAX_PATH  96
#define MOCK_FS_MAX_BYTES 65536

typedef struct {
    bool    in_use;
    char    path[MOCK_FS_MAX_PATH];
    uint8_t data[MOCK_FS_MAX_BYTES];
    size_t  len;
    uint32_t insertion_order; /* lower == older */
} mock_file_t;

typedef struct {
    mock_file_t files[MOCK_FS_MAX_FILES];
    uint32_t    next_order;
    int         iter_cursor;
} mock_fs_t;

static mock_fs_t g_fs;

static void mock_fs_reset(void) {
    memset(&g_fs, 0, sizeof(g_fs));
}

static int mock_fs_find(const char *path) {
    for (int i = 0; i < MOCK_FS_MAX_FILES; i++) {
        if (g_fs.files[i].in_use && strcmp(g_fs.files[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static esp_err_t mock_fs_write(const char *path, const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    if (len > MOCK_FS_MAX_BYTES) return ESP_ERR_NO_MEM;
    int slot = mock_fs_find(path);
    if (slot < 0) {
        for (int i = 0; i < MOCK_FS_MAX_FILES; i++) {
            if (!g_fs.files[i].in_use) { slot = i; break; }
        }
    }
    if (slot < 0) return ESP_ERR_NO_MEM;
    g_fs.files[slot].in_use = true;
    strncpy(g_fs.files[slot].path, path, MOCK_FS_MAX_PATH - 1);
    g_fs.files[slot].path[MOCK_FS_MAX_PATH - 1] = '\0';
    memcpy(g_fs.files[slot].data, data, len);
    g_fs.files[slot].len = len;
    g_fs.files[slot].insertion_order = ++g_fs.next_order;
    return ESP_OK;
}

static esp_err_t mock_fs_read(const char *path, uint8_t *out, size_t *in_out_len, void *ctx) {
    (void)ctx;
    int slot = mock_fs_find(path);
    if (slot < 0) return ESP_ERR_NOT_FOUND;
    if (g_fs.files[slot].len > *in_out_len) return ESP_ERR_NO_MEM;
    memcpy(out, g_fs.files[slot].data, g_fs.files[slot].len);
    *in_out_len = g_fs.files[slot].len;
    return ESP_OK;
}

static esp_err_t mock_fs_delete(const char *path, void *ctx) {
    (void)ctx;
    int slot = mock_fs_find(path);
    if (slot >= 0) {
        g_fs.files[slot].in_use = false;
    }
    return ESP_OK;
}

/* iter_next: returns oldest file first (FIFO ordering by
 * insertion_order). reset=true rewinds; subsequent calls iterate
 * by re-scanning (cheap for the test-sized FS). */
static esp_err_t mock_fs_iter(bool reset, char *out, size_t out_cap, size_t *size, void *ctx) {
    (void)ctx;
    if (reset) g_fs.iter_cursor = 0;
    int target_idx = -1;
    uint32_t target_order = (uint32_t)0xFFFFFFFF;
    int seen = 0;
    for (int i = 0; i < MOCK_FS_MAX_FILES; i++) {
        if (!g_fs.files[i].in_use) continue;
        if (g_fs.files[i].insertion_order < target_order) {
            target_order = g_fs.files[i].insertion_order;
            target_idx = i;
        }
        seen++;
    }
    /* Skip files we already returned. We track this by zero-ing the
     * insertion_order temporarily, but to keep iteration idempotent
     * across reset=false calls the iter just walks the next-oldest
     * each call — and the test invokes reset=true on each cycle. */
    if (target_idx < 0) return ESP_ERR_NOT_FOUND;
    /* Strip the queue-dir prefix from the stored path so the
     * "filename only" contract is honored. */
    const char *full = g_fs.files[target_idx].path;
    const char *slash = strrchr(full, '/');
    const char *name = slash != NULL ? slash + 1 : full;
    strncpy(out, name, out_cap - 1);
    out[out_cap - 1] = '\0';
    *size = g_fs.files[target_idx].len;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Mock HTTP client                                                    */
/* ------------------------------------------------------------------ */

static int  g_http_next_status = 200;
static int  g_http_call_count  = 0;
static char g_http_last_url[256];
static size_t g_http_last_body_len = 0;
static uint8_t g_http_last_body_first_bytes[16];

static int mock_http_post(const char *url, const uint8_t *body, size_t body_len,
                          uint32_t timeout_ms, void *ctx) {
    (void)timeout_ms;
    (void)ctx;
    g_http_call_count++;
    strncpy(g_http_last_url, url, sizeof(g_http_last_url) - 1);
    g_http_last_url[sizeof(g_http_last_url) - 1] = '\0';
    g_http_last_body_len = body_len;
    size_t copy = body_len < sizeof(g_http_last_body_first_bytes)
                ? body_len : sizeof(g_http_last_body_first_bytes);
    memcpy(g_http_last_body_first_bytes, body, copy);
    return g_http_next_status;
}

/* ------------------------------------------------------------------ */
/* Mock wifi-ready                                                     */
/* ------------------------------------------------------------------ */

static bool g_wifi_ready = true;
static bool mock_wifi_ready(void) { return g_wifi_ready; }

/* ------------------------------------------------------------------ */
/* Mock alternate feature (for arbitration test)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int  start_count;
    int  stop_count;
    bool running;
} mock_feature_state_t;

static mock_feature_state_t g_mockf;

static esp_err_t mockf_start(void)        { g_mockf.start_count++; g_mockf.running = true;  return ESP_OK; }
static esp_err_t mockf_stop(void)         { g_mockf.stop_count++;  g_mockf.running = false; return ESP_OK; }
static bool      mockf_is_running(void)   { return g_mockf.running; }

static const feature_descriptor_t mock_feature_desc = {
    .id         = FEATURE_DTC,  /* any non-WOT slot */
    .name       = "mock_feature",
    .start      = mockf_start,
    .stop       = mockf_stop,
    .is_running = mockf_is_running,
};

/* ------------------------------------------------------------------ */
/* Recorder on-finish capture: pipes recordings into the uploader      */
/* (which uses the mock FS) and also keeps the most-recent gzip bytes  */
/* available for the structural check.                                 */
/* ------------------------------------------------------------------ */

static uint8_t g_last_gzip[262144];
static size_t  g_last_gzip_len = 0;

static void capture_on_finish(const uint8_t *gzip_buf, size_t gzip_len, void *ctx) {
    (void)ctx;
    if (gzip_len <= sizeof(g_last_gzip)) {
        memcpy(g_last_gzip, gzip_buf, gzip_len);
        g_last_gzip_len = gzip_len;
    }
    wot_uploader_enqueue(gzip_buf, gzip_len, test_clock_now_ms());
}

/* ------------------------------------------------------------------ */
/* Test fixtures (shared across tests)                                 */
/* ------------------------------------------------------------------ */

#define TEST_NUM_VARS 3
static const char *g_var_name_storage[TEST_NUM_VARS] = {
    WOT_TRIGGER_VARIABLE_NAME,  /* index 0 == throttle */
    "rpm",
    "lambda",
};
static const char * const *g_var_names = (const char * const *)g_var_name_storage;

static void setup_recorder(uint8_t trigger_index) {
    wot_recorder_deinit();
    wot_recorder_config_t cfg = {
        .clock_now_ms         = test_clock_now_ms,
        .on_finish            = capture_on_finish,
        .user_ctx             = NULL,
        .trigger_var_index    = trigger_index,
        .variables_per_sample = TEST_NUM_VARS,
        .variable_names       = g_var_names,
    };
    EXPECT(wot_recorder_init(&cfg) == ESP_OK, "recorder init");
}

static void setup_uploader(void) {
    wot_uploader_deinit();
    wot_uploader_config_t cfg = {
        .http       = { .post = mock_http_post, .user_ctx = NULL },
        .fs         = {
            .write_file  = mock_fs_write,
            .read_file   = mock_fs_read,
            .delete_file = mock_fs_delete,
            .iter_next   = mock_fs_iter,
            .user_ctx    = NULL,
        },
        .wifi_ready = mock_wifi_ready,
        .upload_url = WOT_UPLOAD_DEFAULT_HOST WOT_UPLOAD_ENDPOINT_PATH,
    };
    EXPECT(wot_uploader_init(&cfg) == ESP_OK, "uploader init");
}

static void test_setup(void) {
    test_clock_reset();
    mock_fs_reset();
    g_http_next_status = 200;
    g_http_call_count = 0;
    g_http_last_url[0] = '\0';
    g_http_last_body_len = 0;
    memset(g_http_last_body_first_bytes, 0, sizeof(g_http_last_body_first_bytes));
    g_wifi_ready = true;
    memset(&g_mockf, 0, sizeof(g_mockf));
    g_last_gzip_len = 0;
}

/* ------------------------------------------------------------------ */
/* Test 1 — start with no other feature active                          */
/* ------------------------------------------------------------------ */
static void test_start_no_other_feature(void) {
    test_setup();
    setup_recorder((uint8_t)0);
    setup_uploader();

    EXPECT(wot_logger_register_with_feature_manager() == ESP_OK,
           "register WOT descriptor");

    char err[64] = {0};
    esp_err_t rc = feature_manager_request_start(FEATURE_WOT_LOGGING, err, sizeof(err));
    EXPECT(rc == ESP_OK, "start succeeds when no other feature is active");
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING, "active is WOT logger");
    EXPECT(wot_logger_is_running(), "wot_logger_is_running() true after start");
    EXPECT(wot_recorder_is_armed(), "recorder armed after start");
    EXPECT(wot_uploader_is_running(), "uploader running after start");

    feature_manager_request_stop(FEATURE_WOT_LOGGING);
    EXPECT(!wot_logger_is_running(), "wot_logger_is_running() false after stop");
}

/* ------------------------------------------------------------------ */
/* Test 2 — arbitration: start WOT while a mock feature is active       */
/* ------------------------------------------------------------------ */
static void test_arbitration_swap_into_wot(void) {
    test_setup();
    setup_recorder((uint8_t)0);
    setup_uploader();

    /* Mock feature is in some other slot; it has been registered
     * (idempotent for the test harness on subsequent runs). */
    esp_err_t reg_rc = feature_manager_register(&mock_feature_desc);
    EXPECT(reg_rc == ESP_OK || reg_rc == ESP_ERR_INVALID_STATE,
           "mock feature registered (or already)");

    char err[64] = {0};
    EXPECT(feature_manager_request_start(FEATURE_DTC, err, sizeof(err)) == ESP_OK,
           "start mock feature");
    EXPECT(feature_manager_active() == FEATURE_DTC, "mock feature active");

    /* Now start WOT — feature_manager should arbitrate by stopping
     * the mock feature first, then starting WOT. */
    esp_err_t rc = feature_manager_request_start(FEATURE_WOT_LOGGING, err, sizeof(err));
    EXPECT(rc == ESP_OK, "swap into WOT logging via arbitration succeeds");
    EXPECT(g_mockf.stop_count >= 1, "mock feature stop() was called by arbitrator");
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING, "WOT is now active");
    EXPECT(wot_logger_is_running(), "WOT logger reports running");

    feature_manager_request_stop(FEATURE_WOT_LOGGING);
}

/* ------------------------------------------------------------------ */
/* Test 3 — throttle threshold crossing starts a recording              */
/* ------------------------------------------------------------------ */
static void test_threshold_starts_recording(void) {
    test_setup();
    setup_recorder((uint8_t)0);
    setup_uploader();
    EXPECT(wot_recorder_arm() == ESP_OK, "arm recorder");

    float below[TEST_NUM_VARS] = { 50.0f, 3000.0f, 1.0f };
    EXPECT(wot_recorder_feed_sample(below) == ESP_OK, "feed below-threshold sample");
    EXPECT(!wot_recorder_is_recording(), "recorder NOT recording on below-threshold");

    /* Step past the threshold; recording must begin. */
    float above[TEST_NUM_VARS] = { (float)(WOT_TRIGGER_THRESHOLD_PERCENT + 5), 6000.0f, 0.85f };
    test_clock_fast_forward((uint32_t)100);
    EXPECT(wot_recorder_feed_sample(above) == ESP_OK, "feed above-threshold sample");
    EXPECT(wot_recorder_is_recording(), "recorder IS recording after threshold crossed");
}

/* ------------------------------------------------------------------ */
/* Test 4 — 60s hard cap auto-ends recording, log queued                */
/* ------------------------------------------------------------------ */
static void test_hard_cap_ends_recording(void) {
    test_setup();
    setup_recorder((uint8_t)0);
    setup_uploader();
    EXPECT(wot_recorder_arm() == ESP_OK, "arm recorder");

    /* Cross threshold to begin recording. */
    float above[TEST_NUM_VARS] = { (float)(WOT_TRIGGER_THRESHOLD_PERCENT + 10), 6500.0f, 0.85f };
    EXPECT(wot_recorder_feed_sample(above) == ESP_OK, "trigger sample");
    EXPECT(wot_recorder_is_recording(), "recording started");

    /* Fast-forward past the max-record duration cap and feed one
     * more sample; the recorder must auto-end at this point. */
    test_clock_fast_forward((uint32_t)WOT_MAX_RECORD_DURATION_MS + (uint32_t)10);
    EXPECT(wot_recorder_feed_sample(above) == ESP_OK, "post-cap sample");
    EXPECT(!wot_recorder_is_recording(), "recorder ended at hard cap (60s elapsed)");
    EXPECT(g_last_gzip_len > (size_t)0, "on-finish callback fired with gzipped buffer");
    EXPECT(wot_uploader_queue_count() >= (uint32_t)1, "log was queued by uploader");

    /* Structural gzip check: 0x1F 0x8B 0x08 magic + footer math. */
    EXPECT(g_last_gzip[0] == (uint8_t)0x1F, "gzip magic byte 0 == 0x1F");
    EXPECT(g_last_gzip[1] == (uint8_t)0x8B, "gzip magic byte 1 == 0x8B");
    EXPECT(g_last_gzip[2] == (uint8_t)0x08, "gzip method byte == 0x08 (DEFLATE)");
    EXPECT(g_last_gzip_len >= (size_t)18,   "gzip stream long enough for header + footer");
}

/* ------------------------------------------------------------------ */
/* Test 5 — upload success → file deleted from queue                    */
/* ------------------------------------------------------------------ */
static void test_upload_success_deletes(void) {
    test_setup();
    setup_recorder((uint8_t)0);
    setup_uploader();
    EXPECT(wot_uploader_start() == ESP_OK, "uploader start");

    /* Inject a fake gzipped payload directly into the uploader queue. */
    uint8_t fake[] = { (uint8_t)0x1F, (uint8_t)0x8B, (uint8_t)0x08, (uint8_t)0,
                       (uint8_t)0xAA, (uint8_t)0xBB, (uint8_t)0xCC, (uint8_t)0xDD };
    EXPECT(wot_uploader_enqueue(fake, sizeof(fake), (uint32_t)1000) == ESP_OK,
           "enqueue fake log");
    EXPECT(wot_uploader_queue_count() == (uint32_t)1, "queue has 1 file");

    /* First tick primes last_attempt; a second tick past the retry
     * interval triggers an upload attempt. */
    g_http_next_status = 200;
    wot_uploader_tick((uint32_t)0);
    test_clock_fast_forward((uint32_t)WOT_UPLOAD_RETRY_INTERVAL_MS + (uint32_t)10);
    wot_uploader_tick(test_clock_now_ms());

    EXPECT(g_http_call_count == 1, "uploader POSTed once on retry tick");
    EXPECT(wot_uploader_queue_count() == (uint32_t)0,
           "queue empty after 2xx response (delete on 200)");
    EXPECT(g_http_last_body_len == sizeof(fake),
           "POSTed body has expected length");
    EXPECT(g_http_last_body_first_bytes[0] == (uint8_t)0x1F,
           "POSTed body begins with gzip magic byte 0");
}

/* ------------------------------------------------------------------ */
/* Test 6 — upload 5xx → file retained, retry on next attempt           */
/* ------------------------------------------------------------------ */
static void test_upload_5xx_retains_then_retries(void) {
    test_setup();
    setup_recorder((uint8_t)0);
    setup_uploader();
    EXPECT(wot_uploader_start() == ESP_OK, "uploader start");

    uint8_t fake[] = { (uint8_t)0x1F, (uint8_t)0x8B, (uint8_t)0x08, (uint8_t)0,
                       (uint8_t)0xDE, (uint8_t)0xAD, (uint8_t)0xBE, (uint8_t)0xEF };
    EXPECT(wot_uploader_enqueue(fake, sizeof(fake), (uint32_t)2000) == ESP_OK,
           "enqueue fake log");

    /* First attempt: server returns 503. */
    g_http_next_status = 503;
    wot_uploader_tick((uint32_t)0);
    test_clock_fast_forward((uint32_t)WOT_UPLOAD_RETRY_INTERVAL_MS + (uint32_t)10);
    wot_uploader_tick(test_clock_now_ms());
    EXPECT(g_http_call_count == 1, "POST attempt happened on first retry tick");
    EXPECT(wot_uploader_queue_count() == (uint32_t)1,
           "file retained after 5xx (retain on retry)");

    /* Second attempt next interval: server returns 200; file
     * should be deleted. */
    g_http_next_status = 200;
    test_clock_fast_forward((uint32_t)WOT_UPLOAD_RETRY_INTERVAL_MS + (uint32_t)10);
    wot_uploader_tick(test_clock_now_ms());
    EXPECT(g_http_call_count == 2, "second POST happened on next retry interval");
    EXPECT(wot_uploader_queue_count() == (uint32_t)0,
           "file deleted after eventual 2xx success");
}

/* ------------------------------------------------------------------ */
/* Test 7 — upload skipped when wifi not ready, queue retained          */
/* ------------------------------------------------------------------ */
static void test_upload_skipped_when_wifi_down(void) {
    test_setup();
    setup_recorder((uint8_t)0);
    setup_uploader();
    EXPECT(wot_uploader_start() == ESP_OK, "uploader start");

    uint8_t fake[] = { (uint8_t)0x1F, (uint8_t)0x8B, (uint8_t)0x08, (uint8_t)0,
                       (uint8_t)0x01, (uint8_t)0x02, (uint8_t)0x03, (uint8_t)0x04 };
    wot_uploader_enqueue(fake, sizeof(fake), (uint32_t)3000);
    g_wifi_ready = false;

    wot_uploader_tick((uint32_t)0);
    test_clock_fast_forward((uint32_t)WOT_UPLOAD_RETRY_INTERVAL_MS + (uint32_t)10);
    wot_uploader_tick(test_clock_now_ms());
    EXPECT(g_http_call_count == 0, "no POST attempted while wifi down");
    EXPECT(wot_uploader_queue_count() == (uint32_t)1, "queue retained");
}

/* ------------------------------------------------------------------ */
/* Test 8 — gzip footer integrity (CRC32 + ISIZE)                       */
/* ------------------------------------------------------------------ */
static uint32_t crc32_ref(const uint8_t *data, size_t len) {
    uint32_t crc = ~(uint32_t)0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint32_t b = 0; b < (uint32_t)8; b++) {
            uint32_t mask = (uint32_t)0 - (crc & (uint32_t)1);
            crc = (crc >> (uint32_t)1) ^ ((uint32_t)0xEDB88320 & mask);
        }
    }
    return ~crc;
}

static void test_gzip_structural_footer(void) {
    test_setup();
    setup_recorder((uint8_t)0);
    setup_uploader();
    EXPECT(wot_recorder_arm() == ESP_OK, "arm recorder");

    /* Drive a short recording with deterministic content. */
    float above[TEST_NUM_VARS] = { (float)(WOT_TRIGGER_THRESHOLD_PERCENT + 1), 5500.0f, 0.85f };
    for (int i = 0; i < 5; i++) {
        test_clock_fast_forward((uint32_t)80);
        wot_recorder_feed_sample(above);
    }
    /* Drop below threshold and let cooldown elapse. */
    float below[TEST_NUM_VARS] = { 10.0f, 3000.0f, 1.0f };
    test_clock_fast_forward((uint32_t)100);
    wot_recorder_feed_sample(below);
    test_clock_fast_forward((uint32_t)WOT_TRIGGER_COOLDOWN_MS + (uint32_t)50);
    wot_recorder_feed_sample(below);

    EXPECT(g_last_gzip_len >= (size_t)18, "gzip stream produced");
    EXPECT(g_last_gzip[0] == (uint8_t)0x1F && g_last_gzip[1] == (uint8_t)0x8B,
           "gzip magic intact");

    /* Reconstruct the uncompressed payload from stored DEFLATE
     * blocks. Walk past the 10-byte header; each stored block is
     * 5 framing bytes + LEN bytes raw. */
    size_t pos = (size_t)10;
    uint8_t recon[65536];
    size_t  recon_len = 0;
    while (pos + (size_t)5 <= g_last_gzip_len - (size_t)8) {
        bool is_final = (g_last_gzip[pos] & (uint8_t)0x01) != 0;
        size_t lo = (size_t)g_last_gzip[pos + (size_t)1];
        size_t hi = (size_t)g_last_gzip[pos + (size_t)2];
        size_t chunk = lo | (hi << (size_t)8);
        pos += (size_t)5;
        if (pos + chunk > g_last_gzip_len - (size_t)8) break;
        if (recon_len + chunk > sizeof(recon)) break;
        memcpy(&recon[recon_len], &g_last_gzip[pos], chunk);
        recon_len += chunk;
        pos += chunk;
        if (is_final) break;
    }

    /* CRC32 + ISIZE in the footer must match the reconstructed
     * uncompressed payload. */
    size_t footer_pos = g_last_gzip_len - (size_t)8;
    uint32_t crc_le = (uint32_t)g_last_gzip[footer_pos]
                    | ((uint32_t)g_last_gzip[footer_pos + (size_t)1] << (uint32_t)8)
                    | ((uint32_t)g_last_gzip[footer_pos + (size_t)2] << (uint32_t)16)
                    | ((uint32_t)g_last_gzip[footer_pos + (size_t)3] << (uint32_t)24);
    uint32_t isize_le = (uint32_t)g_last_gzip[footer_pos + (size_t)4]
                      | ((uint32_t)g_last_gzip[footer_pos + (size_t)5] << (uint32_t)8)
                      | ((uint32_t)g_last_gzip[footer_pos + (size_t)6] << (uint32_t)16)
                      | ((uint32_t)g_last_gzip[footer_pos + (size_t)7] << (uint32_t)24);
    uint32_t expected_crc = crc32_ref(recon, recon_len);
    EXPECT((uint32_t)recon_len == isize_le, "ISIZE matches reconstructed CSV length");
    EXPECT(expected_crc == crc_le, "CRC32 matches reconstructed CSV bytes");
}

/* ------------------------------------------------------------------ */
/* Test 9 — argument validation and lifecycle invariants                */
/* ------------------------------------------------------------------ */
static void test_argument_validation(void) {
    test_setup();
    setup_recorder((uint8_t)0);
    setup_uploader();

    EXPECT(wot_recorder_init(NULL) == ESP_ERR_INVALID_STATE ||
           wot_recorder_init(NULL) == ESP_ERR_INVALID_ARG,
           "recorder re-init or NULL is rejected");
    EXPECT(wot_recorder_feed_sample(NULL) == ESP_ERR_INVALID_ARG,
           "feed_sample NULL rejected");
    EXPECT(wot_uploader_enqueue(NULL, (size_t)0, (uint32_t)0) == ESP_ERR_INVALID_ARG,
           "enqueue with NULL rejected");
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(void) {
    fprintf(stdout, "=== wot_logger host unit tests ===\n");

    if (feature_manager_init() != ESP_OK) {
        fprintf(stderr, "feature_manager_init failed\n");
        return 1;
    }
    /* On host build, wot_logger_init() just registers the descriptor
     * and sets the s_initialized flag so wot_logger_start/stop are
     * gated correctly. Recorder + uploader are set up per-test by
     * setup_recorder()/setup_uploader(). */
    if (wot_logger_init() != ESP_OK) {
        fprintf(stderr, "wot_logger_init failed\n");
        return 1;
    }

    test_start_no_other_feature();
    test_arbitration_swap_into_wot();
    test_threshold_starts_recording();
    test_hard_cap_ends_recording();
    test_upload_success_deletes();
    test_upload_5xx_retains_then_retries();
    test_upload_skipped_when_wifi_down();
    test_gzip_structural_footer();
    test_argument_validation();

    if (g_failures == 0) {
        fprintf(stdout, "\n=== OK: all wot_logger unit tests passed ===\n");
        return 0;
    }
    fprintf(stderr, "\n=== FAIL: %d unit test assertions failed ===\n", g_failures);
    return 1;
}
