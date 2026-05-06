/*
 * test_dtc.c — host-runnable unit tests for the DTC read/clear feature.
 *
 * Built into firmware/test/dtc/host_test_runner via the Makefile in
 * that directory and exercised by firmware/test/dtc/eval.sh.
 *
 * Required scenarios (per the kickoff prompt):
 *   1. read positive               — valid 0x59 0x02 response with DTCs
 *   2. read negative (NRC)         — 0x7F 0x19 [NRC]
 *   3. read multi-frame            — long response with many DTCs
 *   4. clear positive              — 0x54 + pre-clear read counts active codes
 *   5. arbitration with mock feature — DTC preempts a mock feature
 *   6. manager mutex held during UDS exchange — feature_manager_active()
 *      reads FEATURE_DTC inside the mock transport callback
 *   7. idempotent re-register      — dtc_register_with_feature_manager
 *      called twice both return OK
 *   + DTC code formatting (SAE J2012) sanity
 *   + description lookup hit + miss
 *   + argument validation
 */

#include "dtc.h"
#include "dtc_uds.h"
#include "dtc_config.h"
#include "feature_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tiny EXPECT framework (same shape as test_wot_logger.c)             */
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
/* Mock UDS transport                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int      read_response_len;       /* > 0: byte length of payload to return.
                                         0: simulate transport timeout.
                                         < 0: simulate transport error. */
    uint8_t  read_response_payload[256];

    int      clear_response_len;
    uint8_t  clear_response_payload[16];

    int      read_calls;
    int      clear_calls;
    int      unknown_calls;

    bool     check_active_during_call;
    bool     active_check_failed;
} mock_uds_t;

static mock_uds_t g_uds;

static int mock_uds_request(const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap,
                            uint32_t timeout_ms, void *ctx) {
    (void)timeout_ms;
    (void)ctx;
    if (g_uds.check_active_during_call &&
        feature_manager_active() != FEATURE_DTC) {
        g_uds.active_check_failed = true;
    }

    if (req_len > (size_t)0 && req[0] == (uint8_t)DTC_UDS_SID_READ) {
        g_uds.read_calls++;
        if (g_uds.read_response_len <= 0) return g_uds.read_response_len;
        size_t n = (size_t)g_uds.read_response_len;
        if (n > resp_cap) n = resp_cap;
        memcpy(resp, g_uds.read_response_payload, n);
        return (int)n;
    }
    if (req_len > (size_t)0 && req[0] == (uint8_t)DTC_UDS_SID_CLEAR) {
        g_uds.clear_calls++;
        if (g_uds.clear_response_len <= 0) return g_uds.clear_response_len;
        size_t n = (size_t)g_uds.clear_response_len;
        if (n > resp_cap) n = resp_cap;
        memcpy(resp, g_uds.clear_response_payload, n);
        return (int)n;
    }
    g_uds.unknown_calls++;
    return -1;
}

/* Helper: encode a DTC into the canonical 3-byte SAE J2012 format.
 * Used to populate mock read responses with deterministic codes. */
static void encode_dtc_bytes(char letter, uint8_t digit1, uint8_t digit2,
                             uint8_t digit3, uint8_t digit4,
                             uint8_t out[3]) {
    uint8_t type;
    switch (letter) {
        case 'P': type = 0; break;
        case 'C': type = 1; break;
        case 'B': type = 2; break;
        case 'U': type = 3; break;
        default:  type = 0; break;
    }
    /* hi byte: type[7:6] | digit1[5:4] | digit2[3:0] */
    out[0] = (uint8_t)(((uint8_t)(type & 0x3) << 6) |
                       ((uint8_t)(digit1 & 0x3) << 4) |
                       (uint8_t)(digit2 & 0xF));
    /* mid byte: digit3[7:4] | digit4[3:0] */
    out[1] = (uint8_t)(((uint8_t)(digit3 & 0xF) << 4) |
                       (uint8_t)(digit4 & 0xF));
    out[2] = (uint8_t)0;
}

static void mock_uds_setup_read_positive_two_codes(void) {
    /* 0x59 0x02 0x00 [P0420 status=0x09] [P0171 status=0x08] */
    g_uds.read_response_payload[0] = (uint8_t)DTC_UDS_READ_POSITIVE_SID;
    g_uds.read_response_payload[1] = (uint8_t)DTC_UDS_SUBFUNC_REPORT_BY_STATUS;
    g_uds.read_response_payload[2] = (uint8_t)0x00; /* statusAvailabilityMask */
    encode_dtc_bytes('P', 0, 4, 2, 0, &g_uds.read_response_payload[3]);
    g_uds.read_response_payload[6] = (uint8_t)0x09;
    encode_dtc_bytes('P', 0, 1, 7, 1, &g_uds.read_response_payload[7]);
    g_uds.read_response_payload[10] = (uint8_t)0x08;
    g_uds.read_response_len = 11;
}

static void mock_uds_setup_read_negative(uint8_t nrc) {
    g_uds.read_response_payload[0] = (uint8_t)DTC_UDS_NEGATIVE_RESPONSE;
    g_uds.read_response_payload[1] = (uint8_t)DTC_UDS_SID_READ;
    g_uds.read_response_payload[2] = nrc;
    g_uds.read_response_len = (int)DTC_UDS_NEGATIVE_RESPONSE_BYTES;
}

static void mock_uds_setup_read_multi_frame_ten_codes(void) {
    g_uds.read_response_payload[0] = (uint8_t)DTC_UDS_READ_POSITIVE_SID;
    g_uds.read_response_payload[1] = (uint8_t)DTC_UDS_SUBFUNC_REPORT_BY_STATUS;
    g_uds.read_response_payload[2] = (uint8_t)0xFF;
    /* Ten distinct codes: P0001..P000A. */
    static const uint8_t k_low_nibbles[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    size_t off = (size_t)DTC_READ_RESPONSE_PREAMBLE_BYTES;
    for (size_t i = 0; i < (size_t)10; i++) {
        encode_dtc_bytes('P', 0, 0,
                         (uint8_t)((k_low_nibbles[i] >> 4) & 0xF),
                         (uint8_t)(k_low_nibbles[i] & 0xF),
                         &g_uds.read_response_payload[off]);
        g_uds.read_response_payload[off + (size_t)3] = (uint8_t)0x09;
        off += (size_t)DTC_RECORD_BYTES;
    }
    g_uds.read_response_len = (int)off;
}

static void mock_uds_setup_clear_positive(void) {
    g_uds.clear_response_payload[0] = (uint8_t)DTC_UDS_CLEAR_POSITIVE_SID;
    g_uds.clear_response_len = 1;
}

/* Build a positive read response with zero DTC records — preamble
 * only. Used by the read-empty scenario. */
static void mock_uds_setup_read_empty(void) {
    g_uds.read_response_payload[0] = (uint8_t)DTC_UDS_READ_POSITIVE_SID;
    g_uds.read_response_payload[1] = (uint8_t)DTC_UDS_SUBFUNC_REPORT_BY_STATUS;
    g_uds.read_response_payload[2] = (uint8_t)0x00; /* statusAvailabilityMask */
    g_uds.read_response_len = (int)DTC_READ_RESPONSE_PREAMBLE_BYTES;
}

/* Build a positive read response carrying a mixed bag: one ACTIVE
 * (testFailed bit set) DTC and one PENDING (pendingDTC bit set) DTC.
 * The test asserts that the raw status bytes are preserved, which is
 * how the UI tells the two states apart per ISO 14229-1. */
static void mock_uds_setup_read_active_and_pending_mix(void) {
    /* P0300 — random misfire — testFailed | confirmedDTC = 0x09 (active) */
    /* P0420 — catalyst        — pendingDTC = 0x04 (pending only) */
    g_uds.read_response_payload[0] = (uint8_t)DTC_UDS_READ_POSITIVE_SID;
    g_uds.read_response_payload[1] = (uint8_t)DTC_UDS_SUBFUNC_REPORT_BY_STATUS;
    g_uds.read_response_payload[2] = (uint8_t)0xFF;
    encode_dtc_bytes('P', 0, 3, 0, 0, &g_uds.read_response_payload[3]);
    g_uds.read_response_payload[6] = (uint8_t)0x09; /* testFailed | confirmed = active */
    encode_dtc_bytes('P', 0, 4, 2, 0, &g_uds.read_response_payload[7]);
    g_uds.read_response_payload[10] = (uint8_t)0x04; /* pendingDTC only */
    g_uds.read_response_len = 11;
}

static void mock_uds_setup_clear_negative(uint8_t nrc) {
    g_uds.clear_response_payload[0] = (uint8_t)DTC_UDS_NEGATIVE_RESPONSE;
    g_uds.clear_response_payload[1] = (uint8_t)DTC_UDS_SID_CLEAR;
    g_uds.clear_response_payload[2] = nrc;
    g_uds.clear_response_len = (int)DTC_UDS_NEGATIVE_RESPONSE_BYTES;
}

/* ------------------------------------------------------------------ */
/* Mock alternate feature for arbitration test                          */
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

/* The host harness does not compile wot_logger, so FEATURE_WOT_LOGGING
 * is unregistered here. We slot the mock there so the swap-test
 * literally exercises "WOT logger → DTC via feature_manager" as
 * called out in the kickoff prompt. */
static const feature_descriptor_t mock_feature_desc = {
    .id         = FEATURE_WOT_LOGGING,
    .name       = "mock_wot_logger",
    .start      = mockf_start,
    .stop       = mockf_stop,
    .is_running = mockf_is_running,
};

/* ------------------------------------------------------------------ */
/* Per-test setup                                                      */
/* ------------------------------------------------------------------ */

static void test_setup(void) {
    /* Drain any active feature from a prior test cleanly. */
    feature_id_t active = feature_manager_active();
    if (active != FEATURE_NONE) {
        feature_manager_request_stop(active);
    }
    memset(&g_uds, 0, sizeof(g_uds));
    memset(&g_mockf, 0, sizeof(g_mockf));
    /* Re-install the mock transport so prior tests' deinit (if any)
     * does not leak. */
    EXPECT(dtc_uds_init(mock_uds_request, NULL) == ESP_OK,
           "mock UDS transport installed");
}

/* ------------------------------------------------------------------ */
/* Test 1 — DTC code formatter (SAE J2012)                              */
/* ------------------------------------------------------------------ */
static void test_format_dtc_code(void) {
    test_setup();
    char buf[DTC_CODE_STRING_LEN];

    /* P0420 — bytes per encode_dtc_bytes('P',0,4,2,0): hi=0x04, mid=0x20 */
    uint8_t enc[3];
    encode_dtc_bytes('P', 0, 4, 2, 0, enc);
    dtc_uds_format_dtc_code(enc[0], enc[1], enc[2], buf, sizeof(buf));
    EXPECT(strcmp(buf, "P0420") == 0, "encodes P0420 round-trip");

    encode_dtc_bytes('C', 1, 0xA, 0xB, 0xC, enc);
    dtc_uds_format_dtc_code(enc[0], enc[1], enc[2], buf, sizeof(buf));
    EXPECT(strcmp(buf, "C1ABC") == 0, "encodes C1ABC (chassis, hex digits)");

    encode_dtc_bytes('B', 2, 0xF, 0xF, 0xF, enc);
    dtc_uds_format_dtc_code(enc[0], enc[1], enc[2], buf, sizeof(buf));
    EXPECT(strcmp(buf, "B2FFF") == 0, "encodes B2FFF (body, max nibbles)");

    encode_dtc_bytes('U', 3, 0, 0, 0, enc);
    dtc_uds_format_dtc_code(enc[0], enc[1], enc[2], buf, sizeof(buf));
    EXPECT(strcmp(buf, "U3000") == 0, "encodes U3000 (network)");
}

/* ------------------------------------------------------------------ */
/* Test 2 — read positive (two DTCs)                                    */
/* ------------------------------------------------------------------ */
static void test_read_positive(void) {
    test_setup();
    mock_uds_setup_read_positive_two_codes();

    dtc_entry_t entries[8];
    size_t      count = 0;
    char        err[128] = {0};

    esp_err_t rc = dtc_read((uint8_t)DTC_DEFAULT_STATUS_MASK,
                            entries, sizeof(entries) / sizeof(entries[0]),
                            &count, err, sizeof(err));
    EXPECT(rc == ESP_OK, "dtc_read returns ESP_OK on positive response");
    EXPECT(count == (size_t)2, "two DTCs returned");
    EXPECT(strcmp(entries[0].code, "P0420") == 0, "first code is P0420");
    EXPECT(entries[0].status == (uint8_t)0x09, "first status byte preserved");
    EXPECT(entries[0].description != NULL, "first description resolved (non-NULL)");
    EXPECT(strstr(entries[0].description, "Catalyst") != NULL,
           "P0420 description mentions Catalyst");
    EXPECT(strcmp(entries[1].code, "P0171") == 0, "second code is P0171");
    EXPECT(strstr(entries[1].description, "Lean") != NULL,
           "P0171 description mentions Lean");
    EXPECT(feature_manager_active() == FEATURE_NONE,
           "feature_manager released after dtc_read returns");
}

/* ------------------------------------------------------------------ */
/* Test 3 — read negative response (NRC)                                */
/* ------------------------------------------------------------------ */
static void test_read_negative_nrc(void) {
    test_setup();
    mock_uds_setup_read_negative((uint8_t)0x22); /* Conditions Not Correct */

    dtc_entry_t entries[8];
    size_t      count = (size_t)99; /* sentinel */
    char        err[128] = {0};

    esp_err_t rc = dtc_read((uint8_t)0,
                            entries, sizeof(entries) / sizeof(entries[0]),
                            &count, err, sizeof(err));
    EXPECT(rc == ESP_ERR_INVALID_RESPONSE, "NRC surfaces as INVALID_RESPONSE");
    EXPECT(count == (size_t)0, "out_count zeroed on NRC");
    EXPECT(dtc_uds_last_nrc() == (uint8_t)0x22, "last NRC byte captured");
    EXPECT(strstr(err, "0x22") != NULL || strstr(err, "0x22") != NULL,
           "err_out mentions the NRC byte (0x22)");
    EXPECT(feature_manager_active() == FEATURE_NONE,
           "feature_manager released after NRC failure");
}

/* ------------------------------------------------------------------ */
/* Test 4 — read multi-frame (10 DTCs in one logical response)          */
/* ------------------------------------------------------------------ */
static void test_read_multi_frame(void) {
    test_setup();
    mock_uds_setup_read_multi_frame_ten_codes();

    dtc_entry_t entries[16];
    size_t      count = 0;
    char        err[128] = {0};

    esp_err_t rc = dtc_read((uint8_t)0,
                            entries, sizeof(entries) / sizeof(entries[0]),
                            &count, err, sizeof(err));
    EXPECT(rc == ESP_OK, "multi-frame response parsed cleanly");
    EXPECT(count == (size_t)10, "all 10 DTCs surfaced (multi-frame parse)");
    EXPECT(strcmp(entries[0].code, "P0001") == 0, "first multi-frame code is P0001");
    EXPECT(strcmp(entries[9].code, "P000A") == 0, "tenth multi-frame code is P000A");
}

/* ------------------------------------------------------------------ */
/* Test 5 — clear positive (with pre-clear count)                       */
/* ------------------------------------------------------------------ */
static void test_clear_positive(void) {
    test_setup();
    mock_uds_setup_read_positive_two_codes();
    mock_uds_setup_clear_positive();

    uint16_t cleared = (uint16_t)0xABCD;
    char     err[128] = {0};

    esp_err_t rc = dtc_clear(&cleared, err, sizeof(err));
    EXPECT(rc == ESP_OK, "dtc_clear returns ESP_OK on positive 0x54");
    EXPECT(cleared == (uint16_t)2, "cleared_count reflects pre-clear read of 2 codes");
    EXPECT(g_uds.read_calls == 1, "exactly one pre-clear read issued");
    EXPECT(g_uds.clear_calls == 1, "exactly one 0x14 clear issued");
    EXPECT(feature_manager_active() == FEATURE_NONE,
           "feature_manager released after clear");
}

/* ------------------------------------------------------------------ */
/* Test 6 — clear negative response (NRC 0x33 securityAccessDenied)     */
/* ------------------------------------------------------------------ */
static void test_clear_negative_nrc(void) {
    test_setup();
    mock_uds_setup_read_positive_two_codes();
    /* NRC 0x33 = securityAccessDenied — the realistic refusal an ECU
     * returns when 0x14 ClearDiagnosticInformation is attempted from
     * an unauthenticated session. Per the kickoff prompt's required
     * scenarios. */
    mock_uds_setup_clear_negative((uint8_t)0x33);

    uint16_t cleared = (uint16_t)42;
    char     err[128] = {0};

    esp_err_t rc = dtc_clear(&cleared, err, sizeof(err));
    EXPECT(rc == ESP_ERR_INVALID_RESPONSE, "clear NRC 0x33 surfaces as INVALID_RESPONSE");
    EXPECT(cleared == (uint16_t)0, "cleared_count zeroed on failure");
    EXPECT(strstr(err, "0x33") != NULL, "err mentions the NRC byte 0x33");
    EXPECT(dtc_uds_last_nrc() == (uint8_t)0x33, "last NRC byte captured");
}

/* ------------------------------------------------------------------ */
/* Test 6b — read empty (zero DTC records, preamble only)               */
/* ------------------------------------------------------------------ */
static void test_read_empty(void) {
    test_setup();
    mock_uds_setup_read_empty();

    dtc_entry_t entries[4];
    size_t      count = (size_t)999; /* sentinel */
    char        err[128] = {0};

    esp_err_t rc = dtc_read((uint8_t)0,
                            entries, sizeof(entries) / sizeof(entries[0]),
                            &count, err, sizeof(err));
    EXPECT(rc == ESP_OK, "read empty response returns ESP_OK");
    EXPECT(count == (size_t)0, "out_count == 0 when ECU reports no DTCs");
    EXPECT(err[0] == '\0', "err_out left empty on clean read with zero codes");
}

/* ------------------------------------------------------------------ */
/* Test 6c — read mixed active + pending, status bits preserved         */
/* ------------------------------------------------------------------ */
static void test_read_active_pending_mix(void) {
    test_setup();
    mock_uds_setup_read_active_and_pending_mix();

    dtc_entry_t entries[4];
    size_t      count = (size_t)0;
    char        err[128] = {0};

    esp_err_t rc = dtc_read((uint8_t)0,
                            entries, sizeof(entries) / sizeof(entries[0]),
                            &count, err, sizeof(err));
    EXPECT(rc == ESP_OK, "mixed active+pending response parsed cleanly");
    EXPECT(count == (size_t)2, "two DTCs surfaced");

    /* Active code (P0300, status 0x09): bit 0 testFailed AND bit 3
     * confirmedDTC must be set; pendingDTC bit 2 must NOT be set. */
    EXPECT(strcmp(entries[0].code, "P0300") == 0, "first code is P0300");
    EXPECT((entries[0].status & (uint8_t)0x01) != (uint8_t)0,
           "P0300 has testFailed bit set (active)");
    EXPECT((entries[0].status & (uint8_t)0x08) != (uint8_t)0,
           "P0300 has confirmedDTC bit set");
    EXPECT((entries[0].status & (uint8_t)0x04) == (uint8_t)0,
           "P0300 does NOT have pendingDTC bit (it is active, not pending)");

    /* Pending code (P0420, status 0x04): bit 2 pendingDTC set; testFailed
     * and confirmedDTC NOT set. */
    EXPECT(strcmp(entries[1].code, "P0420") == 0, "second code is P0420");
    EXPECT((entries[1].status & (uint8_t)0x04) != (uint8_t)0,
           "P0420 has pendingDTC bit set");
    EXPECT((entries[1].status & (uint8_t)0x01) == (uint8_t)0,
           "P0420 does NOT have testFailed (pending only)");
}

/* ------------------------------------------------------------------ */
/* Test 7 — arbitration: DTC preempts a mock feature in another slot    */
/* ------------------------------------------------------------------ */
static void test_arbitration_with_mock_feature(void) {
    test_setup();

    esp_err_t reg_rc = feature_manager_register(&mock_feature_desc);
    EXPECT(reg_rc == ESP_OK || reg_rc == ESP_ERR_INVALID_STATE,
           "mock feature registered (or already)");

    char fmerr[64] = {0};
    EXPECT(feature_manager_request_start(FEATURE_WOT_LOGGING, fmerr, sizeof(fmerr)) == ESP_OK,
           "start mock WOT logger feature");
    EXPECT(feature_manager_active() == FEATURE_WOT_LOGGING, "mock WOT logger now active");

    /* Now perform a DTC read. dtc_read must arbitrate cleanly through
     * feature_manager — that means stop() the mock, do the UDS work,
     * then leave the manager in NONE (because dtc_read ends with a
     * request_stop). */
    mock_uds_setup_read_positive_two_codes();
    dtc_entry_t entries[4];
    size_t      count = 0;
    char        err[128] = {0};
    esp_err_t   rc = dtc_read((uint8_t)0, entries,
                              sizeof(entries) / sizeof(entries[0]),
                              &count, err, sizeof(err));
    EXPECT(rc == ESP_OK, "dtc_read succeeded through arbitration");
    EXPECT(g_mockf.stop_count >= 1, "mock feature stop() called by arbitrator");
    EXPECT(feature_manager_active() == FEATURE_NONE,
           "no feature active after dtc_read returns");
    EXPECT(count == (size_t)2, "DTCs still surfaced through the arbitration path");
}

/* ------------------------------------------------------------------ */
/* Test 8 — feature_manager active is FEATURE_DTC during UDS exchange   */
/*           (the "manager mutex held during UDS exchange" invariant)   */
/* ------------------------------------------------------------------ */
static void test_active_during_uds_exchange(void) {
    test_setup();
    mock_uds_setup_read_positive_two_codes();
    g_uds.check_active_during_call = true;

    dtc_entry_t entries[4];
    size_t      count = 0;
    char        err[128] = {0};
    esp_err_t rc = dtc_read((uint8_t)0, entries,
                            sizeof(entries) / sizeof(entries[0]),
                            &count, err, sizeof(err));
    EXPECT(rc == ESP_OK, "dtc_read OK");
    EXPECT(!g_uds.active_check_failed,
           "feature_manager_active() == FEATURE_DTC for every UDS callback invocation");
    EXPECT(g_uds.read_calls >= 1, "mock transport observed at least one call");
    EXPECT(count >= (size_t)1, "at least one DTC parsed during the active-check exchange");
}

/* ------------------------------------------------------------------ */
/* Test 9 — register_with_feature_manager is idempotent                  */
/* ------------------------------------------------------------------ */
static void test_idempotent_register(void) {
    test_setup();
    /* dtc_feature_init() in main() already registered. A second call
     * here must be tolerated. */
    EXPECT(dtc_register_with_feature_manager() == ESP_OK,
           "second register_with_feature_manager returns ESP_OK");
    EXPECT(dtc_register_with_feature_manager() == ESP_OK,
           "third register_with_feature_manager still ESP_OK");
}

/* ------------------------------------------------------------------ */
/* Test 10 — argument validation                                        */
/* ------------------------------------------------------------------ */
static void test_argument_validation(void) {
    test_setup();
    char err[64] = {0};

    EXPECT(dtc_read((uint8_t)0, NULL, (size_t)8, NULL, err, sizeof(err))
           == ESP_ERR_INVALID_ARG,
           "NULL out_entries rejected");

    dtc_entry_t entries[2];
    EXPECT(dtc_read((uint8_t)0, entries, (size_t)0, NULL, err, sizeof(err))
           == ESP_ERR_INVALID_ARG,
           "zero entries_cap rejected");

    EXPECT(dtc_read((uint8_t)0, entries,
                    sizeof(entries) / sizeof(entries[0]),
                    NULL, err, sizeof(err))
           == ESP_ERR_INVALID_ARG,
           "NULL out_count rejected");

    /* dtc_uds_init's NULL-check */
    EXPECT(dtc_uds_init(NULL, NULL) == ESP_ERR_INVALID_ARG,
           "dtc_uds_init NULL transport rejected");
    /* Reinstall good mock for any later tests. */
    EXPECT(dtc_uds_init(mock_uds_request, NULL) == ESP_OK,
           "mock transport reinstalled");
}

/* ------------------------------------------------------------------ */
/* Test 11 — description lookup hit + miss                              */
/* ------------------------------------------------------------------ */
static void test_description_lookup(void) {
    test_setup();

    /* Set a known DTC and one we don't have descriptions for. */
    g_uds.read_response_payload[0] = (uint8_t)DTC_UDS_READ_POSITIVE_SID;
    g_uds.read_response_payload[1] = (uint8_t)DTC_UDS_SUBFUNC_REPORT_BY_STATUS;
    g_uds.read_response_payload[2] = (uint8_t)0xFF;
    encode_dtc_bytes('P', 0, 4, 2, 0, &g_uds.read_response_payload[3]);
    g_uds.read_response_payload[6] = (uint8_t)0x09; /* P0420 — known */
    encode_dtc_bytes('P', 0, 9, 9, 9, &g_uds.read_response_payload[7]);
    g_uds.read_response_payload[10] = (uint8_t)0x09; /* P0999 — unknown */
    g_uds.read_response_len = 11;

    dtc_entry_t entries[4];
    size_t      count = 0;
    char        err[128] = {0};
    EXPECT(dtc_read((uint8_t)0, entries,
                    sizeof(entries) / sizeof(entries[0]),
                    &count, err, sizeof(err)) == ESP_OK, "lookup-test read OK");
    EXPECT(count == (size_t)2, "two codes surfaced");
    EXPECT(strstr(entries[0].description, "Catalyst") != NULL,
           "known P0420 description resolved");
    EXPECT(strstr(entries[1].description, "manufacturer-specific") != NULL,
           "unknown P0999 falls through to fallback string");
}

/* ------------------------------------------------------------------ */
/* Test 12 — family selection round-trip                                 */
/* ------------------------------------------------------------------ */
static void test_family_selection(void) {
    test_setup();
    dtc_ecu_family_t prev = dtc_feature_set_family(DTC_ECU_FAMILY_MDG1);
    EXPECT(prev == DTC_ECU_FAMILY_MG1, "default family is MG1, set returns previous");
    prev = dtc_feature_set_family(DTC_ECU_FAMILY_MED17);
    EXPECT(prev == DTC_ECU_FAMILY_MDG1, "set returns previous");
    /* Out-of-range coerces to MG1 */
    prev = dtc_feature_set_family((dtc_ecu_family_t)99);
    EXPECT(prev == DTC_ECU_FAMILY_MED17, "set returns previous before coerce");
    /* Restore to MG1 for any subsequent runs. */
    (void)dtc_feature_set_family(DTC_ECU_FAMILY_MG1);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(void) {
    fprintf(stdout, "=== dtc host unit tests ===\n");

    if (feature_manager_init() != ESP_OK) {
        fprintf(stderr, "feature_manager_init failed\n");
        return 1;
    }
    if (dtc_feature_init() != ESP_OK) {
        fprintf(stderr, "dtc_feature_init failed\n");
        return 1;
    }
    /* Host build skips dtc_uds_init internally; install our mock so
     * the very first test does not see an uninitialized layer. */
    if (dtc_uds_init(mock_uds_request, NULL) != ESP_OK) {
        fprintf(stderr, "dtc_uds_init mock install failed\n");
        return 1;
    }

    test_format_dtc_code();
    test_read_positive();
    test_read_negative_nrc();
    test_read_multi_frame();
    test_clear_positive();
    test_clear_negative_nrc();
    test_read_empty();
    test_read_active_pending_mix();
    test_arbitration_with_mock_feature();
    test_active_during_uds_exchange();
    test_idempotent_register();
    test_argument_validation();
    test_description_lookup();
    test_family_selection();

    if (g_failures == 0) {
        fprintf(stdout, "\n=== OK: all dtc unit tests passed ===\n");
        return 0;
    }
    fprintf(stderr, "\n=== FAIL: %d unit test assertions failed ===\n", g_failures);
    return 1;
}
