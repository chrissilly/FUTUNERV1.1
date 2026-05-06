#ifndef DTC_COMMANDS_H
#define DTC_COMMANDS_H

#include <stddef.h>
#include "esp_err.h"

/*
 * dtc_commands — WS / serial command handlers for DTC read and clear.
 *
 * Both handlers are thin shims around dtc_read() / dtc_clear() in
 * dtc/dtc_feature.c. All UDS protocol logic, feature_manager
 * arbitration, and description resolution lives there. This file is
 * cJSON-only — it formats the JSON response body and that is it.
 *
 * Mirrors the wot_log_commands.{c,h} pattern, which is the canonical
 * template for any future feature that exposes a WS / serial command.
 */

esp_err_t cmd_dtc_read(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_dtc_clear(int fd, const char *params, char *response, size_t response_size);

#endif /* DTC_COMMANDS_H */
