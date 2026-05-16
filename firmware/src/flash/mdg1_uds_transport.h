#ifndef MDG1_UDS_TRANSPORT_H
#define MDG1_UDS_TRANSPORT_H

/*
 * mdg1_uds_transport.h — UDS-level transport abstraction for the MDG1
 * flash orchestrator.
 *
 * The orchestrator operates on whole UDS messages (already-assembled,
 * ISO-TP segmentation hidden below this layer). Two implementations
 * exist:
 *
 *   mdg1_transport_shadow — host-side; logs every TX/RX to a text file
 *                            for diffing against MM captures. Synthesizes
 *                            ECU responses from an expected-responses
 *                            playback fixture.
 *
 *   mdg1_transport_can    — production; wraps the CAN/ISO-TP stack.
 *                            ID 0x7E0 tester→ECU, 0x7E8 ECU→tester.
 *                            DORMANT in this prompt (built but not init'd).
 *
 * Per Hard Rule: tester transmits on 0x7E0 only, listens on 0x7E8 only.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef MDG1_FLASH_ORCHESTRATOR_HOST_BUILD
typedef int esp_err_t;
#  ifndef ESP_OK
#    define ESP_OK                   0
#  endif
#  ifndef ESP_FAIL
#    define ESP_FAIL                -1
#  endif
#  ifndef ESP_ERR_INVALID_ARG
#    define ESP_ERR_INVALID_ARG     0x102
#  endif
#  ifndef ESP_ERR_INVALID_STATE
#    define ESP_ERR_INVALID_STATE   0x103
#  endif
#  ifndef ESP_ERR_INVALID_SIZE
#    define ESP_ERR_INVALID_SIZE    0x104
#  endif
#  ifndef ESP_ERR_NOT_FOUND
#    define ESP_ERR_NOT_FOUND       0x105
#  endif
#  ifndef ESP_ERR_NOT_SUPPORTED
#    define ESP_ERR_NOT_SUPPORTED   0x106
#  endif
#  ifndef ESP_ERR_TIMEOUT
#    define ESP_ERR_TIMEOUT         0x107
#  endif
#  ifndef ESP_ERR_NOT_FINISHED
#    define ESP_ERR_NOT_FINISHED    0x10C
#  endif
#else
#  include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Function-pointer transport interface. The orchestrator holds a pointer
 * to one of these and never knows the underlying transport (CAN vs shadow).
 *
 * Contract:
 *   send_request   — emit a single UDS message. Caller's buffer is the
 *                    whole UDS payload starting with the SID byte. The
 *                    transport handles ISO-TP segmentation if needed.
 *                    Returns ESP_OK on accepted-for-send; transport-
 *                    specific error codes otherwise.
 *
 *   recv_response  — read a single UDS message from the ECU. Blocks up
 *                    to `timeout_ms`. Writes message bytes (SID first)
 *                    into `buf` (up to `cap`); returns actual length
 *                    via `out_len`. Negative responses (`0x7F xx xx`)
 *                    are valid messages and returned normally — the
 *                    orchestrator decides how to handle them.
 *                    ESP_ERR_TIMEOUT on no response in window.
 *
 *   flush          — discard any pending RX buffer / pending state.
 *                    Called between orchestrator phases that the
 *                    underlying transport might have queued data for.
 *                    May be a no-op for some transports.
 *
 * Every callback receives the iface's `ctx` as its first arg — opaque,
 * for transport-private state (file handles, session counters, …).
 */
typedef struct mdg1_uds_transport_s {
    esp_err_t (*send_request)(void *ctx, const uint8_t *data, size_t len);
    esp_err_t (*recv_response)(void *ctx, uint8_t *buf, size_t cap,
                               size_t *out_len, uint32_t timeout_ms);
    esp_err_t (*flush)(void *ctx);
    void *ctx;
} mdg1_uds_transport_t;

#ifdef __cplusplus
}
#endif

#endif /* MDG1_UDS_TRANSPORT_H */
