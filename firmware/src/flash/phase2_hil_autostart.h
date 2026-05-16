#ifndef PHASE2_HIL_AUTOSTART_H
#define PHASE2_HIL_AUTOSTART_H

/*
 * phase2_hil_autostart — NVS-armed one-shot HIL preflight runner.
 *
 * Workflow:
 *   1. Operator calls phase2_hil_autostart_arm() (via the
 *      phase2_hil_preflight_arm WS/serial command, auth-gated).
 *      This writes a uint8_t flag into NVS namespace
 *      MDG1_HIL_NVS_NAMESPACE / key MDG1_HIL_NVS_KEY_ARMED.
 *   2. Operator reboots the dongle.
 *   3. At boot, after feature_manager_init and the Phase 2 AES iface
 *      register, main.c calls phase2_hil_autostart_run_if_armed().
 *      This reads the flag, CLEARS it immediately (one-shot — must
 *      not loop on subsequent boots), and if it was set, runs the
 *      orchestrator shadow preflight, writes the resulting UDS log
 *      to MDG1_HIL_AUTOSTART_LOG_ABS_PATH, dumps the log as base64
 *      over UART between marker lines, and logs
 *      MDG1_HIL_AUTOSTART_COMPLETE_MARKER.
 *
 * Why NVS-armed instead of a direct trigger:
 *   - Dongle's primary console is UART0 (physical pins) with
 *     USB-Serial-JTAG only as secondary-output, so USB-CDC stdin
 *     doesn't reach the command dispatcher. See PHASE_2_PREREQUISITES
 *     P-item "USJ primary console".
 *   - WebSocket dispatcher only spins up when an AP client connects.
 *   - NVS arm bridges both — operator arms via whichever surface is
 *     available, reboot triggers the run, and base64 dump gives a
 *     remote-readable artifact.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Autostart modes. NVS key `MDG1_HIL_NVS_KEY_ARMED` stores one of these
 * as a uint8_t. Default 0 (unarmed); never-written keys read back as 0.
 */
typedef enum {
    PHASE2_HIL_MODE_NONE   = 0,
    PHASE2_HIL_MODE_SHADOW = 1,  /* host-shadow transport, no CAN bus */
    PHASE2_HIL_MODE_PROD   = 2,  /* production CAN transport — wire bytes
                                    leave the chip; halt-before-erase
                                    still active so no EraseMemory frame
                                    is ever emitted */
} phase2_hil_mode_t;

/*
 * Write the autostart mode into NVS. Shadow alias kept for callers that
 * predate the mode enum. Idempotent.
 */
esp_err_t phase2_hil_autostart_arm(void);                          /* mode = SHADOW */
esp_err_t phase2_hil_autostart_arm_with_mode(phase2_hil_mode_t mode);

/*
 * Read the autostart-armed flag. If set:
 *   1. Clear the flag (one-shot — guaranteed BEFORE the orchestrator
 *      runs, so a crash mid-run does not loop the dongle into a
 *      perpetual preflight cycle).
 *   2. Run the orchestrator shadow preflight (write log to
 *      MDG1_HIL_AUTOSTART_LOG_ABS_PATH).
 *   3. Dump the resulting log over UART between
 *      MDG1_HIL_AUTOSTART_LOG_{BEGIN,END}_MARKER lines, base64-encoded.
 *   4. Log MDG1_HIL_AUTOSTART_COMPLETE_MARKER.
 *
 * If the flag is not set, returns ESP_OK and does nothing. Safe to
 * call on every boot.
 *
 * In dry-run mode (compiled with MDG1_HIL_AUTOSTART_DRY_RUN=1), the
 * arm-read-clear-marker lifecycle runs but the orchestrator is NOT
 * invoked; the marker still logs so the wiring can be validated
 * before Phase 2 is added.
 */
esp_err_t phase2_hil_autostart_run_if_armed(void);

/*
 * Snapshot of the armed flag (read-only). For diagnostics.
 */
bool phase2_hil_autostart_is_armed(void);

#ifdef __cplusplus
}
#endif

#endif /* PHASE2_HIL_AUTOSTART_H */
