#ifndef PHASE2_HIL_PREFLIGHT_COMMANDS_H
#define PHASE2_HIL_PREFLIGHT_COMMANDS_H

/*
 * phase2_hil_preflight_commands — serial/WS command surface for the
 * dev-RS7 HIL preflight halt-before-erase validation.
 *
 * Args:
 *   ""        — same as "shadow" (default)
 *   "shadow"  — exercise the orchestrator end-to-end with the shadow
 *               UDS transport. Writes the resulting UDS log to
 *               /cal/phase2_hil_preflight.log on LittleFS. Asserts
 *               the orchestrator halts before EraseMemory (returns
 *               ESP_ERR_NOT_FINISHED, MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE
 *               progress event fires exactly once, no
 *               MDG1_FLASH_PHASE_SECTION_ERASE event). NO CAN BUS.
 *   "prod"    — reserved for the post-"go HIL" flow that talks to the
 *               real ECU; rejected in this build.
 *
 * Response (JSON):
 *   {
 *     "ok": <bool>,
 *     "log_path": "/cal/phase2_hil_preflight.log",
 *     "log_fs_read_path": "phase2_hil_preflight.log",
 *     "rc": <int>,
 *     "events": <int>,
 *     "halt_events_seen": <int>,
 *     "erase_events_seen": <int>,
 *     ["error": "..."]
 *   }
 *
 * Pull the log via the existing fs_read command with path
 * "phase2_hil_preflight.log" (FS_PARTITION_STORAGE is the /cal mount).
 */

#include "esp_err.h"
#include <stddef.h>

esp_err_t cmd_phase2_hil_preflight(int fd,
                                   const char *params,
                                   char *response,
                                   size_t response_size);

/*
 * phase2_hil_preflight_arm — set the NVS armed flag so the NEXT boot
 * runs the shadow preflight autostart. See firmware/src/flash/
 * phase2_hil_autostart.h for the lifecycle.
 *
 * Response (JSON):
 *   { "ok": <bool>, "armed": <bool>, ["error": "..."] }
 */
esp_err_t cmd_phase2_hil_preflight_arm(int fd,
                                       const char *params,
                                       char *response,
                                       size_t response_size);

#endif /* PHASE2_HIL_PREFLIGHT_COMMANDS_H */
