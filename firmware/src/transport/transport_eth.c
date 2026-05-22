/*
 * transport_eth.c — Ethernet transport skeleton.
 *
 * Status: STUB. Not yet implemented; not registered in
 * firmware/src/CMakeLists.txt. Lands here so future work has a
 * known landing zone. Sibling: transport_iface.h (proposed
 * interface), transport_can.c (planned adapter over can_manager_*).
 *
 * Hardware: W5500 SPI Ethernet PHY (or equivalent) — "arriving
 * soon" per MISSION_SPEC §2 as of 2026-05-22.
 *
 * Design doc: firmware/src/transport/README.md. Owner directive
 * 2026-05-21 marks B5 as DEFERRED-PENDING-HARDWARE — actual
 * implementation waits on hardware-on-bench HIL.
 */

#include "transport_iface.h"

/* TODO(B5): implement futuner_transport_open() for FUTUNER_TRANSPORT_ETH.
 *
 * Expected work:
 *   - Initialize W5500 via SPI (ESP-IDF spi_master driver)
 *   - Bring up LwIP netif on the W5500
 *   - Accept TCP socket (or raw UDP?) destination from impl_config
 *   - Allocate concrete struct futuner_transport with the socket
 *     handle stashed inside
 *
 * Outstanding decisions Sean owns (see README §migration path):
 *   - W5500 vs alternative PHY
 *   - DoIP (ISO 13400) vs raw TCP framing
 *   - Failover semantics if CAN primary fails over to Ethernet
 */

/* TODO(B5): implement futuner_transport_send() — push payload onto
 * the active socket. ISO-TP segmentation is link-MTU-dependent;
 * Ethernet payloads can ride 1500-byte frames directly, no
 * fragmentation needed for typical UDS sizes. */

/* TODO(B5): implement futuner_transport_recv() — block on
 * select()/recv() with timeout, return reassembled payload. */

/* TODO(B5): implement futuner_transport_name() / _close(). */
