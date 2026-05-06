#ifndef SBF_APPLIER_H
#define SBF_APPLIER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "sbf_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sbf_applier — runs the per-map blend + write loop for a loaded
 * SBF. The applier is host-testable because it depends only on:
 *
 *   - sbf_loader_iface_t  (already injectable)
 *   - sbf_apply_ecu_write_iface_t  (this header — wraps ecu_write_data
 *                                   so the host test mocks it)
 *
 * The applier never touches FreeRTOS, never blocks indefinitely, and
 * emits no I/O of its own. It returns elapsed_ms via the result
 * struct so the orchestrator can WS-emit apply_completed events with
 * the timing.
 */

typedef void (*sbf_apply_progress_fn_t)(uint32_t maps_done,
                                        uint32_t maps_total,
                                        void    *user_ctx);

typedef esp_err_t (*sbf_apply_write_fn_t)(uint32_t        ecu_address,
                                          const uint8_t  *bytes,
                                          size_t          byte_count,
                                          uint8_t         mid_byte,
                                          uint32_t        address_offset,
                                          uint32_t        per_write_timeout_ms,
                                          void           *user_ctx);

typedef uint32_t (*sbf_apply_clock_fn_t)(void *user_ctx);

typedef struct {
    /* Synchronous write to ECU. Blocks (with internal timeout) until
     * the ECU acknowledges the chunk. Returns ESP_OK on success,
     * ESP_ERR_TIMEOUT on per-write timeout, otherwise ESP_FAIL. */
    sbf_apply_write_fn_t   write;

    /* Monotonic ms clock. */
    sbf_apply_clock_fn_t   clock_now_ms;

    /* Progress callback fired every SBF_PROGRESS_EVENT_EVERY_N_MAPS
     * maps. NULL → no progress events. */
    sbf_apply_progress_fn_t progress;

    /* User context passed through to all callbacks. */
    void                  *user_ctx;
} sbf_applier_deps_t;

typedef struct {
    /* Set on success. */
    uint32_t maps_applied;
    uint32_t total_bytes_written;
    uint32_t elapsed_ms;
    /* On failure: maps_applied is the count completed; failure_reason
     * is a short string. */
    char     failure_reason[64];
} sbf_apply_result_t;

/*
 * Apply `loaded` to the ECU at the requested ethanol percent.
 *
 * loaded:        opaque handle from sbf_loader_iface_t.load.
 * loader:        the iface used to load the handle (so the applier
 *                can call its accessors).
 * ethanol_pct:   [0..100], clamped if out of range.
 * mid_byte/offset: write parameters from sbf_variants_lookup.
 * deps:          ecu_write + clock + optional progress + ctx.
 * out:           filled with maps_applied, elapsed_ms, etc.
 *
 * Returns ESP_OK on success, an error code on first per-map failure
 * (subsequent maps are not attempted).
 */
esp_err_t sbf_applier_apply(sbf_loaded_t              *loaded,
                            const sbf_loader_iface_t  *loader,
                            uint8_t                    ethanol_pct,
                            uint8_t                    mid_byte,
                            uint32_t                   address_offset,
                            const sbf_applier_deps_t  *deps,
                            sbf_apply_result_t        *out);

#ifdef __cplusplus
}
#endif

#endif /* SBF_APPLIER_H */
