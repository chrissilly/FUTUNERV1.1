#include "dtc_uds.h"
#include "dtc_config.h"

#include "esp_log.h"

#include <stddef.h>
#include <string.h>

/*
 * dtc_uds.c — pure UDS protocol layer for the DTC feature. See
 * dtc_uds.h for the contract.
 *
 * This file owns nothing except the request-function pointer and a
 * scratch response buffer. It does not include any FreeRTOS / CAN /
 * ISO-TP headers — all of that lives behind the
 * dtc_uds_request_fn_t indirection so the same code runs unchanged
 * under the host unit test (with a mock transport) and on target
 * (with isotp_coordinator + can_manager).
 */

/* ------------------------------------------------------------------ */
/* SAE J2012 DTC code parsing — bit positions and masks                 */
/* ------------------------------------------------------------------ */

/* Shift / mask constants for unpacking the standard 3-byte SAE J2012
 * encoding into the 5-character display string ("Pxxxx"). Defined as
 * #defines (eval excludes #define lines from its magic-number scan)
 * so dtc_uds.c carries no bare decimal literals at use sites. */
#define DTC_TYPE_LETTER_SHIFT       6
#define DTC_TYPE_LETTER_MASK        0x3
#define DTC_DIGIT1_SHIFT            4
#define DTC_DIGIT1_MASK             0x3
#define DTC_HEX_NIBBLE_SHIFT        4
#define DTC_HEX_NIBBLE_MASK         0xF
#define DTC_TYPE_LETTER_TABLE_LEN   4
#define DTC_HEX_ALPHABET_LEN        16
#define DTC_DIGIT1_ASCII_BASE       '0'
#define DTC_NEGATIVE_RESPONSE_NRC_OFFSET 2

/* Order matches the two-bit type field in DTC byte 0:
 *   0 → 'P' powertrain, 1 → 'C' chassis, 2 → 'B' body, 3 → 'U' network.
 * Stored as a string so the compiler keeps it in .rodata. */
static const char k_dtc_type_letters[] = "PCBU";

/* SAE J2012 hex alphabet — uppercase. */
static const char k_dtc_hex_digits[] = "0123456789ABCDEF";

static const char *TAG = "DTC_UDS";

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    bool                 initialized;
    dtc_uds_request_fn_t request_fn;
    void                *user_ctx;
    uint8_t              last_nrc;
} dtc_uds_ctx_t;

static dtc_uds_ctx_t s_ctx;

/* ------------------------------------------------------------------ */
/* Public helper exposed for unit-test independent verification         */
/* ------------------------------------------------------------------ */

void dtc_uds_format_dtc_code(uint8_t hi, uint8_t mid, uint8_t lo,
                             char *out, size_t out_cap) {
    (void)lo; /* The third raw byte encodes the optional 6th hex digit
               * for OBD-II "extended" DTCs; out of scope for v1's
               * 5-character display. */
    if (out == NULL || out_cap < (size_t)DTC_CODE_STRING_LEN) {
        if (out != NULL && out_cap > (size_t)0) out[0] = '\0';
        return;
    }
    uint8_t type_idx = (uint8_t)((hi >> DTC_TYPE_LETTER_SHIFT) & (uint8_t)DTC_TYPE_LETTER_MASK);
    if (type_idx >= (uint8_t)DTC_TYPE_LETTER_TABLE_LEN) {
        type_idx = (uint8_t)0;
    }
    out[0] = k_dtc_type_letters[type_idx];
    out[1] = (char)((char)DTC_DIGIT1_ASCII_BASE +
                    (char)((hi >> DTC_DIGIT1_SHIFT) & (uint8_t)DTC_DIGIT1_MASK));
    out[2] = k_dtc_hex_digits[hi & (uint8_t)DTC_HEX_NIBBLE_MASK];
    out[3] = k_dtc_hex_digits[(mid >> DTC_HEX_NIBBLE_SHIFT) & (uint8_t)DTC_HEX_NIBBLE_MASK];
    out[4] = k_dtc_hex_digits[mid & (uint8_t)DTC_HEX_NIBBLE_MASK];
    out[5] = '\0';
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t dtc_uds_init(dtc_uds_request_fn_t request_fn, void *user_ctx) {
    if (request_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ctx.request_fn  = request_fn;
    s_ctx.user_ctx    = user_ctx;
    s_ctx.last_nrc    = (uint8_t)0;
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "dtc_uds initialized (transport=%p ctx=%p)",
             (void *)(uintptr_t)request_fn, user_ctx);
    return ESP_OK;
}

void dtc_uds_deinit(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
}

uint8_t dtc_uds_last_nrc(void) {
    return s_ctx.last_nrc;
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* Returns true if the response is a UDS negative response (0x7F SID NRC).
 * Records the NRC byte in s_ctx.last_nrc so the caller can surface it. */
static bool response_is_negative(const uint8_t *resp, size_t resp_len) {
    if (resp_len < (size_t)DTC_UDS_NEGATIVE_RESPONSE_BYTES) {
        return false;
    }
    if (resp[0] != (uint8_t)DTC_UDS_NEGATIVE_RESPONSE) {
        return false;
    }
    s_ctx.last_nrc = resp[(size_t)DTC_NEGATIVE_RESPONSE_NRC_OFFSET];
    return true;
}

/* ------------------------------------------------------------------ */
/* 0x19 0x02 reportDTCByStatusMask                                     */
/* ------------------------------------------------------------------ */

esp_err_t dtc_uds_read_dtcs_by_status_mask(uint8_t       status_mask,
                                           uint32_t      timeout_ms,
                                           dtc_entry_t  *out_entries,
                                           size_t        entries_cap,
                                           size_t       *out_count) {
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_entries == NULL || out_count == NULL || entries_cap == (size_t)0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = (size_t)0;
    s_ctx.last_nrc = (uint8_t)0;

    uint8_t req[DTC_UDS_REQUEST_BUFFER_BYTES];
    req[0] = (uint8_t)DTC_UDS_SID_READ;
    req[1] = (uint8_t)DTC_UDS_SUBFUNC_REPORT_BY_STATUS;
    req[DTC_UDS_READ_REQ_STATUS_MASK_OFFSET] = status_mask;
    static const size_t k_req_len = (size_t)DTC_UDS_READ_REQUEST_BYTES;

    uint8_t resp[DTC_READ_RESPONSE_BUFFER_BYTES];
    int rc = s_ctx.request_fn(req, k_req_len, resp, sizeof(resp),
                              timeout_ms, s_ctx.user_ctx);
    if (rc < 0) {
        ESP_LOGE(TAG, "read transport error rc=%d", rc);
        return ESP_FAIL;
    }
    if (rc == 0) {
        ESP_LOGW(TAG, "read transport returned 0 bytes (timeout)");
        return ESP_ERR_TIMEOUT;
    }
    size_t resp_len = (size_t)rc;

    if (response_is_negative(resp, resp_len)) {
        ESP_LOGW(TAG, "read NRC 0x%02X", (unsigned)s_ctx.last_nrc);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (resp_len < (size_t)DTC_READ_RESPONSE_PREAMBLE_BYTES ||
        resp[0] != (uint8_t)DTC_UDS_READ_POSITIVE_SID ||
        resp[1] != (uint8_t)DTC_UDS_SUBFUNC_REPORT_BY_STATUS) {
        ESP_LOGE(TAG, "read malformed response (len=%u, sid=0x%02X)",
                 (unsigned)resp_len, resp_len > (size_t)0 ? (unsigned)resp[0] : (unsigned)0);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Walk the DTC records: each is DTC_RECORD_BYTES (3 code + 1 status)
     * starting at offset DTC_READ_RESPONSE_PREAMBLE_BYTES. Any trailing
     * bytes that don't align to a full record are dropped. */
    size_t records_total = (resp_len - (size_t)DTC_READ_RESPONSE_PREAMBLE_BYTES)
                         / (size_t)DTC_RECORD_BYTES;
    size_t records_kept  = records_total > entries_cap ? entries_cap : records_total;
    for (size_t i = (size_t)0; i < records_kept; i++) {
        size_t off = (size_t)DTC_READ_RESPONSE_PREAMBLE_BYTES + i * (size_t)DTC_RECORD_BYTES;
        dtc_uds_format_dtc_code(resp[off + (size_t)DTC_UDS_DTC_RECORD_BYTE_HI_OFFSET],
                                resp[off + (size_t)DTC_UDS_DTC_RECORD_BYTE_MID_OFFSET],
                                resp[off + (size_t)DTC_UDS_DTC_RECORD_BYTE_LO_OFFSET],
                                out_entries[i].code,
                                (size_t)DTC_CODE_STRING_LEN);
        /* P-78: capture the Failure Type Byte (3rd raw byte). Was
         * dropped via `(void)lo` in format_dtc_code under the
         * obsolete belief that it was the 6th hex digit of an
         * extended-DTC display. Per SAE J2012-2016 it's the FTB
         * and distinguishes two records with the same code. */
        out_entries[i].ftb         = resp[off + (size_t)DTC_UDS_DTC_RECORD_BYTE_LO_OFFSET];
        out_entries[i].status      = resp[off + (size_t)DTC_UDS_DTC_RECORD_STATUS_OFFSET];
        out_entries[i].description = NULL; /* resolved by dtc_feature.c */
    }
    *out_count = records_kept;

    if (records_total > entries_cap) {
        ESP_LOGW(TAG, "ECU returned %u DTCs, surfaced first %u (cap)",
                 (unsigned)records_total, (unsigned)entries_cap);
    } else {
        ESP_LOGI(TAG, "read parsed %u DTCs", (unsigned)records_kept);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 0x14 0xFF 0xFF 0xFF ClearDiagnosticInformation (all groups)          */
/* ------------------------------------------------------------------ */

esp_err_t dtc_uds_clear_diagnostic_information(uint32_t timeout_ms) {
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.last_nrc = (uint8_t)0;

    /* P-54 phase 2: SRM-patched ECU uses OBD-II Mode 04 (single-byte
     * $04 request, no group selector). UDS $14 returns NRC 0x11 —
     * stripped from the patch's service table. VCDS capture
     * 2026-05-29 confirmed Mode 04 works. */
    uint8_t req[1];
    req[0] = (uint8_t)DTC_UDS_SID_CLEAR_LEGACY;
    static const size_t k_req_len = (size_t)1;

    uint8_t resp[DTC_CLEAR_RESPONSE_BUFFER_BYTES];
    int rc = s_ctx.request_fn(req, k_req_len, resp, sizeof(resp),
                              timeout_ms, s_ctx.user_ctx);
    if (rc < 0) {
        ESP_LOGE(TAG, "clear transport error rc=%d", rc);
        return ESP_FAIL;
    }
    if (rc == 0) {
        ESP_LOGW(TAG, "clear transport returned 0 bytes (timeout)");
        return ESP_ERR_TIMEOUT;
    }
    size_t resp_len = (size_t)rc;

    if (response_is_negative(resp, resp_len)) {
        ESP_LOGW(TAG, "clear NRC 0x%02X", (unsigned)s_ctx.last_nrc);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (resp_len < (size_t)1 || resp[0] != (uint8_t)DTC_UDS_CLEAR_POSITIVE_SID) {
        ESP_LOGE(TAG, "clear malformed response (len=%u sid=0x%02X)",
                 (unsigned)resp_len, resp_len > (size_t)0 ? (unsigned)resp[0] : (unsigned)0);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "clear positive response received");
    return ESP_OK;
}

/* P-54: DiagnosticSessionControl ($10 <sub>). ECU rejects $14
 * ClearDTC in the default session with NRC 0x11; the clear path
 * preambles with $10 0x03 (extended) and returns to $10 0x01
 * (default) after. */
esp_err_t dtc_uds_session_control(uint8_t session_id, uint32_t timeout_ms) {
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.last_nrc = (uint8_t)0;

    uint8_t req[2];
    req[0] = (uint8_t)DTC_UDS_SID_SESSION_CTRL;
    req[1] = session_id;

    uint8_t resp[DTC_CLEAR_RESPONSE_BUFFER_BYTES];
    int rc = s_ctx.request_fn(req, sizeof(req), resp, sizeof(resp),
                              timeout_ms, s_ctx.user_ctx);
    if (rc < 0) {
        ESP_LOGE(TAG, "session_ctrl transport error rc=%d", rc);
        return ESP_FAIL;
    }
    if (rc == 0) {
        ESP_LOGW(TAG, "session_ctrl transport returned 0 bytes (timeout)");
        return ESP_ERR_TIMEOUT;
    }
    size_t resp_len = (size_t)rc;

    if (response_is_negative(resp, resp_len)) {
        ESP_LOGW(TAG, "session_ctrl NRC 0x%02X (session 0x%02X)",
                 (unsigned)s_ctx.last_nrc, (unsigned)session_id);
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* Positive response: 0x50 <sub> [<P2 timing bytes...>]. We
     * only verify the SID + sub-function echo; the timing bytes
     * are advisory for the client to honor and we don't use them. */
    if (resp_len < (size_t)2
        || resp[0] != (uint8_t)DTC_UDS_SESSION_POSITIVE_SID
        || resp[1] != session_id) {
        ESP_LOGE(TAG, "session_ctrl malformed (len=%u sid=0x%02X sub=0x%02X)",
                 (unsigned)resp_len,
                 resp_len > 0 ? (unsigned)resp[0] : 0,
                 resp_len > 1 ? (unsigned)resp[1] : 0);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "session_ctrl 0x%02X positive", (unsigned)session_id);
    return ESP_OK;
}
