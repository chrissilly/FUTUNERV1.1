#ifndef DTC_CONFIG_H
#define DTC_CONFIG_H

/*
 * dtc_config.h — central tunables for the DTC read/clear feature.
 *
 * Per FUTV1.1/CLAUDE.md "no magic numbers" rule, every numeric and
 * string constant the DTC feature uses lives in this header. The .c
 * files (dtc_feature.c, dtc_uds.c) MUST NOT contain integer literals
 * (other than 0/1) or behavioral string literals.
 *
 * All defaults below are PROPOSED and need approval from Sean before lock.
 */

/* ------------------------------------------------------------------ */
/* UDS service surface                                                 */
/* ------------------------------------------------------------------ */

/*
 * UDS service IDs and subfunctions used by this feature.
 *
 * Per CLAUDE.md hard-rule §1, only the documented standard UDS
 * services are allowed. We use:
 *   0x19 with subfunction 0x02 (reportDTCByStatusMask) — reads the
 *     list of active/pending DTCs whose status byte intersects the
 *     supplied mask. No 0x04 (snapshot), 0x06 (extended), or 0x0A
 *     (supportedDTCs) — those are out of scope for v1.
 *   0x14 (ClearDiagnosticInformation) with group identifier
 *     0xFFFFFF (all groups). No per-group selectivity in v1.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define DTC_UDS_SID_READ                      0x19
#define DTC_UDS_SUBFUNC_REPORT_BY_STATUS      0x02
#define DTC_UDS_SID_CLEAR                     0x14
#define DTC_UDS_CLEAR_GROUP_ALL_BYTE_HI       0xFF
#define DTC_UDS_CLEAR_GROUP_ALL_BYTE_MID      0xFF
#define DTC_UDS_CLEAR_GROUP_ALL_BYTE_LO       0xFF

/* Positive-response bit (UDS-wide: response SID == request SID + 0x40). */
#define DTC_UDS_POSITIVE_OFFSET               0x40
#define DTC_UDS_NEGATIVE_RESPONSE             0x7F

/* NRC byte for RequestCorrectlyReceived-ResponsePending. ECU emits
 * 7F <sid> 78 while it's working on a long-running request; the client
 * must keep waiting for the eventual non-pending response per ISO 14229. */
#define DTC_UDS_NRC_RESPONSE_PENDING          0x78

/* P2*_server extended response window (ms) per ISO 14229 §7.2 — the
 * max time the ECU may keep emitting NRC 0x78 before the final response.
 * Used to bound the pending-skip loop in target_uds_request. */
#define DTC_UDS_P2_STAR_SERVER_MS             5000

/* Expected positive responses. Proposed default — needs approval from Sean. */
#define DTC_UDS_READ_POSITIVE_SID             0x59  /* 0x19 + 0x40 */
#define DTC_UDS_CLEAR_POSITIVE_SID            0x54  /* 0x14 + 0x40 */

/* CAN ID for physical addressing of the engine ECU.
 * CLAUDE.md §1: 0x7E0 is the ONLY allowed TX address. Never 0x7DF /
 * 0x710 / 0x7E1–0x7E7. Constant lives here so it can never be changed
 * by accident in dtc_*.c. Proposed default — needs approval from Sean. */
#define DTC_UDS_PHYSICAL_TX_ID                0x7E0

/* ------------------------------------------------------------------ */
/* Default behavior                                                    */
/* ------------------------------------------------------------------ */

/*
 * Default status mask used when the caller does not supply one.
 * 0x09 = bit 0 (testFailed) | bit 3 (confirmedDTC) — the standard
 * "show me everything that is failing or has been confirmed failed"
 * mask used by the bulk of OBD-II scan tools.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define DTC_DEFAULT_STATUS_MASK               0x09

/*
 * Per-operation timeouts (ms). Sized for a typical UDS exchange on a
 * Bosch MG1/MDG1/MED17 over 500 kbps CAN, with headroom for the ECU
 * spending up to ~3× its usual response time before we consider the
 * exchange failed.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define DTC_READ_TIMEOUT_MS                   5000
#define DTC_CLEAR_TIMEOUT_MS                  5000

/*
 * Maximum number of DTC records the firmware will surface in one
 * read response. ISO 14229 allows the ECU to return any number of
 * DTC records that fit in the ISO-TP response buffer (4096 bytes max
 * here; that is up to ~1023 4-byte DTC records). The dongle's RAM
 * and the WS payload size do not need to bear that cost — the gauge
 * UI shows DTCs in a scrollable list and 64 is comfortably more than
 * any realistically-faulted ECU surfaces in practice.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define DTC_MAX_CODES_PER_RESPONSE            64

/*
 * Default ECU family for the description lookup table when the
 * variant manifest has not yet identified the connected ECU. MG1
 * is the family Sean's RS7 dev car runs. The active family is
 * overridable at runtime via dtc_feature_set_family().
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define DTC_FAMILY_DEFAULT                    "MG1"

/* ------------------------------------------------------------------ */
/* Wire-format sizing (RFC 14229 / SAE J2012 mechanical)                */
/* ------------------------------------------------------------------ */

/*
 * Bytes per DTC record on the wire (3 bytes DTC code + 1 byte status).
 * Per ISO 14229-1 reportDTCByStatusMask response format. Proposed
 * default — needs approval from Sean before lock. */
#define DTC_RECORD_BYTES                      4

/*
 * Bytes of preamble in a 0x19 0x02 positive response before the first
 * DTC record begins (0x59 SID + 0x02 subfunc echo + 1-byte
 * statusAvailabilityMask). Proposed default — needs approval from
 * Sean before lock. */
#define DTC_READ_RESPONSE_PREAMBLE_BYTES      3

/*
 * Maximum bytes of a 0x19 0x02 positive response we are willing to
 * buffer. Sized as preamble + DTC_MAX_CODES_PER_RESPONSE × DTC_RECORD_BYTES.
 * Proposed default — needs approval from Sean before lock.
 */
#define DTC_READ_RESPONSE_BUFFER_BYTES        (DTC_READ_RESPONSE_PREAMBLE_BYTES + \
                                               DTC_MAX_CODES_PER_RESPONSE * DTC_RECORD_BYTES)

/*
 * Maximum bytes of a 0x14 positive response we accept. Proposed
 * default — needs approval from Sean before lock. */
#define DTC_CLEAR_RESPONSE_BUFFER_BYTES       8

/*
 * Length (chars including NUL) of a SAE J2012 5-character DTC string
 * such as "P0420". Proposed default — needs approval from Sean
 * before lock. */
#define DTC_CODE_STRING_LEN                   6

/*
 * Negative-response payload length: 0x7F + requestSID + NRC byte = 3.
 * Proposed default — needs approval from Sean before lock. */
#define DTC_UDS_NEGATIVE_RESPONSE_BYTES       3

/* ------------------------------------------------------------------ */
/* Target-adapter timing (compiled out under DTC_FEATURE_HOST_BUILD)    */
/* ------------------------------------------------------------------ */

/*
 * Polling interval (ms) the on-target adapter uses while waiting for
 * an ISO-TP response after a UDS request has been transmitted. Short
 * enough to keep the gauge UI responsive; long enough to avoid a
 * busy-wait loop. Proposed default — needs approval from Sean before
 * lock. */
#define DTC_TARGET_POLL_INTERVAL_MS           10

/*
 * Wait (ms) the on-target adapter will block trying to acquire the
 * ISO-TP coordinator lock before declaring the bus busy. Sized to
 * coexist with the 1 Hz tester-present keepalive without serializing
 * behind a stuck logger poll. Proposed default — needs approval from
 * Sean before lock. */
#define DTC_TARGET_ISOTP_REQUEST_TIMEOUT_MS   200

/*
 * Maximum size (bytes) of an outbound DTC UDS request frame. The two
 * services we use require at most 4 bytes (0x14 0xFF 0xFF 0xFF). Sized
 * with headroom for a future status-mask byte addition. Proposed
 * default — needs approval from Sean before lock. */
#define DTC_UDS_REQUEST_BUFFER_BYTES          8

/* Wire-format byte counts for the two requests we issue. The 0x19 0x02
 * read request is [SID, subfunc, statusMask] = 3 bytes; the 0x14 clear
 * request is [SID, group_hi, group_mid, group_lo] = 4 bytes. Proposed
 * defaults — needs approval from Sean before lock. */
#define DTC_UDS_READ_REQUEST_BYTES            3
#define DTC_UDS_CLEAR_REQUEST_BYTES           4

/* Byte offsets within the read request payload. Proposed defaults —
 * needs approval from Sean before lock. */
#define DTC_UDS_READ_REQ_STATUS_MASK_OFFSET   2

/* Byte offsets within the clear request payload (after the SID). The
 * three group bytes follow the 0x14 SID at offsets 1, 2, 3. Proposed
 * defaults — needs approval from Sean before lock. */
#define DTC_UDS_CLEAR_REQ_GROUP_HI_OFFSET     1
#define DTC_UDS_CLEAR_REQ_GROUP_MID_OFFSET    2
#define DTC_UDS_CLEAR_REQ_GROUP_LO_OFFSET     3

/* Byte offsets within a single 4-byte DTC record in the 0x19 0x02
 * response. The first three bytes are the SAE J2012 raw DTC (high,
 * middle, low) and the fourth is the status byte. Proposed defaults —
 * needs approval from Sean before lock. */
#define DTC_UDS_DTC_RECORD_BYTE_HI_OFFSET     0
#define DTC_UDS_DTC_RECORD_BYTE_MID_OFFSET    1
#define DTC_UDS_DTC_RECORD_BYTE_LO_OFFSET     2
#define DTC_UDS_DTC_RECORD_STATUS_OFFSET      3

/* Target-adapter ISO-TP transport return codes (negative values per
 * the dtc_uds_request_fn_t contract). The protocol layer only checks
 * the sign; the exact value is opaque, but naming them makes the
 * adapter intent explicit. Proposed defaults — needs approval from
 * Sean before lock. */
#define DTC_TARGET_UDS_RESULT_BUS_BUSY        (-1)
#define DTC_TARGET_UDS_RESULT_SEND_FAIL       (-2)

#endif /* DTC_CONFIG_H */
