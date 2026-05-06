#ifndef DTC_H
#define DTC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "dtc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dtc — Diagnostic Trouble Code (DTC) read/clear feature for FUTUNER.
 *
 * One feature, two operations: dtc_read() and dtc_clear() share UDS
 * session state and the description database, and both arbitrate
 * through feature_manager as FEATURE_DTC. Each operation is short-
 * lived (typical < 2 s) — request_start, do the UDS exchange,
 * request_stop. A long-running tuning session would never overlap
 * with these.
 *
 * UDS surface (CLAUDE.md §1 hard rule: standard services only):
 *   - 0x19 0x02 (reportDTCByStatusMask) for read
 *   - 0x14 with group 0xFFFFFF (ClearDiagnosticInformation, all
 *     groups) for clear
 *
 * Description lookup is keyed by ECU FAMILY (MG1, MDG1, MED17), not
 * per-variant. Family is settable at runtime via
 * dtc_feature_set_family(); it defaults to DTC_FAMILY_DEFAULT until
 * the per-variant manifest lands in Phase A.
 */

/* ------------------------------------------------------------------ */
/* Public types                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    DTC_ECU_FAMILY_MG1 = 0,
    DTC_ECU_FAMILY_MDG1,
    DTC_ECU_FAMILY_MED17,
    DTC_ECU_FAMILY_COUNT,
} dtc_ecu_family_t;

typedef struct {
    /* Five-character SAE J2012 DTC code, NUL-terminated. Example:
     * "P0420". DTC_CODE_STRING_LEN is the buffer size including NUL. */
    char        code[DTC_CODE_STRING_LEN];

    /* Raw status byte exactly as the ECU returned it. ISO 14229
     * semantics: bit 0 testFailed, bit 1 testFailedThisOperationCycle,
     * bit 2 pendingDTC, bit 3 confirmedDTC, bit 4
     * testNotCompletedSinceLastClear, bit 5 testFailedSinceLastClear,
     * bit 6 testNotCompletedThisOperationCycle, bit 7
     * warningIndicatorRequested. */
    uint8_t     status;

    /* Pointer to a static, NUL-terminated description string. Resolved
     * via the family-keyed lookup table at read time. Returns
     * "manufacturer-specific code (see scan tool)" for codes the
     * lookup does not cover. */
    const char *description;
} dtc_entry_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/*
 * Initialize the DTC feature module. On target this also wires the
 * UDS transport adapter (isotp_coordinator + can_manager). On host
 * builds (DTC_FEATURE_HOST_BUILD), the test harness configures the
 * transport via dtc_uds_init() directly.
 *
 * Must be called once at boot, AFTER feature_manager_init().
 * Idempotent.
 */
esp_err_t dtc_feature_init(void);

/*
 * Idempotent. Hands the DTC feature descriptor to
 * feature_manager_register(). Called from dtc_feature_init(); also
 * exposed for test scaffolding that wants to use a fresh
 * feature_manager instance without re-running the full init path.
 */
esp_err_t dtc_register_with_feature_manager(void);

/*
 * Reflects whether DTC is the currently-active feature. True only
 * during the brief window between feature_manager_request_start and
 * the matching request_stop. Used by feature_manager during preempt-
 * swap to know when stop() has fully completed.
 */
bool      dtc_feature_is_running(void);

/*
 * Switch the active ECU family used for description lookups. Returns
 * the family that was previously active, so callers can restore on
 * failure paths. Out-of-range families are silently coerced to
 * DTC_ECU_FAMILY_MG1.
 */
dtc_ecu_family_t dtc_feature_set_family(dtc_ecu_family_t family);

/* ------------------------------------------------------------------ */
/* High-level operations                                               */
/* ------------------------------------------------------------------ */

/*
 * Read DTCs from the ECU using UDS 0x19 0x02 (reportDTCByStatusMask).
 *
 * status_mask: which DTC status bits to filter on. Pass 0 to use
 *              DTC_DEFAULT_STATUS_MASK.
 * out_entries: caller-provided buffer of dtc_entry_t. Filled in with
 *              the codes the ECU returned, capped at entries_cap.
 * entries_cap: number of slots available in out_entries.
 * out_count:   on ESP_OK, set to the number of entries actually
 *              filled. On error, set to 0.
 * err_out/err_cap: optional human-readable error message buffer
 *              populated on failure (NRC, transport error, etc.).
 *
 * Behavior:
 *   - Calls feature_manager_request_start(FEATURE_DTC) so that no
 *     concurrent feature can race the bus. If another feature is
 *     active, the manager either preempts it or the request fails;
 *     this function propagates that outcome.
 *   - Issues the UDS request via the configured transport.
 *   - Always calls feature_manager_request_stop(FEATURE_DTC) before
 *     returning, regardless of outcome.
 */
esp_err_t dtc_read(uint8_t      status_mask,
                   dtc_entry_t *out_entries,
                   size_t       entries_cap,
                   size_t      *out_count,
                   char        *err_out,
                   size_t       err_cap);

/*
 * Clear all DTCs in the ECU using UDS 0x14 with group 0xFFFFFF.
 *
 * cleared_count_out: on ESP_OK, set to the number of DTCs that were
 *              active at the time of the clear (counted via a
 *              pre-clear UDS 0x19 0x02 read using
 *              DTC_DEFAULT_STATUS_MASK). UDS 0x14 itself does not
 *              return a count; the pre-read is the only way to
 *              produce a meaningful "how many did we clear" number
 *              for the WS response. May be NULL if the caller does
 *              not care.
 * err_out/err_cap: optional human-readable error message buffer.
 *
 * Behavior is the same start/stop arbitration pattern as dtc_read().
 */
esp_err_t dtc_clear(uint16_t *cleared_count_out,
                    char     *err_out,
                    size_t    err_cap);

#ifdef __cplusplus
}
#endif

#endif /* DTC_H */
