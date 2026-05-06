#ifndef SBF_COMMANDS_H
#define SBF_COMMANDS_H

#include <stddef.h>
#include "esp_err.h"

/*
 * sbf_commands — WS / serial command handlers for live tune.
 *
 * Four commands:
 *   live_tune_start   UNSECURED  → params {stage, ethanol_pct}
 *   live_tune_set     UNSECURED  → params {stage, ethanol_pct}
 *   live_tune_stop    UNSECURED
 *   live_tune_status  UNSECURED  (read-only snapshot)
 *
 * The license gate runs inside sbf_orchestrator_live_tune_start so
 * the gate path is consistent whether the request comes via WS,
 * serial, or any future RPC surface.
 */

esp_err_t cmd_live_tune_start  (int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_live_tune_set    (int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_live_tune_stop   (int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_live_tune_status (int fd, const char *params, char *response, size_t response_size);

#endif /* SBF_COMMANDS_H */
