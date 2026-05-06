#ifndef WOT_LOG_COMMANDS_H
#define WOT_LOG_COMMANDS_H

#include <stddef.h>
#include "esp_err.h"

/*
 * wot_log_commands — WS / serial command handlers for starting and
 * stopping the WOT logging feature. These are the FIRST place
 * command_handler talks to feature_manager. Use this file as the
 * template for every future feature wiring (DTC, VIN pairing, SBF
 * live tune, BLE ethanol, …).
 */

esp_err_t cmd_wot_log_start(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_wot_log_stop(int fd, const char *params, char *response, size_t response_size);

#endif /* WOT_LOG_COMMANDS_H */
