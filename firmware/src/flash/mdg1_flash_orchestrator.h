#ifndef MDG1_FLASH_ORCHESTRATOR_H
#define MDG1_FLASH_ORCHESTRATOR_H

/*
 * mdg1_flash_orchestrator — MDG1 5-section full-flash sequencer.
 *
 * Drives the per-section UDS choreography (SecurityAccess → fingerprint
 * → 5×{Erase, RequestDownload, TransferData, TransferExit, CheckMemory}
 * → CheckProgrammingDependencies → ECUReset). Transport-agnostic via
 * mdg1_uds_transport_t.
 *
 * SCOPE NOTE FOR THIS PROMPT (validated end-to-end against MM in shadow
 * mode; not yet wired into feature_manager or the WS layer):
 *
 *   - Orchestrator emits the FLASH-CRITICAL UDS stream starting at
 *     SecurityAccess seed-request. MM's 30-DID preflight discovery
 *     dance is NOT reproduced (those reads are server identification,
 *     not flash protocol). The diff tool compares the flash-critical
 *     window, not the full MM capture.
 *
 *   - Plan struct is consumed read-only. Caller owns lifetime.
 *
 *   - mdg1_flash.c's ad-hoc AES path is NOT replaced in this prompt
 *     (deferred to the wire-up prompt that also registers with
 *     feature_manager). The orchestrator uses mdg1_payload_pack()
 *     directly.
 *
 *   - mdg1_aes_mbedtls.c is built but registered manually by callers.
 *     A wire-up prompt later calls mdg1_aes_mbedtls_register() at boot.
 *
 * SA2 seed-to-key: defers to the existing sa2_vm.c (untracked but
 * present in the source tree). If sa2_vm.{c,h} are not in the build,
 * orchestrator falls back to ESP_ERR_NOT_SUPPORTED on SA — surfaced in
 * the progress callback as "sa2_vm not linked".
 */

#include <stdbool.h>
#include "mdg1_uds_transport.h"
#include "mdg1_variant_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Progress callback — fired per UDS turn so a future WS layer can
 * stream status. Phase values are stable across versions.
 */
typedef enum {
    MDG1_FLASH_PHASE_INIT             = 0,
    MDG1_FLASH_PHASE_SECURITY_SEED,
    MDG1_FLASH_PHASE_SECURITY_KEY,
    MDG1_FLASH_PHASE_FINGERPRINT,
    MDG1_FLASH_PHASE_SECTION_ERASE,
    MDG1_FLASH_PHASE_SECTION_REQUEST_DOWNLOAD,
    MDG1_FLASH_PHASE_SECTION_TRANSFER_DATA,
    MDG1_FLASH_PHASE_SECTION_TRANSFER_EXIT,
    MDG1_FLASH_PHASE_SECTION_CHECK_MEMORY,
    MDG1_FLASH_PHASE_CHECK_PROG_DEPENDENCIES,
    MDG1_FLASH_PHASE_ECU_RESET,
    MDG1_FLASH_PHASE_DONE,
    MDG1_FLASH_PHASE_FAILED,
    MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE,
} mdg1_flash_phase_t;

typedef struct {
    mdg1_flash_phase_t phase;
    size_t             section_index;   /* 0..variant->section_count-1 (when meaningful) */
    size_t             bytes_done;
    size_t             bytes_total;
    esp_err_t          last_err;
    const char        *message;
} mdg1_flash_progress_t;

typedef void (*mdg1_flash_progress_cb_t)(const mdg1_flash_progress_t *p, void *user_ctx);

/*
 * Plan struct. Most fields come from the loaded variant; the plaintext
 * source is per-flash (could be a customer's BIN download path, not
 * just the manifest's default). For shadow validation, plaintext_source
 * MUST point at the ECU dump bin so the orchestrator's per-section
 * slicing matches MM's exact plaintext.
 */
typedef struct {
    const mdg1_variant_t *variant;           /* required; owner-supplied */
    const char           *plaintext_bin_path; /* if NULL, falls back to variant->plaintext_bin_path */
    const uint8_t         fingerprint_bytes[9]; /* MDG1_PROG_FINGERPRINT_BYTES */
    bool                  use_default_fingerprint;
    /* Runtime opt-in for the HIL halt-before-erase gate. When true,
     * orchestrator_run() returns ESP_ERR_NOT_FINISHED right after the
     * fingerprint write succeeds, BEFORE emitting any RoutineControl-
     * Erase frame. The compile-time MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE
     * flag forces this on regardless of the plan field. */
    bool                  hil_halt_before_erase;
#ifdef MDG1_FLASH_ORCHESTRATOR_HOST_BUILD
    /* TEST-ONLY (host build): when true, the PRIMARY halt-before-erase
     * block is skipped so the host harness can exercise the redundant
     * DEFENSIVE-SECONDARY halt block that lives at the top of the
     * per-section loop. Production firmware never sees this field
     * because it is compiled out unless MDG1_FLASH_ORCHESTRATOR_HOST_BUILD
     * is defined. Setting it without hil_halt_before_erase has no
     * effect — the secondary only fires when the HIL flag is on. */
    bool                  _force_skip_primary_halt_for_test_only;
#endif
} mdg1_flash_plan_t;

/*
 * Run the orchestrator. Blocks until done or fatal error. Calls
 * `progress` (if non-NULL) per UDS turn. Returns ESP_OK on success.
 *
 * `transport` MUST be initialized (production or shadow).
 *
 * `mdg1_payload_set_aes_iface()` MUST have been called with a valid
 * iface before `mdg1_flash_orchestrator_run` is invoked — otherwise
 * the first per-section TransferData fails ESP_ERR_INVALID_STATE.
 */
esp_err_t mdg1_flash_orchestrator_run(const mdg1_flash_plan_t *plan,
                                      mdg1_uds_transport_t    *transport,
                                      mdg1_flash_progress_cb_t progress,
                                      void                    *progress_user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* MDG1_FLASH_ORCHESTRATOR_H */
