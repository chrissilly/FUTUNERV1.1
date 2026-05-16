#ifndef MDG1_FLASH_ORCHESTRATOR_CONFIG_H
#define MDG1_FLASH_ORCHESTRATOR_CONFIG_H

/*
 * mdg1_flash_orchestrator_config.h — central tunables for the MDG1
 * 5-section full-flash orchestrator (Phase 2).
 *
 * Per FUTV1.1/CLAUDE.md "no magic numbers" rule, every numeric or
 * string constant the orchestrator + transport + manifest-loader
 * codepath consumes lives here. Wire-protocol byte values come from
 * hw_reference/MM_Flash_Capture_Analysis.md (the ground-truth MM
 * capture decode) — they're not magic, they're protocol, and they're
 * named here so the orchestrator C code reads as protocol-level
 * intent.
 *
 * All defaults below are PROPOSED and need approval from Sean before lock.
 */

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* CAN identifiers — single-ECU MDG1 (Hard rule: 0x7E0 / 0x7E8 only)  */
/* ------------------------------------------------------------------ */

/*
 * Tester→ECU CAN ID. Diagnostic addressing for the engine ECU on the
 * MDG1 family. NEVER broadcast on 0x7DF, NEVER address 0x710 or
 * 0x7E1..0x7E7 — the J533 gateway locks out for 10+ minutes.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_FLASH_CAN_ID_REQUEST           0x7E0u

/*
 * ECU→tester CAN ID.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_FLASH_CAN_ID_RESPONSE          0x7E8u

/* ------------------------------------------------------------------ */
/* UDS service IDs — sourced from MM_Flash_Capture_Analysis.md §2     */
/* ------------------------------------------------------------------ */

#define MDG1_UDS_SID_DIAG_SESSION           0x10u  /* DiagnosticSessionControl   */
#define MDG1_UDS_SID_ECU_RESET              0x11u  /* ECUReset                   */
#define MDG1_UDS_SID_CLEAR_DTC              0x14u  /* ClearDiagnosticInformation */
#define MDG1_UDS_SID_READ_DID               0x22u  /* ReadDataByIdentifier       */
#define MDG1_UDS_SID_SECURITY_ACCESS        0x27u  /* SecurityAccess             */
#define MDG1_UDS_SID_WRITE_DID              0x2Eu  /* WriteDataByIdentifier      */
#define MDG1_UDS_SID_ROUTINE_CONTROL        0x31u  /* RoutineControl             */
#define MDG1_UDS_SID_REQUEST_DOWNLOAD       0x34u  /* RequestDownload            */
#define MDG1_UDS_SID_TRANSFER_DATA          0x36u  /* TransferData               */
#define MDG1_UDS_SID_REQUEST_TRANSFER_EXIT  0x37u  /* RequestTransferExit        */
#define MDG1_UDS_SID_TESTER_PRESENT         0x3Eu  /* TesterPresent              */

/* Negative-response service code (always 0x7F on the wire). */
#define MDG1_UDS_NEGATIVE_RESPONSE          0x7Fu

/* RoutineControl IDs (per MM_Flash_Capture_Analysis.md §2.4) */
#define MDG1_RID_ERASE_MEMORY               0xFF00u  /* 31 01 FF 00 ...        */
#define MDG1_RID_CHECK_PROG_DEPENDENCIES    0xFF01u  /* 31 01 FF 01            */
#define MDG1_RID_CHECK_MEMORY               0x0202u  /* 31 01 02 02 <CRC32>    */
#define MDG1_RID_PROG_PRECONDITIONS         0x0203u  /* 31 01 02 03            */

/* Diagnostic-session sub-functions (MM_Flash_Capture_Analysis.md §2.1) */
#define MDG1_SESSION_EXTENDED               0x03u
#define MDG1_SESSION_PROGRAMMING            0x02u

/* ECUReset sub-function (hard reset). */
#define MDG1_RESET_HARD                     0x01u

/* SecurityAccess level for MDG1 family. Send-key level is +1. */
#define MDG1_SECURITY_LEVEL_SEED            0x11u
#define MDG1_SECURITY_LEVEL_KEY             0x12u

/* TesterPresent sub-function (zero-sub-function variant). */
#define MDG1_TESTER_PRESENT_SUBFUNCTION     0x00u

/* Programming-fingerprint DID (MM writes this between SA and erase). */
#define MDG1_DID_PROG_FINGERPRINT           0xF15Au

/*
 * MM's fingerprint payload bytes for the dev-car capture (9 bytes).
 * The orchestrator emits this exact value in shadow mode; the diff
 * tool masks the entire WriteDataByIdentifier 0x2E F1 5A frame so
 * timestamps from production tooling don't false-fail the diff.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_PROG_FINGERPRINT_BYTES         { \
    0x21, 0x11, 0x22, 0x00, 0x06, 0x46, 0x22, 0x0A, 0x68 \
}
#define MDG1_PROG_FINGERPRINT_LEN           9u

/* ------------------------------------------------------------------ */
/* Wire-format constants                                              */
/* ------------------------------------------------------------------ */

/*
 * dataFormatIdentifier sent on RequestDownload (high nibble = compression
 * method 2 = LZRB; low nibble = encryption method 0xA = Bosch AES-128-CBC).
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_DATA_FORMAT_LZRB_AES           0x2Au

/*
 * addressAndLengthFormatIdentifier sent on RequestDownload (high nibble
 * = memorySize length 3 bytes; low nibble = memoryAddress length 1 byte).
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_ALFID_SIZE3_ADDR1              0x31u

/*
 * EraseMemory routine parameter prefix (RID 0xFF00) — "01 <BID> 00".
 * The 0x01 is "erase 1 memory range"; the trailing 0x00 is padding the
 * MM tool emits consistently. We mirror it exactly.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_ERASE_NUM_RANGES               0x01u
#define MDG1_ERASE_TRAILING_PAD             0x00u

/*
 * RequestTransferExit parameter byte. MM emits a single 0x00 padding
 * byte after the service ID; we mirror exactly.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_TRANSFER_EXIT_TRAILING         0x00u

/*
 * Per-TransferData blockSequenceCounter starts at this value at the
 * top of every section. Wraps modulo 256 within a section
 * (MM_Flash_Capture_Analysis.md §2.4.3).
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_TRANSFER_DATA_BC_INITIAL       0x01u

/*
 * maxNumberOfBlockLength as observed in the ECU's response to
 * RequestDownload. MM-captured value is always 0x0FFF on MDG1.
 * Orchestrator parses the actual response value out of 74 20 <maxlen…>
 * and clamps chunks accordingly; this constant is the *expected*
 * value used to sanity-check the parsed value at startup.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_EXPECTED_MAX_BLOCK_LENGTH      0x0FFFu

/* Bytes that the TransferData PCI consumes per chunk: SID + BC. */
#define MDG1_TRANSFER_DATA_PCI_OVERHEAD     2u

/* ------------------------------------------------------------------ */
/* Timeouts and limits                                                */
/* ------------------------------------------------------------------ */

/*
 * P2/P2* timing budgets per ISO 14229 + MM's captured values
 * (50 02 00 32 01 F4 → P2=50 ms, P2*=5000 ms). Negative-response
 * pending (0x7F xx 0x78) may extend up to P2*. EraseMemory and
 * CheckMemory observed wall times are below this ceiling.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_UDS_P2_MS                      50u
#define MDG1_UDS_P2_STAR_MS                 5000u

/*
 * Long-running routine timeout. Used for ProgrammingPreconditions
 * (3.6 s observed), TransferExit (1.3 s observed), and CheckMemory
 * (2.4 s observed on ASW3). 10 s gives ~2x headroom on the worst case.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_UDS_ROUTINE_TIMEOUT_MS         10000u

/*
 * ECUReset wall time observed at ~0.7 s; allow 5 s for slow gateways.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_UDS_RESET_TIMEOUT_MS           5000u

/*
 * Per-TransferData ACK timeout. MM observed ~5 ms; allow 1 s for
 * bus contention / re-arbitration.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_UDS_TRANSFER_ACK_TIMEOUT_MS    1000u

/*
 * Maximum number of flash sections in a single MDG1 full-flash plan.
 * Today's MDG1 layout (block IDs 0x02..0x06) has exactly 5; this
 * constant is the static upper bound for the plan struct.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_MAX_FLASH_SECTIONS             5u

/*
 * Maximum length of a UDS message we'll send or receive (after ISO-TP
 * reassembly). 4 KiB covers any TransferData chunk plus PCI overhead.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_MAX_UDS_MESSAGE_BYTES          4100u

/* ------------------------------------------------------------------ */
/* Shadow-mode placeholder values (zeroed by the diff tool's masking) */
/* ------------------------------------------------------------------ */

/*
 * Placeholder SecurityAccess seed the shadow transport synthesizes in
 * place of the real ECU's random seed. Any value works — the diff
 * tool zeros the seed bytes before comparing. 0xDEADBEEF chosen as
 * an instantly-recognizable sentinel during debugging.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_SHADOW_SECURITY_SEED_PLACEHOLDER  0xDEADBEEFu

/* ------------------------------------------------------------------ */
/* HIL preflight halt-before-erase compile-time gate                  */
/* ------------------------------------------------------------------ */

/*
 * When set to 1 at build time (-DMDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE=1),
 * mdg1_flash_orchestrator_run() returns ESP_ERR_NOT_FINISHED immediately
 * after the fingerprint write succeeds, BEFORE emitting any
 * RoutineControl-Erase (31 01 FF 00 ...) frame.
 *
 * Used by the dev-RS7 HIL preflight firmware build that validates
 * SecurityAccess + fingerprint wire bytes against MagicMotorsport's
 * reference capture without risking a real erase.
 *
 * Production firmware MUST ship with this OFF. The runtime override
 * mdg1_flash_plan_t::hil_halt_before_erase provides per-call opt-in
 * for host tests + shadow dry-runs without rebuilding the firmware.
 *
 * Proposed default 0 (OFF) — needs approval from Sean before lock.
 */
#ifndef MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE
#define MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE 0
#endif

/* ------------------------------------------------------------------ */
/* HIL preflight NVS-armed autostart                                  */
/* ------------------------------------------------------------------ */

/*
 * NVS namespace for the HIL preflight autostart flag. Distinct from
 * the main FUTUNER namespace so a NVS erase of phase2 keys doesn't
 * touch logger profiles, license cache, etc.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_HIL_NVS_NAMESPACE              "phase2"

/*
 * NVS key (uint8_t) — set to 1 by phase2_hil_preflight_arm; read +
 * cleared (set to 0) at boot by phase2_hil_autostart_run_if_armed.
 * One-shot semantics: every armed cycle runs exactly once.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_HIL_NVS_KEY_ARMED              "hil_pf_armed"

/*
 * LittleFS path the autostart writes its shadow log to. Caller can
 * pull this via fs_read (WS) or via the boot-time base64 dump on
 * UART. Path is absolute (/cal mount); the fs_read API expects the
 * relative form MDG1_HIL_AUTOSTART_LOG_FS_PATH.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_HIL_AUTOSTART_LOG_ABS_PATH     "/cal/phase2_hil_preflight.log"
#define MDG1_HIL_AUTOSTART_LOG_FS_PATH      "phase2_hil_preflight.log"

/* Prod-mode autostart writes to a SEPARATE file so a shadow+prod side-
 * by-side comparison is possible by pulling both via fs_read. */
#define MDG1_HIL_AUTOSTART_LOG_PROD_ABS_PATH "/cal/phase2_hil_preflight_prod.log"
#define MDG1_HIL_AUTOSTART_LOG_PROD_FS_PATH  "phase2_hil_preflight_prod.log"

/*
 * UART markers bracketing the base64-encoded shadow log dumped at
 * boot. Tooling parses between these literal lines to reconstruct
 * the log without needing WS/fs_read access.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_HIL_AUTOSTART_LOG_BEGIN_MARKER "[PHASE2_HIL_AUTOSTART_LOG_BEGIN]"
#define MDG1_HIL_AUTOSTART_LOG_END_MARKER   "[PHASE2_HIL_AUTOSTART_LOG_END]"

/*
 * Boot-time chip-report marker line. Tooling greps this from the boot
 * output to confirm the autostart path executed.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_HIL_AUTOSTART_COMPLETE_MARKER  "[PHASE2_HIL_AUTOSTART] complete"

/* ------------------------------------------------------------------ */
/* Production CAN transport tuning                                    */
/* ------------------------------------------------------------------ */

/*
 * Poll interval for can_manager_receive_isotp inside the transport's
 * recv loop. 10 ms matches dtc_feature.c's poll cadence — fast enough
 * to keep MDG1's typical P2 turnaround responsive, slow enough not to
 * starve other tasks.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_TRANSPORT_CAN_RECV_POLL_INTERVAL_MS    10u

/*
 * Maximum time mdg1_transport_can_open() will wait to acquire ownership
 * of the ISO-TP coordinator (other features holding it: logger polls,
 * connection manager tester-presents). Beyond this, the orchestrator
 * fails fast rather than spin forever.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_TRANSPORT_CAN_COORDINATOR_TIMEOUT_MS   3000u

/*
 * Maximum hex byte count the prod-transport TX-tee log emits per frame.
 * Caps a wild TransferData chunk from flooding the UART; flash-critical
 * frames (SA seed/key, fingerprint write) are <16 bytes so they always
 * print in full. Trailing bytes beyond the cap are ellipsized.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_TRANSPORT_CAN_TEE_LOG_HEX_BYTES        32u

#endif /* MDG1_FLASH_ORCHESTRATOR_CONFIG_H */
