#ifndef DTC_UDS_H
#define DTC_UDS_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "dtc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dtc_uds — pure UDS protocol layer for DTC operations.
 *
 * This header is internal to the dtc module (and to the host test
 * harness). dtc_feature.c is the public-API consumer; client code
 * outside this module talks through dtc.h.
 *
 * The protocol layer never touches the CAN bus directly. It accepts
 * a `dtc_uds_request_fn_t` function pointer at init time; that
 * function is responsible for:
 *   1. Sending the request bytes via ISO-TP (CAN ID 0x7E0 only —
 *      see CLAUDE.md §1).
 *   2. Waiting for and returning the ECU's response bytes (handles
 *      multi-frame ISO-TP fragmentation transparently).
 *
 * On target the adapter wraps isotp_coordinator + can_manager. In
 * the host unit test it is replaced with a deterministic mock that
 * returns canned 0x59 / 0x54 / 0x7F payloads.
 */

/*
 * Transport callback.
 *
 * req:        outbound request bytes (UDS service ID first).
 * req_len:    number of bytes in req.
 * resp:       caller-provided response buffer; the callback fills
 *             this with the bytes ISO-TP reassembled from the ECU.
 * resp_cap:   capacity of resp in bytes.
 * timeout_ms: maximum time the callback may block waiting for a
 *             complete response.
 * user_ctx:   opaque pointer passed through from dtc_uds_init().
 *
 * Return value:
 *   > 0 — number of response bytes written into resp.
 *   = 0 — no response within timeout (treat as transport timeout).
 *   < 0 — transport error (bus busy, send failed, etc.). The exact
 *         negative value is opaque; the protocol layer only checks
 *         the sign.
 */
typedef int (*dtc_uds_request_fn_t)(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *resp,
                                    size_t         resp_cap,
                                    uint32_t       timeout_ms,
                                    void          *user_ctx);

/*
 * Configure the UDS protocol layer with a transport callback.
 *
 * Idempotent: calling more than once replaces the registered
 * function pointer + ctx (used by the host test to swap mocks
 * between scenarios).
 */
esp_err_t dtc_uds_init(dtc_uds_request_fn_t request_fn, void *user_ctx);

/* Reset the protocol layer to an uninitialized state. */
void      dtc_uds_deinit(void);

/*
 * Issue UDS 0x19 0x02 (reportDTCByStatusMask) and parse the response.
 *
 * status_mask:  status filter byte sent in the request.
 * timeout_ms:   transport deadline.
 * out_entries:  caller-provided array; each filled-in entry has its
 *               .code populated as a 5-character SAE J2012 string and
 *               .status set to the raw ECU status byte. The
 *               .description pointer is left NULL — that resolution
 *               happens at the dtc_feature.c layer where the family
 *               table lives.
 * entries_cap:  capacity of out_entries.
 * out_count:    number of entries actually filled. May exceed
 *               entries_cap-counted records if the ECU returned more
 *               than the buffer holds, in which case out_count is
 *               clamped to entries_cap.
 *
 * Returns:
 *   ESP_OK            — positive response, parsed cleanly.
 *   ESP_ERR_INVALID_STATE — dtc_uds_init not called.
 *   ESP_ERR_INVALID_ARG   — out_entries / out_count NULL, or cap == 0.
 *   ESP_ERR_TIMEOUT       — transport returned 0 bytes in time.
 *   ESP_ERR_INVALID_RESPONSE — response did not parse (bad SID,
 *                              truncated, NRC). Use
 *                              dtc_uds_last_nrc() to retrieve the NRC
 *                              byte if applicable (0 if not negative).
 *   ESP_FAIL          — generic transport failure (negative return
 *                       from the transport callback).
 */
esp_err_t dtc_uds_read_dtcs_by_status_mask(uint8_t       status_mask,
                                           uint32_t      timeout_ms,
                                           dtc_entry_t  *out_entries,
                                           size_t        entries_cap,
                                           size_t       *out_count);

/*
 * Issue UDS 0x14 with group 0xFFFFFF (ClearDiagnosticInformation, all
 * groups).
 *
 * timeout_ms: transport deadline.
 *
 * Returns ESP_OK on positive response, ESP_ERR_INVALID_RESPONSE on
 * NRC, ESP_ERR_TIMEOUT on no response, ESP_FAIL on transport error.
 * Same NRC retrieval shape as the read path.
 */
esp_err_t dtc_uds_clear_diagnostic_information(uint32_t timeout_ms);

/*
 * NRC byte from the most recent negative response, if the prior call
 * returned ESP_ERR_INVALID_RESPONSE. 0 if the prior call did not
 * surface a negative response.
 */
uint8_t   dtc_uds_last_nrc(void);

/*
 * Format a 3-byte raw DTC into the standard 5-character SAE J2012
 * string ("P0420"). out must be at least DTC_CODE_STRING_LEN bytes.
 *
 * Exposed so the host test can assert formatting independently.
 */
void      dtc_uds_format_dtc_code(uint8_t hi, uint8_t mid, uint8_t lo,
                                  char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DTC_UDS_H */
