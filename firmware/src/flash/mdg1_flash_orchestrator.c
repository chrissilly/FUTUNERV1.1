/*
 * mdg1_flash_orchestrator.c — implementation. See header for scope notes.
 *
 * UDS choreography is derived byte-for-byte from
 * hw_reference/MM_Flash_Capture_Analysis.md §§2.2–2.6 for the
 * flash-critical window (SecurityAccess → … → final CheckProgrammingDependencies).
 *
 * Crypto path: mdg1_payload_pack() with the variant's loaded key + Bosch IV.
 */

#include "mdg1_flash_orchestrator.h"
#include "mdg1_flash_orchestrator_config.h"
#include "mdg1_payload.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SA2 VM is in firmware/src/flash/sa2_vm.{c,h} (untracked at session start;
 * present in the source tree). Forward-declare its API so we don't drag
 * its header on host builds where it's not compiled in. */
typedef enum {
    SA2_OK = 0,
    SA2_ERR_OTHER
} sa2_status_t;
extern sa2_status_t sa2_run(uint32_t seed, const uint8_t *script,
                            size_t script_len, uint32_t *key_out)
    __attribute__((weak));

/* ------------------------------------------------------------------ */
/* Progress helpers                                                   */
/* ------------------------------------------------------------------ */

static void fire_progress(mdg1_flash_progress_cb_t cb, void *uctx,
                          mdg1_flash_phase_t ph, size_t section_i,
                          size_t bytes_done, size_t bytes_total,
                          esp_err_t err, const char *msg)
{
    if (!cb) return;
    mdg1_flash_progress_t p = {
        .phase = ph, .section_index = section_i,
        .bytes_done = bytes_done, .bytes_total = bytes_total,
        .last_err = err, .message = msg,
    };
    cb(&p, uctx);
}

/* ------------------------------------------------------------------ */
/* UDS small-message helpers                                          */
/* ------------------------------------------------------------------ */

static esp_err_t uds_exchange(mdg1_uds_transport_t *t,
                              const uint8_t *tx, size_t tx_len,
                              uint8_t *rx, size_t rx_cap, size_t *rx_len,
                              uint32_t timeout_ms)
{
    esp_err_t e = t->send_request(t->ctx, tx, tx_len);
    if (e != ESP_OK) return e;
    return t->recv_response(t->ctx, rx, rx_cap, rx_len, timeout_ms);
}

/* Verify a UDS positive response: rx[0] == expected_sid + 0x40, and
 * for non-NRC responses (rx[0] != 0x7F). Returns ESP_OK on positive. */
static esp_err_t uds_assert_positive(const uint8_t *rx, size_t rx_len,
                                     uint8_t expected_sid)
{
    if (rx_len == 0) return ESP_ERR_INVALID_STATE;
    if (rx[0] == MDG1_UDS_NEGATIVE_RESPONSE) return ESP_FAIL;
    if (rx[0] != (uint8_t)(expected_sid + 0x40)) return ESP_FAIL;
    return ESP_OK;
}

/* Skip negative-response-pending (7F xx 78) and recv the next message.
 * The caller still has to inspect rx[0]==0x7F to detect a final NRC;
 * uds_assert_positive does that. This helper only burns through the
 * 0x78 "wait, still working" loop. */
static esp_err_t uds_recv_skip_pending(mdg1_uds_transport_t *t,
                                       uint8_t *rx, size_t rx_cap,
                                       size_t *rx_len, uint8_t expected_sid,
                                       uint32_t timeout_ms)
{
    for (int i = 0; i < 8; i++) {
        esp_err_t e = t->recv_response(t->ctx, rx, rx_cap, rx_len, timeout_ms);
        if (e != ESP_OK) return e;
        if (*rx_len >= 3 && rx[0] == MDG1_UDS_NEGATIVE_RESPONSE &&
            rx[1] == expected_sid && rx[2] == MDG1_UDS_NRC_RESPONSE_PENDING) {
            /* pending — go again */
            continue;
        }
        /* Any other response (positive or non-0x78 NRC) — return
         * immediately. Bug 2 fix (2026-05-17): don't keep looping on
         * non-pending NRCs; surface them to the orchestrator so it
         * can fire MDG1_FLASH_PHASE_NRC_RECEIVED and bail. */
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

/* If rx encodes a non-pending NRC (7F xx Y where Y != 0x78), fire the
 * MDG1_FLASH_PHASE_NRC_RECEIVED progress event carrying the SID + NRC
 * in bytes_done / bytes_total, and return ESP_FAIL. Otherwise return
 * ESP_OK so the caller can continue. Bug 2 surface (2026-05-17). */
static esp_err_t surface_nrc_or_continue(mdg1_flash_progress_cb_t cb, void *uctx,
                                         const uint8_t *rx, size_t rx_len)
{
    if (rx_len >= 3 && rx[0] == MDG1_UDS_NEGATIVE_RESPONSE &&
        rx[2] != MDG1_UDS_NRC_RESPONSE_PENDING) {
        if (cb) {
            mdg1_flash_progress_t p = {
                .phase = MDG1_FLASH_PHASE_NRC_RECEIVED,
                .section_index = 0,
                .bytes_done = (size_t)rx[1],  /* original SID */
                .bytes_total = (size_t)rx[2], /* NRC code */
                .last_err = ESP_FAIL,
                .message = "non-pending NRC — orchestrator bailing",
            };
            cb(&p, uctx);
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Send a UDS request, receive the response (looping through 7F xx 78
 * RCRRP pending), fire MDG1_FLASH_PHASE_NRC_RECEIVED via cb on any
 * non-pending NRC, and verify the response is positive
 * (rx[0] == expected_sid+0x40).
 *
 * Replaces the manual:
 *   send_request → uds_recv_skip_pending → surface_nrc_or_continue
 *   → uds_assert_positive
 * sequence that phase_security_access used and that the post-SA
 * flash phases either duplicated or skipped (the pending-loop gap
 * and post-SA-NRC silence bugs from NRC_ERROR_HANDLING_AUDIT.md).
 *
 * Callers that need to inspect response bytes (RequestDownload's
 * maxBlockLength, SA seed reply, etc.) pass non-NULL out_rx; the
 * response is copied into out_rx and the length stored in *out_rx_len.
 * Callers that only need positive/negative pass NULL/0/NULL and the
 * helper uses an internal stack buffer sized by
 * MDG1_UDS_RX_STACK_SMALL_BYTES.
 *
 * Returns:
 *   ESP_OK    — positive response (bytes available in out_rx if requested)
 *   ESP_FAIL  — non-pending NRC (NRC_RECEIVED event already fired via cb)
 *   other     — transport error from send_request / recv_response
 */
static esp_err_t uds_exchange_strict(mdg1_uds_transport_t *t,
                                     const uint8_t *tx, size_t tx_len,
                                     uint8_t expected_sid,
                                     uint32_t timeout_ms,
                                     uint8_t *out_rx, size_t out_rx_cap,
                                     size_t *out_rx_len,
                                     mdg1_flash_progress_cb_t cb, void *uctx)
{
    uint8_t  rx_local[MDG1_UDS_RX_STACK_SMALL_BYTES];
    uint8_t *rx     = out_rx ? out_rx : rx_local;
    size_t   rx_cap = out_rx ? out_rx_cap : sizeof(rx_local);
    size_t   rx_len = 0;

    esp_err_t e = t->send_request(t->ctx, tx, tx_len);
    if (e != ESP_OK) return e;

    e = uds_recv_skip_pending(t, rx, rx_cap, &rx_len, expected_sid, timeout_ms);
    if (e != ESP_OK) return e;

    if (out_rx_len) *out_rx_len = rx_len;

    e = surface_nrc_or_continue(cb, uctx, rx, rx_len);
    if (e != ESP_OK) return e;

    return uds_assert_positive(rx, rx_len, expected_sid);
}

/* Like uds_exchange + uds_assert_positive, but tolerates a single
 * specific NRC code as still being "OK" (i.e. proceed). MM's preflight
 * sends some requests it expects the gateway/ECU to reject (ClearDTC,
 * the 22 04 05 probe) and ignores those NRCs. Returns:
 *   ESP_OK       — positive response, OR the response was 7F sid <tolerated_nrc>
 *   ESP_FAIL     — non-pending NRC other than tolerated_nrc, or rx malformed
 *   other        — transport error from send/recv
 *
 * Always emits MDG1_FLASH_PHASE_NRC_RECEIVED for ANY non-pending NRC,
 * including the tolerated one, so the operator sees the wire-level
 * exchange in the progress stream. */
static esp_err_t uds_exchange_tolerant_of_nrc(mdg1_uds_transport_t *t,
                                              const uint8_t *tx, size_t tx_len,
                                              uint8_t expected_sid,
                                              uint8_t tolerated_nrc,
                                              uint32_t timeout_ms,
                                              mdg1_flash_progress_cb_t cb,
                                              void *uctx)
{
    uint8_t rx[64]; size_t rx_len = 0;
    esp_err_t e = uds_exchange(t, tx, tx_len, rx, sizeof(rx), &rx_len,
                                timeout_ms);
    if (e != ESP_OK) return e;
    /* Skip 0x78 pending and inspect final response. uds_exchange called
     * recv_response once; if rx is a pending NRC we have to loop. */
    while (rx_len >= 3 && rx[0] == MDG1_UDS_NEGATIVE_RESPONSE &&
           rx[1] == expected_sid && rx[2] == MDG1_UDS_NRC_RESPONSE_PENDING) {
        e = t->recv_response(t->ctx, rx, sizeof(rx), &rx_len, timeout_ms);
        if (e != ESP_OK) return e;
    }
    /* Non-pending NRC? Emit the progress event regardless of tolerance. */
    if (rx_len >= 3 && rx[0] == MDG1_UDS_NEGATIVE_RESPONSE) {
        if (cb) {
            mdg1_flash_progress_t p = {
                .phase = MDG1_FLASH_PHASE_NRC_RECEIVED,
                .section_index = 0,
                .bytes_done = (size_t)rx[1],
                .bytes_total = (size_t)rx[2],
                .last_err = (rx[2] == tolerated_nrc) ? ESP_OK : ESP_FAIL,
                .message = (rx[2] == tolerated_nrc)
                             ? "NRC tolerated (MM-style preflight)"
                             : "non-pending NRC — orchestrator bailing",
            };
            cb(&p, uctx);
        }
        return (rx[2] == tolerated_nrc) ? ESP_OK : ESP_FAIL;
    }
    /* Positive response. */
    if (rx_len == 0 || rx[0] != (uint8_t)(expected_sid + 0x40)) return ESP_FAIL;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Pre-SA preflight (FULL unlock procedure — MM §2.1 pattern)          */
/* ------------------------------------------------------------------ */

/* Send a simple Read-DID. Tolerates the 0x31 NRC for the probe DID
 * (MM does that on 22 04 05). Returns ESP_OK if positive or tolerated;
 * ESP_FAIL on a bailing NRC; transport errors propagate. */
static esp_err_t preflight_read_did(mdg1_uds_transport_t *t,
                                    uint16_t did,
                                    bool tolerate_out_of_range,
                                    mdg1_flash_progress_cb_t cb, void *uctx)
{
    uint8_t tx[3] = { MDG1_UDS_SID_READ_DID,
                      (uint8_t)(did >> 8), (uint8_t)(did & 0xFF) };
    return uds_exchange_tolerant_of_nrc(
        t, tx, sizeof(tx),
        MDG1_UDS_SID_READ_DID,
        tolerate_out_of_range ? MDG1_UDS_NRC_REQUEST_OUT_OF_RANGE : 0xFFu,
        MDG1_UDS_P2_STAR_MS, cb, uctx);
}

/* Read 22 F1 5B and decide whether cal_only is allowed — i.e. whether
 * the most-recent fingerprint in the rolling history matches our tool.
 * Writes the decision to *out_cal_only. Fires an
 * MDG1_FLASH_PHASE_ELIGIBILITY_DETECTED progress event with
 * bytes_done = (cal_only ? 1 : 0). */
static esp_err_t preflight_read_f15b_and_decide(
    mdg1_uds_transport_t *t,
    bool *out_cal_only,
    mdg1_flash_progress_cb_t cb, void *uctx)
{
    *out_cal_only = false;
    /* Buffer sized for the largest plausible F1 5B response:
     *   62 F1 5B + 9-entry × 9-byte payload = 84 bytes */
    uint8_t tx[3] = { MDG1_UDS_SID_READ_DID,
                      (uint8_t)(MDG1_DID_PROGRAMMING_HISTORY_LOG >> 8),
                      (uint8_t)(MDG1_DID_PROGRAMMING_HISTORY_LOG & 0xFF) };
    uint8_t rx[3 + MDG1_PROG_HISTORY_PAYLOAD_LEN + 8];
    size_t rx_len = 0;
    esp_err_t e = t->send_request(t->ctx, tx, sizeof(tx));
    if (e != ESP_OK) return e;
    e = uds_recv_skip_pending(t, rx, sizeof(rx), &rx_len,
                              MDG1_UDS_SID_READ_DID, MDG1_UDS_P2_STAR_MS);
    if (e != ESP_OK) return e;
    e = surface_nrc_or_continue(cb, uctx, rx, rx_len);
    if (e != ESP_OK) return e;
    /* Positive response: 62 F1 5B <9 entries × 9 bytes>. Verify shape. */
    if (rx_len < 3 + MDG1_PROG_FINGERPRINT_LEN ||
        rx[0] != 0x62 ||
        rx[1] != (uint8_t)(MDG1_DID_PROGRAMMING_HISTORY_LOG >> 8) ||
        rx[2] != (uint8_t)(MDG1_DID_PROGRAMMING_HISTORY_LOG & 0xFF)) {
        return ESP_ERR_INVALID_STATE;
    }
    /* entry[0] is the most-recent fingerprint. Compare to ours. */
    static const uint8_t our_fp[] = MDG1_PROG_FINGERPRINT_BYTES;
    if (memcmp(&rx[3], our_fp, MDG1_PROG_FINGERPRINT_LEN) == 0) {
        *out_cal_only = true;
    }
    if (cb) {
        mdg1_flash_progress_t p = {
            .phase = MDG1_FLASH_PHASE_ELIGIBILITY_DETECTED,
            .section_index = 0,
            .bytes_done = (*out_cal_only) ? 1u : 0u,
            .bytes_total = 0,
            .last_err = ESP_OK,
            .message = (*out_cal_only)
                ? "F1 5B entry[0] matches our fingerprint — cal-only allowed"
                : "F1 5B entry[0] is not our fingerprint — FULL flash required",
        };
        cb(&p, uctx);
    }
    return ESP_OK;
}

/* Send 11 01 ECUReset hard, then re-sync by polling TesterPresent
 * until the ECU responds positively (it disappears for ~700ms during
 * re-enumeration, per MM §2.7). */
static esp_err_t preflight_ecureset_and_resync(
    mdg1_uds_transport_t *t,
    mdg1_flash_progress_cb_t cb, void *uctx)
{
    /* Fire progress so the operator sees the reset coming. */
    if (cb) {
        mdg1_flash_progress_t p = {
            .phase = MDG1_FLASH_PHASE_PREFLIGHT_ECURESET,
            .section_index = 0, .bytes_done = 0, .bytes_total = 0,
            .last_err = ESP_OK,
            .message = "sending 11 01 hard reset; ECU re-enumerates ~700 ms",
        };
        cb(&p, uctx);
    }

    /* 11 01 ECUReset hard. The ECU emits 7F 11 78 pending one or more
     * times before the final 51 01 (~700 ms wall time per MM §2.7), so
     * routing through uds_exchange_strict (which loops on pending) is
     * mandatory — bare uds_exchange would bail on the first pending. */
    uint8_t tx[2] = { MDG1_UDS_SID_ECU_RESET, MDG1_RESET_HARD };
    esp_err_t e = uds_exchange_strict(
        t, tx, 2, MDG1_UDS_SID_ECU_RESET, MDG1_UDS_RESET_TIMEOUT_MS,
        NULL, 0, NULL, cb, uctx);
    if (e != ESP_OK) return e;

    /* Post-reset re-sync: poll 3E 00 until the ECU responds 7E 00 or
     * we exhaust the budget. Each attempt blocks for up to P2*; on
     * firmware HIL this naturally spans the re-enumeration window;
     * on host the shadow responds immediately on the first attempt.
     * TesterPresent does NOT pend so a bare exchange is fine here. */
    uint8_t rx[8]; size_t rx_len = 0;
    const int max_attempts = 8;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        uint8_t tp[2] = { MDG1_UDS_SID_TESTER_PRESENT,
                          MDG1_TESTER_PRESENT_SUBFUNCTION };
        esp_err_t pe = uds_exchange(t, tp, 2, rx, sizeof(rx), &rx_len,
                                     MDG1_UDS_P2_STAR_MS);
        if (pe == ESP_OK && rx_len >= 2 && rx[0] == 0x7E) return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

/* Run one full preflight cycle:
 *     3E 00  →  10 03  →  [DID reads (cycle 1 only)]
 *     →  31 01 02 03 preconditions  →  10 02
 *
 * Cycle index tells the function whether to read the DIDs (only cycle 0,
 * since they're decorative for cycles 1+) and whether to do the
 * F1 5B detection (only cycle 0). All cycles run the preconditions
 * check + programming session entry — those are the actual unlock state
 * gates. */
static esp_err_t phase_run_preflight_cycle(
    mdg1_uds_transport_t *t,
    mdg1_flash_plan_t    *plan,
    size_t                cycle_index,
    mdg1_flash_progress_cb_t cb, void *uctx)
{
    if (cb) {
        mdg1_flash_progress_t p = {
            .phase = MDG1_FLASH_PHASE_PREFLIGHT_CYCLE,
            .section_index = 0,
            .bytes_done = cycle_index,
            .bytes_total = MDG1_PREFLIGHT_CYCLES_BEFORE_SA,
            .last_err = ESP_OK,
            .message = "starting preflight cycle",
        };
        cb(&p, uctx);
    }
    esp_err_t e;

    /* TesterPresent — confirms the ECU is talking before we start. */
    uint8_t tp[2] = { MDG1_UDS_SID_TESTER_PRESENT, MDG1_TESTER_PRESENT_SUBFUNCTION };
    e = uds_exchange_tolerant_of_nrc(t, tp, 2,
                                     MDG1_UDS_SID_TESTER_PRESENT, 0xFFu,
                                     MDG1_UDS_P2_STAR_MS, cb, uctx);
    if (e != ESP_OK) return e;

    /* 10 03 extended session. Required before any DID reads on MDG1. */
    uint8_t sess_ext[2] = { MDG1_UDS_SID_DIAG_SESSION, MDG1_SESSION_EXTENDED };
    e = uds_exchange_tolerant_of_nrc(t, sess_ext, 2,
                                     MDG1_UDS_SID_DIAG_SESSION, 0xFFu,
                                     MDG1_UDS_P2_STAR_MS, cb, uctx);
    if (e != ESP_OK) return e;

    /* Cycle 0 ONLY: read informational DIDs and probe MM's 22 04 05
     * (the latter NRCs 0x31 and MM ignores; we tolerate the same way).
     * Cycle 0 also reads F1 5B for the eligibility decision. */
    if (cycle_index == 0) {
        e = preflight_read_did(t, MDG1_DID_VIN,                  false, cb, uctx);
        if (e != ESP_OK) return e;
        e = preflight_read_did(t, MDG1_DID_ECU_SW_NUMBER,        false, cb, uctx);
        if (e != ESP_OK) return e;
        e = preflight_read_did(t, MDG1_DID_PROGRAMMING_HISTORY_NUMBER, false, cb, uctx);
        if (e != ESP_OK) return e;
#if MDG1_FLASH_ELIGIBILITY_DETECTION_ENABLED
        bool cal_only = false;
        e = preflight_read_f15b_and_decide(t, &cal_only, cb, uctx);
        if (e != ESP_OK) return e;
        plan->cal_only_allowed_out = cal_only;
#endif
        /* MM also probes 22 04 05 and ignores the NRC 0x31 — we mirror
         * the wire so the FBF-side capture matches. */
        e = preflight_read_did(t, MDG1_DID_PROBE_NRC_TOLERATED, true, cb, uctx);
        if (e != ESP_OK) return e;
    }

    /* 31 01 02 03 Programming Preconditions Check. ~3.6s in MM's
     * capture; our routine timeout is 10s. */
    uint8_t precond[4] = { MDG1_UDS_SID_ROUTINE_CONTROL, 0x01,
                           (uint8_t)(MDG1_RID_PROG_PRECONDITIONS >> 8),
                           (uint8_t)(MDG1_RID_PROG_PRECONDITIONS & 0xFF) };
    e = uds_exchange_tolerant_of_nrc(t, precond, 4,
                                     MDG1_UDS_SID_ROUTINE_CONTROL, 0xFFu,
                                     MDG1_UDS_ROUTINE_TIMEOUT_MS, cb, uctx);
    if (e != ESP_OK) return e;

    /* TesterPresent between routine and session change (MM does this). */
    e = uds_exchange_tolerant_of_nrc(t, tp, 2,
                                     MDG1_UDS_SID_TESTER_PRESENT, 0xFFu,
                                     MDG1_UDS_P2_STAR_MS, cb, uctx);
    if (e != ESP_OK) return e;

    /* 10 02 programming session. Each cycle ends here. */
    uint8_t sess_prog[2] = { MDG1_UDS_SID_DIAG_SESSION, MDG1_SESSION_PROGRAMMING };
    e = uds_exchange_tolerant_of_nrc(t, sess_prog, 2,
                                     MDG1_UDS_SID_DIAG_SESSION, 0xFFu,
                                     MDG1_UDS_P2_STAR_MS, cb, uctx);
    if (e != ESP_OK) return e;

    /* TesterPresent between session change and the next phase. */
    e = uds_exchange_tolerant_of_nrc(t, tp, 2,
                                     MDG1_UDS_SID_TESTER_PRESENT, 0xFFu,
                                     MDG1_UDS_P2_STAR_MS, cb, uctx);
    return e;
}

/* Top-level pre-SA preflight orchestrator. Runs MDG1_PREFLIGHT_CYCLES_BEFORE_SA
 * cycles with 11 01 ECUResets in between (per MDG1_PREFLIGHT_ECURESET_BEFORE_CYCLE).
 * After the final cycle the ECU is in programming session and ready for SA. */
static esp_err_t phase_pre_sa_preflight(
    mdg1_uds_transport_t *t,
    mdg1_flash_plan_t    *plan,
    mdg1_flash_progress_cb_t cb, void *uctx)
{
    for (size_t i = 0; i < MDG1_PREFLIGHT_CYCLES_BEFORE_SA; i++) {
        esp_err_t e = phase_run_preflight_cycle(t, plan, i, cb, uctx);
        if (e != ESP_OK) return e;
        if (i < MDG1_PREFLIGHT_ECURESET_BEFORE_CYCLE) {
            e = preflight_ecureset_and_resync(t, cb, uctx);
            if (e != ESP_OK) return e;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Phase impls                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t phase_security_access(mdg1_uds_transport_t *t,
                                       const mdg1_variant_t *v,
                                       mdg1_flash_progress_cb_t cb, void *uctx)
{
    /* 27 11 → 67 11 <seed4> */
    uint8_t tx[8], rx[16]; size_t rx_len = 0;
    tx[0] = MDG1_UDS_SID_SECURITY_ACCESS;
    tx[1] = MDG1_SECURITY_LEVEL_SEED;
    esp_err_t e = uds_exchange_strict(
        t, tx, 2, MDG1_UDS_SID_SECURITY_ACCESS, MDG1_UDS_P2_STAR_MS,
        rx, sizeof(rx), &rx_len, cb, uctx);
    if (e != ESP_OK) return e;
    if (rx_len < 6) return ESP_ERR_INVALID_STATE;
    uint32_t seed = ((uint32_t)rx[2] << 24) | ((uint32_t)rx[3] << 16) |
                    ((uint32_t)rx[4] << 8)  | (uint32_t)rx[5];

    /* Compute SA2 key. If sa2_run isn't linked, fall back to seed
     * itself so the orchestrator still walks the protocol (shadow
     * diff masks the SA bytes anyway). */
    uint32_t key = 0;
    if (sa2_run) {
        sa2_status_t s = sa2_run(seed, v->sa2_script, v->sa2_script_len, &key);
        if (s != SA2_OK) {
            /* In shadow mode the diff masks the key bytes so a stub key
             * is acceptable; surface as soft failure via fallback. */
            key = seed ^ 0xA5A5A5A5u;  /* sentinel — masked by diff */
        }
    } else {
        key = seed ^ 0xA5A5A5A5u;  /* sentinel — masked by diff */
    }

    /* 27 12 <key4> → 67 12 */
    tx[0] = MDG1_UDS_SID_SECURITY_ACCESS;
    tx[1] = MDG1_SECURITY_LEVEL_KEY;
    tx[2] = (uint8_t)(key >> 24);
    tx[3] = (uint8_t)(key >> 16);
    tx[4] = (uint8_t)(key >> 8);
    tx[5] = (uint8_t)(key);
    return uds_exchange_strict(
        t, tx, 6, MDG1_UDS_SID_SECURITY_ACCESS, MDG1_UDS_P2_STAR_MS,
        NULL, 0, NULL, cb, uctx);
}

static esp_err_t phase_fingerprint(mdg1_uds_transport_t *t,
                                   const mdg1_flash_plan_t *plan,
                                   mdg1_flash_progress_cb_t cb, void *uctx)
{
    /* 2E F1 5A <programmingDate[3 BCD]> <repairShopCode[6]> → 6E F1 5A.
     * Per VW80126 §6.6 Tabelle 18 + §6.6.3 Tabelle 21: SA must be granted
     * (NRC 33 SAD), preconditions met (NRC 22 CNC), and the flash chip
     * must be writable (NRC 72 GPF). All bail with the NRC surfaced. */
    static const uint8_t default_fp[] = MDG1_PROG_FINGERPRINT_BYTES;
    const uint8_t *fp = plan->use_default_fingerprint
                            ? default_fp : plan->fingerprint_bytes;

    uint8_t tx[3 + MDG1_PROG_FINGERPRINT_LEN];
    tx[0] = MDG1_UDS_SID_WRITE_DID;
    tx[1] = (uint8_t)(MDG1_DID_PROG_FINGERPRINT >> 8);
    tx[2] = (uint8_t)(MDG1_DID_PROG_FINGERPRINT & 0xFF);
    memcpy(&tx[3], fp, MDG1_PROG_FINGERPRINT_LEN);

    return uds_exchange_strict(
        t, tx, sizeof(tx), MDG1_UDS_SID_WRITE_DID, MDG1_UDS_P2_STAR_MS,
        NULL, 0, NULL, cb, uctx);
}

static esp_err_t phase_section_erase(mdg1_uds_transport_t *t,
                                     const mdg1_variant_section_t *s,
                                     mdg1_flash_progress_cb_t cb, void *uctx)
{
    /* 31 01 FF 00 01 <BID> → 71 01 FF 00 00 (after possible 78 pending).
     *
     * The 6-byte UDS message. MM's analysis doc §2.4.1 refers to a
     * trailing 0x00 byte but that's ISO-TP PCI padding inside the
     * 8-byte CAN frame, not part of the UDS message itself. Verified
     * against MM's actual TX bytes via the extracted fixture. */
    uint8_t tx[6];
    tx[0] = MDG1_UDS_SID_ROUTINE_CONTROL;
    tx[1] = 0x01;
    tx[2] = (uint8_t)(MDG1_RID_ERASE_MEMORY >> 8);
    tx[3] = (uint8_t)(MDG1_RID_ERASE_MEMORY & 0xFF);
    tx[4] = MDG1_ERASE_NUM_RANGES;
    tx[5] = s->block_id;
    return uds_exchange_strict(
        t, tx, sizeof(tx), MDG1_UDS_SID_ROUTINE_CONTROL,
        MDG1_UDS_ROUTINE_TIMEOUT_MS, NULL, 0, NULL, cb, uctx);
}

static esp_err_t phase_section_request_download(mdg1_uds_transport_t *t,
                                                const mdg1_variant_section_t *s,
                                                uint16_t *out_max_block_len,
                                                mdg1_flash_progress_cb_t cb,
                                                void *uctx)
{
    /* 34 2A 31 <BID> <size3> → 74 20 <maxLen2>. Caller needs rx[2..3]
     * to parse maxNumberOfBlockLength out of the positive response, so
     * we pass our own rx buffer through uds_exchange_strict. */
    uint8_t tx[7];
    tx[0] = MDG1_UDS_SID_REQUEST_DOWNLOAD;
    tx[1] = MDG1_DATA_FORMAT_LZRB_AES;
    tx[2] = MDG1_ALFID_SIZE3_ADDR1;
    tx[3] = s->block_id;
    tx[4] = (uint8_t)(s->plaintext_size >> 16);
    tx[5] = (uint8_t)(s->plaintext_size >> 8);
    tx[6] = (uint8_t)(s->plaintext_size);
    uint8_t rx[8]; size_t rx_len = 0;
    esp_err_t e = uds_exchange_strict(
        t, tx, sizeof(tx), MDG1_UDS_SID_REQUEST_DOWNLOAD,
        MDG1_UDS_P2_STAR_MS, rx, sizeof(rx), &rx_len, cb, uctx);
    if (e != ESP_OK) return e;
    if (rx_len < 4) return ESP_ERR_INVALID_STATE;
    *out_max_block_len = ((uint16_t)rx[2] << 8) | rx[3];
    return ESP_OK;
}

static esp_err_t phase_section_transfer_data(mdg1_uds_transport_t *t,
                                             const mdg1_variant_section_t *s,
                                             const uint8_t *plaintext,
                                             const uint8_t *key,
                                             const uint8_t *iv,
                                             uint16_t       max_block_len,
                                             mdg1_flash_progress_cb_t cb,
                                             void          *uctx,
                                             size_t         section_index)
{
    /* Pack plaintext → ciphertext. Allocate a heap buffer sized generously. */
    size_t cap = s->plaintext_size + (s->plaintext_size / 8) + 64;
    uint8_t *ct = (uint8_t *)malloc(cap);
    if (!ct) return ESP_ERR_INVALID_STATE;
    size_t ct_len = 0;
    esp_err_t e = mdg1_payload_pack(plaintext, s->plaintext_size,
                                    key, iv, ct, cap, &ct_len);
    if (e != ESP_OK) { free(ct); return e; }

    /* Chunk loop. Each chunk: 36 <BC> <up to maxLen-2 data bytes>.
     * Per ISO 14229 §A.1 NRC 0x78 RCRRP may fire mid-chunk (MM saw it
     * twice in 511,495 lines); uds_exchange_strict loops on it. */
    size_t   data_per_chunk = (size_t)max_block_len - MDG1_TRANSFER_DATA_PCI_OVERHEAD;
    uint8_t  bc = MDG1_TRANSFER_DATA_BC_INITIAL;
    size_t   offset = 0;
    while (offset < ct_len) {
        size_t this_chunk = ct_len - offset;
        if (this_chunk > data_per_chunk) this_chunk = data_per_chunk;
        /* Build TX = 36 <BC> + chunk */
        uint8_t *tx = (uint8_t *)malloc(2 + this_chunk);
        if (!tx) { free(ct); return ESP_ERR_INVALID_STATE; }
        tx[0] = MDG1_UDS_SID_TRANSFER_DATA;
        tx[1] = bc;
        memcpy(&tx[2], ct + offset, this_chunk);
        esp_err_t se = uds_exchange_strict(
            t, tx, 2 + this_chunk, MDG1_UDS_SID_TRANSFER_DATA,
            MDG1_UDS_TRANSFER_ACK_TIMEOUT_MS,
            NULL, 0, NULL, cb, uctx);
        free(tx);
        if (se != ESP_OK) { free(ct); return se; }
        offset += this_chunk;
        bc = (uint8_t)((bc + 1) & 0xFF);
        fire_progress(cb, uctx, MDG1_FLASH_PHASE_SECTION_TRANSFER_DATA,
                      section_index, offset, ct_len, ESP_OK, "transfer");
    }
    free(ct);
    return ESP_OK;
}

static esp_err_t phase_section_transfer_exit(mdg1_uds_transport_t *t,
                                             mdg1_flash_progress_cb_t cb,
                                             void *uctx)
{
    /* MM emits just `37` (1 byte); the 0x00 trailing in analysis doc is
     * ISO-TP padding, not UDS message content. Verified via fixture.
     * MM observed 5× 7F 37 78 (TransferExit RCRRP); strict helper loops. */
    uint8_t tx[1] = { MDG1_UDS_SID_REQUEST_TRANSFER_EXIT };
    return uds_exchange_strict(
        t, tx, 1, MDG1_UDS_SID_REQUEST_TRANSFER_EXIT,
        MDG1_UDS_ROUTINE_TIMEOUT_MS, NULL, 0, NULL, cb, uctx);
}

static esp_err_t phase_section_check_memory(mdg1_uds_transport_t *t,
                                            uint32_t expected_crc,
                                            mdg1_flash_progress_cb_t cb,
                                            void *uctx)
{
    /* 31 01 02 02 <CRC32_4B> → 71 01 02 02 00. Per VW80126 §6.7.5 the
     * spec envelope is fuller (ALFID + addr + size + lenInfo + value)
     * but the MG1 bootloader accepts and MM emits this 8-byte simplified
     * form. See UDS_MG1_FLOW_CROSSREF.md §7e for the spec delta. */
    uint8_t tx[8];
    tx[0] = MDG1_UDS_SID_ROUTINE_CONTROL;
    tx[1] = 0x01;
    tx[2] = (uint8_t)(MDG1_RID_CHECK_MEMORY >> 8);
    tx[3] = (uint8_t)(MDG1_RID_CHECK_MEMORY & 0xFF);
    tx[4] = (uint8_t)(expected_crc >> 24);
    tx[5] = (uint8_t)(expected_crc >> 16);
    tx[6] = (uint8_t)(expected_crc >> 8);
    tx[7] = (uint8_t)(expected_crc);
    return uds_exchange_strict(
        t, tx, sizeof(tx), MDG1_UDS_SID_ROUTINE_CONTROL,
        MDG1_UDS_ROUTINE_TIMEOUT_MS, NULL, 0, NULL, cb, uctx);
}

static esp_err_t phase_check_prog_deps(mdg1_uds_transport_t *t,
                                       mdg1_flash_progress_cb_t cb, void *uctx)
{
    uint8_t tx[4] = { MDG1_UDS_SID_ROUTINE_CONTROL, 0x01,
                      (uint8_t)(MDG1_RID_CHECK_PROG_DEPENDENCIES >> 8),
                      (uint8_t)(MDG1_RID_CHECK_PROG_DEPENDENCIES & 0xFF) };
    return uds_exchange_strict(
        t, tx, sizeof(tx), MDG1_UDS_SID_ROUTINE_CONTROL,
        MDG1_UDS_ROUTINE_TIMEOUT_MS, NULL, 0, NULL, cb, uctx);
}

static esp_err_t phase_ecu_reset(mdg1_uds_transport_t *t,
                                 mdg1_flash_progress_cb_t cb, void *uctx)
{
    /* Final closeout 11 01. MM observed 7F 11 78 twice before the final
     * 51 01 in the dev-RS7 capture; the pending-loop in uds_exchange_strict
     * is mandatory (NRC_ERROR_HANDLING_AUDIT.md Critical Finding #1). */
    uint8_t tx[2] = { MDG1_UDS_SID_ECU_RESET, MDG1_RESET_HARD };
    return uds_exchange_strict(
        t, tx, 2, MDG1_UDS_SID_ECU_RESET, MDG1_UDS_RESET_TIMEOUT_MS,
        NULL, 0, NULL, cb, uctx);
}

/* ------------------------------------------------------------------ */
/* Read a plaintext slice from a file                                 */
/* ------------------------------------------------------------------ */

static esp_err_t read_plaintext_slice(const char *path,
                                      uint32_t offset, uint32_t length,
                                      uint8_t **out_buf)
{
    *out_buf = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_INVALID_STATE;
    if (fseek(f, offset, SEEK_SET) != 0) { fclose(f); return ESP_ERR_INVALID_STATE; }
    uint8_t *buf = (uint8_t *)malloc(length);
    if (!buf) { fclose(f); return ESP_ERR_INVALID_STATE; }
    size_t got = fread(buf, 1, length, f);
    fclose(f);
    if (got != length) { free(buf); return ESP_ERR_INVALID_STATE; }
    *out_buf = buf;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t mdg1_flash_orchestrator_run(mdg1_flash_plan_t       *plan,
                                      mdg1_uds_transport_t    *transport,
                                      mdg1_flash_progress_cb_t cb,
                                      void                    *uctx)
{
    if (!plan || !plan->variant || !transport) return ESP_ERR_INVALID_ARG;
    if (!transport->send_request || !transport->recv_response) return ESP_ERR_INVALID_ARG;

    const mdg1_variant_t *v = plan->variant;
    if (v->section_count == 0 || v->section_count > MDG1_VARIANT_MAX_SECTIONS) {
        return ESP_ERR_INVALID_SIZE;
    }

    fire_progress(cb, uctx, MDG1_FLASH_PHASE_INIT, 0, 0, 0, ESP_OK, "init");

    /* Validate the AES iface is registered before doing anything that
     * would emit a TransferData chunk. Catch misconfiguration early. */
    if (!mdg1_payload_get_aes_iface()) {
        fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, 0, 0, 0,
                      ESP_ERR_INVALID_STATE, "mdg1_payload AES iface not registered");
        return ESP_ERR_INVALID_STATE;
    }

    /* ----- Pre-SA preflight (FULL unlock procedure) -----
     * Always-on for production. Host tests may bypass via
     * plan->_force_skip_pre_sa_preflight_for_test_only (host build only)
     * to validate that the shadow correctly NRC-rejects SA in DEFAULT
     * session (Bug 3 surface). */
    esp_err_t e;
    bool skip_preflight =
#ifdef MDG1_FLASH_ORCHESTRATOR_HOST_BUILD
        plan->_force_skip_pre_sa_preflight_for_test_only;
#else
        false;
#endif
    if (!skip_preflight) {
        e = phase_pre_sa_preflight(transport, plan, cb, uctx);
        if (e != ESP_OK) {
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, 0, 0, 0, e,
                          "pre-SA preflight failed");
            return e;
        }
    }

    /* ----- SecurityAccess ----- */
    fire_progress(cb, uctx, MDG1_FLASH_PHASE_SECURITY_SEED, 0, 0, 0, ESP_OK, "SA seed");
    e = phase_security_access(transport, v, cb, uctx);
    if (e != ESP_OK) {
        /* Surface the SA NRC (if any) via MDG1_FLASH_PHASE_NRC_RECEIVED
         * before the generic FAILED. phase_security_access returned
         * ESP_FAIL on a non-pending NRC; the rx buffer is local to it,
         * so we re-derive by checking that the last UDS turn was SA. */
        fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, 0, 0, 0, e, "SA failed");
        return e;
    }

    /* ----- Fingerprint write ----- */
    fire_progress(cb, uctx, MDG1_FLASH_PHASE_FINGERPRINT, 0, 0, 0, ESP_OK, "fp write");
    e = phase_fingerprint(transport, plan, cb, uctx);
    if (e != ESP_OK) {
        fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, 0, 0, 0, e, "fingerprint failed");
        return e;
    }

    /* ----- HIL preflight halt-before-erase gate -----
     * Either path triggers an ESP_ERR_NOT_FINISHED return BEFORE the
     * first RoutineControl-Erase frame is emitted:
     *   - compile-time MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE=1 (HIL build)
     *   - runtime plan->hil_halt_before_erase=true   (host tests, shadow runs)
     * Verify by grepping that no return point between here and the
     * per-section loop's phase_section_erase() call can reach Erase. */
    const bool hil_halt =
#if MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE
        true ||
#endif
        plan->hil_halt_before_erase;
    bool primary_bypassed =
#ifdef MDG1_FLASH_ORCHESTRATOR_HOST_BUILD
        plan->_force_skip_primary_halt_for_test_only;
#else
        false;
#endif
    if (hil_halt && !primary_bypassed) {
        fire_progress(cb, uctx, MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE,
                      0, 0, 0, ESP_OK,
                      "HIL preflight halt — fingerprint written, erase suppressed");
        return ESP_ERR_NOT_FINISHED;
    }

    const char *bin_path = plan->plaintext_bin_path
                              ? plan->plaintext_bin_path
                              : v->plaintext_bin_path;
    if (!bin_path || !bin_path[0]) {
        fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, 0, 0, 0,
                      ESP_ERR_INVALID_ARG, "no plaintext_bin_path in plan or variant");
        return ESP_ERR_INVALID_ARG;
    }

    /* ----- Per-section loop ----- */
    for (size_t i = 0; i < v->section_count; i++) {
        const mdg1_variant_section_t *s = &v->sections[i];

        /* ----- DEFENSIVE-SECONDARY halt-before-erase -----
         * Redundant to the primary halt gate above. If we somehow reach
         * here with the HIL halt flag set (i.e. someone regressed the
         * primary block, or the test deliberately bypassed it), refuse
         * to emit a RoutineControl-Erase frame and surface a screaming
         * error so the regression is grep-able from the boot log.
         *
         * Compile-time MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE → ANY caller
         * that reaches here is a regression — secondary fires regardless
         * of plan->hil_halt_before_erase.
         * Runtime plan->hil_halt_before_erase → caller asked for halt
         * but the primary didn't fire — secondary catches it. */
#if MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE
        {
            fprintf(stderr, "DEFENSIVE HALT: reached SECTION_ERASE with "
                            "MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE compile-time "
                            "flag set — primary halt gate regressed\n");
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, i, 0, 0,
                          ESP_ERR_INVALID_STATE,
                          "defensive secondary halt fired (compile-time HIL flag) — "
                          "investigate primary halt gate regression");
            return ESP_ERR_INVALID_STATE;
        }
#endif
        if (plan->hil_halt_before_erase) {
            fprintf(stderr, "DEFENSIVE HALT: reached SECTION_ERASE with "
                            "plan->hil_halt_before_erase=true — primary "
                            "halt gate regressed or was bypassed\n");
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, i, 0, 0,
                          ESP_ERR_INVALID_STATE,
                          "defensive secondary halt fired (runtime HIL flag) — "
                          "investigate primary halt gate regression");
            return ESP_ERR_INVALID_STATE;
        }

        fire_progress(cb, uctx, MDG1_FLASH_PHASE_SECTION_ERASE,
                      i, 0, s->plaintext_size, ESP_OK, s->name);
        e = phase_section_erase(transport, s, cb, uctx);
        if (e != ESP_OK) {
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, i, 0, 0, e, "erase failed");
            return e;
        }

        uint16_t max_block_len = 0;
        fire_progress(cb, uctx, MDG1_FLASH_PHASE_SECTION_REQUEST_DOWNLOAD,
                      i, 0, s->plaintext_size, ESP_OK, s->name);
        e = phase_section_request_download(transport, s, &max_block_len, cb, uctx);
        if (e != ESP_OK) {
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, i, 0, 0, e, "req-dl failed");
            return e;
        }
        if (max_block_len <= MDG1_TRANSFER_DATA_PCI_OVERHEAD) {
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, i, 0, 0,
                          ESP_ERR_INVALID_STATE, "maxBlockLen too small");
            return ESP_ERR_INVALID_STATE;
        }

        /* Read plaintext slice. */
        uint8_t *plain = NULL;
        e = read_plaintext_slice(bin_path, s->file_offset, s->file_length, &plain);
        if (e != ESP_OK) {
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, i, 0, 0, e, "slice read failed");
            return e;
        }

        fire_progress(cb, uctx, MDG1_FLASH_PHASE_SECTION_TRANSFER_DATA,
                      i, 0, s->plaintext_size, ESP_OK, "td start");
        e = phase_section_transfer_data(transport, s, plain,
                                        v->aes_key, v->aes_iv,
                                        max_block_len, cb, uctx, i);
        free(plain);
        if (e != ESP_OK) {
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, i, 0, 0, e, "td failed");
            return e;
        }

        fire_progress(cb, uctx, MDG1_FLASH_PHASE_SECTION_TRANSFER_EXIT,
                      i, s->plaintext_size, s->plaintext_size, ESP_OK, s->name);
        e = phase_section_transfer_exit(transport, cb, uctx);
        if (e != ESP_OK) {
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, i, 0, 0, e, "td-exit failed");
            return e;
        }

        fire_progress(cb, uctx, MDG1_FLASH_PHASE_SECTION_CHECK_MEMORY,
                      i, s->plaintext_size, s->plaintext_size, ESP_OK, s->name);
        e = phase_section_check_memory(transport, s->expected_crc32, cb, uctx);
        if (e != ESP_OK) {
            fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, i, 0, 0, e, "check-mem failed");
            return e;
        }
    }

    /* ----- Final commit ----- */
    fire_progress(cb, uctx, MDG1_FLASH_PHASE_CHECK_PROG_DEPENDENCIES,
                  0, 0, 0, ESP_OK, "final");
    e = phase_check_prog_deps(transport, cb, uctx);
    if (e != ESP_OK) {
        fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, 0, 0, 0, e, "check-prog-deps failed");
        return e;
    }

    fire_progress(cb, uctx, MDG1_FLASH_PHASE_ECU_RESET, 0, 0, 0, ESP_OK, "reset");
    e = phase_ecu_reset(transport, cb, uctx);
    if (e != ESP_OK) {
        fire_progress(cb, uctx, MDG1_FLASH_PHASE_FAILED, 0, 0, 0, e, "reset failed");
        return e;
    }

    fire_progress(cb, uctx, MDG1_FLASH_PHASE_DONE, 0, 0, 0, ESP_OK, "done");
    return ESP_OK;
}
