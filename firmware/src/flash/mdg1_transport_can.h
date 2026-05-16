#ifndef MDG1_TRANSPORT_CAN_H
#define MDG1_TRANSPORT_CAN_H

/*
 * mdg1_transport_can — production CAN/ISO-TP transport for the Phase 2
 * flash orchestrator. Wraps can_manager_send_isotp / can_manager_receive_isotp
 * (which in turn drive the bundled isotp-c reassembly on top of the project's
 * twai-backed CAN driver) and acquires ISOTP_OWNER_PHASE2_FLASH for the
 * duration of the orchestrator run.
 *
 * Hard-coded CAN IDs (per CLAUDE.md hard rule): tester→ECU 0x7E0,
 * ECU→tester 0x7E8. The orchestrator is not allowed to address any other
 * CAN ID; the isotp_link initialization in can_manager.c is what enforces
 * this — that's the single point where 0x7E0 is the configured TX ID.
 *
 * Lifecycle:
 *   - mdg1_transport_can_open()   acquires coordinator ownership, populates
 *                                 iface. Returns ESP_ERR_INVALID_STATE if
 *                                 can_manager is not RUNNING or coordinator
 *                                 is held by another owner.
 *   - mdg1_transport_can_close()  releases coordinator ownership, zeroes iface.
 *                                 Idempotent.
 *
 * Host build excludes this TU entirely (#ifndef MDG1_FLASH_ORCHESTRATOR_HOST_BUILD).
 */

#include "mdg1_uds_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mdg1_transport_can_open(mdg1_uds_transport_t *out_iface);
void      mdg1_transport_can_close(mdg1_uds_transport_t *iface);

#ifdef __cplusplus
}
#endif

#endif /* MDG1_TRANSPORT_CAN_H */
