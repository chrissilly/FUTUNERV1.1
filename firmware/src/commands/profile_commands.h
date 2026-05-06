#ifndef PROFILE_COMMANDS_H
#define PROFILE_COMMANDS_H

#include "command_handler.h"

/**
 * List all available variables for the current boxcode.
 * Returns the full catalog with name, display_name, unit, required flag.
 * No params required. Boxcode must be set (ECU connected).
 *
 * Response: {"variables":[{"name":"nmot_w","display_name":"Engine Speed",
 *            "unit":"rpm","required":true,"size":2},...], "boxcode":"...","count":10}
 */
esp_err_t cmd_list_available_vars(int fd, const char *params, char *response, size_t response_size);

/**
 * Get the current logger profile (which optional vars are selected).
 *
 * Response: {"boxcode":"...","selected":["rl_w","tmot",...],"has_saved_profile":true}
 */
esp_err_t cmd_get_logger_profile(int fd, const char *params, char *response, size_t response_size);

/**
 * Save a new logger profile. Params: {"variables":["rl_w","tmot","pvdg_w",...]}
 * These are optional variable names only — required vars are always included.
 * Saves to LittleFS and triggers logger reconfiguration on next poll cycle.
 *
 * Response: success/fail message
 */
esp_err_t cmd_set_logger_profile(int fd, const char *params, char *response, size_t response_size);

/**
 * Delete the saved profile for the current boxcode, reverting to defaults.
 *
 * Response: success/fail message
 */
esp_err_t cmd_delete_logger_profile(int fd, const char *params, char *response, size_t response_size);

#endif // PROFILE_COMMANDS_H
