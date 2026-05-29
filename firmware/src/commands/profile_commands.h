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

/**
 * P-75: enumerate stored named profiles for the current boxcode.
 * No params. Response includes the active marker.
 *
 * Response: {"boxcode":"...","active":"...","profiles":[{"name":"x","vars":[...]},...]}
 */
esp_err_t cmd_list_logger_profiles(int fd, const char *params, char *response, size_t response_size);

/**
 * P-75: mark a stored named profile as active and reconfigure. The
 * profile file must already exist (saved via cmd_set_logger_profile).
 * Params: {"name":"<profile-name>"}
 */
esp_err_t cmd_load_logger_profile(int fd, const char *params, char *response, size_t response_size);

/**
 * P-75: rename a stored profile. If old_name was the active profile,
 * the active marker tracks new_name. Fails if new_name already exists.
 * Params: {"old_name":"a","new_name":"b"}
 */
esp_err_t cmd_rename_logger_profile(int fd, const char *params, char *response, size_t response_size);

#endif // PROFILE_COMMANDS_H
