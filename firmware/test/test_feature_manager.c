/*
 * test_feature_manager.c — host-runnable unit tests for the feature
 * manager state arbiter.
 *
 * Built into firmware/test/feature_manager/host_test_runner via the
 * Makefile in that directory and exercised by
 * firmware/test/feature_manager/eval.sh.
 *
 * Required scenarios (per CLAUDE_CODE_KICKOFF.md Prompt 1):
 *   1. Register a feature, start it, verify active.
 *   2. Request stop, verify inactive.
 *   3. Two features registered. Start A. Request start B. Verify A.stop()
 *      called first, then B.start(), then active == B (the swap path).
 *   4. Idempotent: request start of already-active feature returns OK
 *      without re-calling start().
 *   5. Failure mode: feature whose stop() returns error. Request another
 *      feature. Verify swap fails, active stays on the failed-stop
 *      feature, err_out populated.
 *   + Argument validation: FEATURE_NONE / FEATURE_COUNT bounds.
 */

#include "feature_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Mock features                                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
    int       started_count;
    int       stopped_count;
    bool      running;
    esp_err_t start_rc;
    esp_err_t stop_rc;
    bool      ignore_stop;   /* if true, stop() leaves running=true */
} mock_state_t;

static mock_state_t mock_a;
static mock_state_t mock_b;

#define DEFINE_MOCK(LETTER, STATE)                                          \
    static esp_err_t mock_##LETTER##_start(void) {                          \
        STATE.started_count++;                                              \
        if (STATE.start_rc == ESP_OK) STATE.running = true;                 \
        return STATE.start_rc;                                              \
    }                                                                       \
    static esp_err_t mock_##LETTER##_stop(void) {                           \
        STATE.stopped_count++;                                              \
        if (STATE.stop_rc == ESP_OK && !STATE.ignore_stop) STATE.running = false; \
        return STATE.stop_rc;                                               \
    }                                                                       \
    static bool mock_##LETTER##_is_running(void) { return STATE.running; }

DEFINE_MOCK(a, mock_a)
DEFINE_MOCK(b, mock_b)

static const feature_descriptor_t desc_a = {
    .id         = FEATURE_WOT_LOGGING,
    .name       = "mock_a",
    .start      = mock_a_start,
    .stop       = mock_a_stop,
    .is_running = mock_a_is_running,
};

static const feature_descriptor_t desc_b = {
    .id         = FEATURE_LIVE_TUNE,
    .name       = "mock_b",
    .start      = mock_b_start,
    .stop       = mock_b_stop,
    .is_running = mock_b_is_running,
};

/* ---------------------------------------------------------------------- */
/* Tiny EXPECT framework                                                  */
/* ---------------------------------------------------------------------- */

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

static void mocks_reset(void) {
    memset(&mock_a, 0, sizeof(mock_a));
    memset(&mock_b, 0, sizeof(mock_b));
}

/* Per-test setup. Order matters:
 *   1. Reset mocks so any sticky stop_rc=FAIL or ignore_stop=true from a
 *      prior test does not trip cleanup.
 *   2. Force any active feature back to FEATURE_NONE through the public API.
 *      With clean stop_rc=OK, this stop succeeds.
 *   3. Reset mocks AGAIN so the test starts with zero call counters —
 *      otherwise the cleanup stop in step 2 would inflate them. */
static void test_setup(void) {
    mocks_reset();
    feature_id_t active = feature_manager_active();
    if (active != FEATURE_NONE) {
        feature_manager_request_stop(active);
    }
    mocks_reset();
}

/* ---------------------------------------------------------------------- */
/* Test 1 — register + start + active                                     */
/* ---------------------------------------------------------------------- */
static void test_register_and_start(void) {
    test_setup();

    char err[64] = {0};
    esp_err_t rc = feature_manager_request_start(FEATURE_WOT_LOGGING, err, sizeof(err));
    EXPECT(rc == ESP_OK, "start of registered feature returns ESP_OK");
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING, "active is FEATURE_WOT_LOGGING");
    EXPECT(strcmp(feature_manager_active_name(), "mock_a") == 0, "active_name is mock_a");
    EXPECT(mock_a.started_count == 1, "A.start called exactly once");
    EXPECT(err[0] == '\0', "err_out left empty on success");
}

/* ---------------------------------------------------------------------- */
/* Test 2 — request stop, verify inactive                                 */
/* ---------------------------------------------------------------------- */
static void test_request_stop(void) {
    test_setup();

    feature_manager_request_start(FEATURE_WOT_LOGGING, NULL, 0);
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING, "precondition: A active");

    esp_err_t rc = feature_manager_request_stop(FEATURE_WOT_LOGGING);
    EXPECT(rc == ESP_OK, "stop of active feature returns ESP_OK");
    EXPECT(feature_manager_active() == FEATURE_NONE, "no feature active after stop");
    EXPECT(mock_a.stopped_count == 1, "A.stop called exactly once");
}

/* ---------------------------------------------------------------------- */
/* Test 3 — swap A → B (the central preempt path)                          */
/* ---------------------------------------------------------------------- */
static void test_swap_a_to_b(void) {
    test_setup();

    feature_manager_request_start(FEATURE_WOT_LOGGING, NULL, 0);
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING, "precondition: A active");

    char err[64] = {0};
    esp_err_t rc = feature_manager_request_start(FEATURE_LIVE_TUNE, err, sizeof(err));

    EXPECT(rc == ESP_OK, "swap A→B returns ESP_OK");
    EXPECT(mock_a.stopped_count == 1, "A.stop called during swap");
    EXPECT(mock_b.started_count == 1, "B.start called after A.stop");
    EXPECT(feature_manager_active() == FEATURE_LIVE_TUNE, "active is now B");
}

/* ---------------------------------------------------------------------- */
/* Test 4 — idempotent re-start of already-active feature                  */
/* ---------------------------------------------------------------------- */
static void test_idempotent_start(void) {
    test_setup();

    feature_manager_request_start(FEATURE_WOT_LOGGING, NULL, 0);
    int starts_before = mock_a.started_count;

    esp_err_t rc = feature_manager_request_start(FEATURE_WOT_LOGGING, NULL, 0);
    EXPECT(rc == ESP_OK, "idempotent re-start returns ESP_OK");
    EXPECT(mock_a.started_count == starts_before, "A.start NOT recalled when already active");
    EXPECT(mock_a.stopped_count == 0, "A.stop NOT called on idempotent path");
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING, "A still active");
}

/* ---------------------------------------------------------------------- */
/* Test 5 — swap when stop() returns error → swap fails, active unchanged */
/* ---------------------------------------------------------------------- */
static void test_swap_when_stop_fails(void) {
    test_setup();

    /* Start A clean, then arm A.stop to return ESP_FAIL on next call. */
    feature_manager_request_start(FEATURE_WOT_LOGGING, NULL, 0);
    mock_a.stop_rc = ESP_FAIL;

    char err[64] = {0};
    esp_err_t rc = feature_manager_request_start(FEATURE_LIVE_TUNE, err, sizeof(err));

    EXPECT(rc != ESP_OK, "swap aborts when A.stop fails");
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING,
           "active stays on failed-stop feature A");
    EXPECT(mock_b.started_count == 0, "B.start was NOT called");
    EXPECT(strlen(err) > 0, "err_out populated with failure reason");
}

/* ---------------------------------------------------------------------- */
/* Test 6 — swap when is_running() never falls within timeout              */
/* ---------------------------------------------------------------------- */
static void test_swap_when_stop_times_out(void) {
    test_setup();

    feature_manager_request_start(FEATURE_WOT_LOGGING, NULL, 0);
    /* stop() returns OK, but feature ignores it and stays running. */
    mock_a.ignore_stop = true;

    char err[64] = {0};
    esp_err_t rc = feature_manager_request_start(FEATURE_LIVE_TUNE, err, sizeof(err));

    EXPECT(rc != ESP_OK, "swap aborts when stop times out");
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING, "active stays on stuck feature");
    EXPECT(mock_b.started_count == 0, "B.start was NOT called after timeout");
    EXPECT(strlen(err) > 0, "err_out populated for timeout case");

    /* Recovery: clear the ignore flag and force-stop A so later tests start clean. */
    mock_a.ignore_stop = false;
    mock_a.running = false;
    feature_manager_request_stop(FEATURE_WOT_LOGGING);
}

/* ---------------------------------------------------------------------- */
/* Test 7 — argument validation                                            */
/* ---------------------------------------------------------------------- */
static void test_argument_validation(void) {
    test_setup();

    char err[64] = {0};
    EXPECT(feature_manager_request_start(FEATURE_NONE, err, sizeof(err)) == ESP_ERR_INVALID_ARG,
           "request_start(FEATURE_NONE) → ESP_ERR_INVALID_ARG");
    EXPECT(strlen(err) > 0, "err_out populated for FEATURE_NONE");

    err[0] = '\0';
    EXPECT(feature_manager_request_start(FEATURE_COUNT, err, sizeof(err)) == ESP_ERR_INVALID_ARG,
           "request_start(FEATURE_COUNT) → ESP_ERR_INVALID_ARG");
    EXPECT(strlen(err) > 0, "err_out populated for FEATURE_COUNT");

    EXPECT(feature_manager_request_stop(FEATURE_NONE) == ESP_OK,
           "request_stop(FEATURE_NONE) → ESP_OK no-op");
    EXPECT(feature_manager_request_stop(FEATURE_COUNT) == ESP_ERR_INVALID_ARG,
           "request_stop(FEATURE_COUNT) → ESP_ERR_INVALID_ARG");

    /* NULL err_out / zero err_len must not crash. */
    EXPECT(feature_manager_request_start(FEATURE_NONE, NULL, 0) == ESP_ERR_INVALID_ARG,
           "NULL err_out is tolerated on invalid id");
}

/* ---------------------------------------------------------------------- */
/* Test 8 — register input validation                                      */
/* ---------------------------------------------------------------------- */
static void test_register_validation(void) {
    EXPECT(feature_manager_register(NULL) == ESP_ERR_INVALID_ARG,
           "register(NULL) rejected");

    feature_descriptor_t bad = desc_a;
    bad.id = FEATURE_NONE;
    EXPECT(feature_manager_register(&bad) == ESP_ERR_INVALID_ARG,
           "register with FEATURE_NONE id rejected");

    bad = desc_a;
    bad.id = FEATURE_COUNT;
    EXPECT(feature_manager_register(&bad) == ESP_ERR_INVALID_ARG,
           "register with out-of-range id rejected");

    bad = desc_a;
    bad.start = NULL;
    EXPECT(feature_manager_register(&bad) == ESP_ERR_INVALID_ARG,
           "register with NULL start fn rejected");

    /* Duplicate registration of an already-registered id must fail. */
    EXPECT(feature_manager_register(&desc_a) == ESP_ERR_INVALID_STATE,
           "duplicate register of id rejected");
}

/* ---------------------------------------------------------------------- */
/* main                                                                   */
/* ---------------------------------------------------------------------- */
int main(void) {
    printf("=== feature_manager host unit tests ===\n");

    if (feature_manager_init() != ESP_OK) {
        fprintf(stderr, "FAIL: feature_manager_init returned non-OK\n");
        return 1;
    }
    /* init must be idempotent. */
    if (feature_manager_init() != ESP_OK) {
        fprintf(stderr, "FAIL: feature_manager_init not idempotent\n");
        return 1;
    }

    if (feature_manager_register(&desc_a) != ESP_OK) {
        fprintf(stderr, "FAIL: register desc_a\n");
        return 1;
    }
    if (feature_manager_register(&desc_b) != ESP_OK) {
        fprintf(stderr, "FAIL: register desc_b\n");
        return 1;
    }

    test_register_and_start();
    test_request_stop();
    test_swap_a_to_b();
    test_idempotent_start();
    test_swap_when_stop_fails();
    test_swap_when_stop_times_out();
    test_argument_validation();
    test_register_validation();

    if (g_failures == 0) {
        printf("\n=== OK: all feature_manager unit tests passed ===\n");
        return 0;
    }
    fprintf(stderr, "\n=== FAIL: %d unit test assertions failed ===\n", g_failures);
    return 1;
}
