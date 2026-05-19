#ifndef LOGGER_PROFILE_H
#define LOGGER_PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "logger_variables.h"

/**
 * Logger Profile Manager
 *
 * Persists user-selected variable lists per boxcode to LittleFS.
 * Required variables are always included automatically — profiles
 * only track which optional variables the user has enabled.
 *
 * Storage: /cal/profiles/<boxcode>.json
 * Format:  {"vars":["rl_w","tmot","pvdg_w",...]}
 */

#define LOGGER_PROFILE_MAX_SELECTED  64  /* Max optional vars in a profile */
#define LOGGER_PROFILE_DIR           "profiles"

/**
 * Initialize the profile system. Creates the profiles directory if needed.
 * Must be called after fs_manager_mount_partition(FS_PARTITION_STORAGE).
 */
esp_err_t logger_profile_init(void);

/**
 * Save the current optional variable selection for the given boxcode.
 *
 * @param boxcode       Boxcode string (e.g. "4K0907557G__0003")
 * @param var_names     Array of variable name strings to save
 * @param var_count     Number of entries in var_names
 * @return ESP_OK on success
 */
esp_err_t logger_profile_save(const char *boxcode,
                               const char **var_names,
                               uint8_t var_count);

/**
 * Load the saved profile for a boxcode.
 *
 * @param boxcode       Boxcode string
 * @param var_names     Output array of variable name buffers (caller allocates)
 * @param max_vars      Size of the var_names array
 * @param var_count     Output: number of variables loaded
 * @return ESP_OK if profile exists and was loaded, ESP_ERR_NOT_FOUND if no profile
 */
esp_err_t logger_profile_load(const char *boxcode,
                               char var_names[][32],
                               uint8_t max_vars,
                               uint8_t *var_count);

/**
 * Delete the saved profile for a boxcode.
 */
esp_err_t logger_profile_delete(const char *boxcode);

/**
 * Check if a saved profile exists for the given boxcode.
 */
bool logger_profile_exists(const char *boxcode);

/**
 * Apply a saved profile to the logger. Clears current variables,
 * adds all required variables, then adds each saved optional variable.
 *
 * @param boxcode  Boxcode to load profile for (must already be set via logger_variables_set_boxcode)
 * @return true if profile was applied (or no profile exists and defaults were used),
 *         false on error
 */
bool logger_profile_apply(const char *boxcode);

#endif // LOGGER_PROFILE_H
