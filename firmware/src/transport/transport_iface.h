#ifndef FUTUNER_TRANSPORT_IFACE_H
#define FUTUNER_TRANSPORT_IFACE_H

/*
 * transport_iface.h — proposed Phase 1+ transport-abstraction shape.
 *
 * Status: PROPOSED. Not yet implemented, not yet registered in
 * firmware/src/CMakeLists.txt. Lands here as a design doc in code
 * form so future work has a target. See firmware/src/transport/README.md
 * for the as-is audit + migration sequence.
 *
 * Spec: MISSION_SPEC §4.7 — "UDS/ISO-TP stack must be
 * transport-agnostic — same application logic runs over CAN or
 * Ethernet without modification."
 *
 * Sibling reference: firmware/src/flash/ already implements a per-
 * call transport vtable (`mdg1_uds_transport_t`) scoped to the
 * Phase 2 flash orchestrator. This interface generalizes that
 * shape for Phase 1's main UDS path.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Forward declaration — concrete struct private to each transport's
 * implementation. */
typedef struct futuner_transport futuner_transport_t;

/* Transport-agnostic config. Implementations cast to their own
 * concrete config struct. Sized for a small union; if config payloads
 * grow, switch to a tagged union or per-impl factory function. */
typedef struct {
    /* Implementation tag. The factory dispatches on this. */
    enum {
        FUTUNER_TRANSPORT_CAN = 0,
        FUTUNER_TRANSPORT_ETH = 1,
    } kind;

    /* Per-kind opaque blob; transport_*.c casts to the right type. */
    void *impl_config;
} futuner_transport_config_t;

/*
 * Open a transport. Allocates the concrete struct, returns the
 * handle through *out. Caller owns the handle and must call
 * futuner_transport_close() when done.
 */
esp_err_t futuner_transport_open(futuner_transport_t              **out,
                                  const futuner_transport_config_t  *cfg);

/*
 * Send `len` bytes as the next request. The transport is
 * responsible for ISO-TP segmentation if the payload exceeds the
 * link MTU.
 */
esp_err_t futuner_transport_send(futuner_transport_t *t,
                                  const uint8_t       *payload,
                                  size_t               len);

/*
 * Block up to timeout_ms for the next reassembled response.
 * Writes up to `cap` bytes into payload; the actual length lands
 * in *out_len. Returns ESP_OK on a full response, ESP_ERR_TIMEOUT
 * on no-response-in-window, ESP_FAIL on transport error.
 */
esp_err_t futuner_transport_recv(futuner_transport_t *t,
                                  uint8_t             *payload,
                                  size_t               cap,
                                  size_t              *out_len,
                                  uint32_t             timeout_ms);

/* "can" / "eth" — for logging and diagnostics. */
const char *futuner_transport_name(const futuner_transport_t *t);

/* Idempotent. */
esp_err_t futuner_transport_close(futuner_transport_t *t);

#endif /* FUTUNER_TRANSPORT_IFACE_H */
