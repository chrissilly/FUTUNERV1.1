#ifndef MDG1_TRANSPORT_SHADOW_H
#define MDG1_TRANSPORT_SHADOW_H

/*
 * mdg1_transport_shadow — host-side transport that logs TX/RX to a
 * deterministic text file and synthesizes ECU responses from a JSON
 * expected-responses playback fixture.
 *
 * Used by the orchestrator's host-test harness to validate the byte
 * stream against the MagicMotorsport reference captures without
 * touching real hardware.
 *
 * Shadow log format (one frame per line):
 *     TX <hex bytes>      tester→ECU
 *     RX <hex bytes>      synthesized ECU→tester
 * No timestamps. Session-variant fields (SA seed/key, fingerprint
 * timestamp, TesterPresent keep-alive count) carry placeholder values
 * here; the diff tool masks them before comparing.
 */

#include "mdg1_uds_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize a shadow transport with an output log path and an
 * expected-responses playback fixture path. The fixture is the JSON
 * produced by tools/extract_mm_expected_responses.py and contains
 * an ordered list of (request_prefix, response_bytes) pairs that
 * shadow recv_response() walks linearly.
 *
 * out_iface — populated; its `ctx` points at an opaque context that
 *             this module allocates. Caller MUST call
 *             mdg1_transport_shadow_close() at end of run to flush
 *             the log + free the context.
 *
 * Returns ESP_OK or an error code on failure (file open, JSON parse).
 */
esp_err_t mdg1_transport_shadow_open(const char *out_log_path,
                                     const char *expected_responses_json_path,
                                     mdg1_uds_transport_t *out_iface);

/* Close + flush + free. Idempotent on already-closed iface. */
void mdg1_transport_shadow_close(mdg1_uds_transport_t *iface);

/*
 * Optional: tell the shadow transport which SecurityAccess seed value
 * to synthesize. Defaults to MDG1_SHADOW_SECURITY_SEED_PLACEHOLDER
 * (0xDEADBEEF). Used by test_session_variant_mask_zeroes_seed_key_fingerprint
 * to vary the seed across runs and prove the diff mask zeros it.
 */
void mdg1_transport_shadow_set_seed(mdg1_uds_transport_t *iface, uint32_t seed_be);

#ifdef __cplusplus
}
#endif

#endif /* MDG1_TRANSPORT_SHADOW_H */
