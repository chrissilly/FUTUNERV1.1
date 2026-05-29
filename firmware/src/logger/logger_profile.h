#ifndef LOGGER_PROFILE_H
#define LOGGER_PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#define LOGGER_PROFILE_NAME_MAX_LEN  32  /* Max length of a profile name (filename-safe) */
#define LOGGER_PROFILE_MAX_PROFILES  16  /* Max profiles stored per boxcode */
#define LOGGER_PROFILE_DEFAULT_NAME  "default"  /* Migration target for legacy single-file profiles */

/* P-28: max on-apply callbacks. Per CLAUDE.md Rule 3 (no magic numbers)
 * — adjust here, not at call sites. Sized for current callers
 * (wot_logger) with headroom. */
#define LOGGER_PROFILE_MAX_ON_APPLY_CBS  4

/**
 * Initialize the profile system. Creates the profiles directory if needed.
 * Must be called after fs_manager_mount_partition(FS_PARTITION_STORAGE).
 */
esp_err_t logger_profile_init(void);

/**
 * P-75: named profile API. Storage layout is per-boxcode subdir:
 *   /cal/profiles/<boxcode>/<name>.json   — one file per named profile
 *   /cal/profiles/<boxcode>/.active       — plain-text name of active profile
 *
 * Legacy single-file layout (/cal/profiles/<boxcode>.json) is migrated
 * to <boxcode>/<LOGGER_PROFILE_DEFAULT_NAME>.json on first apply.
 */

/**
 * Validate a profile name. Accepts [A-Za-z0-9_-]{1,LOGGER_PROFILE_NAME_MAX_LEN}.
 * Rejects empty strings, NULL, paths containing dots / slashes, anything
 * else. Returns true when safe to use as a filename component.
 */
bool logger_profile_name_is_valid(const char *name);

/**
 * Save a named profile + mark it active.
 *
 * @param boxcode       Boxcode string (e.g. "4K0907557G__0003")
 * @param name          Profile name (validated; see logger_profile_name_is_valid)
 * @param var_names     Array of variable name strings to save
 * @param var_count     Number of entries in var_names
 */
esp_err_t logger_profile_save(const char *boxcode,
                               const char *name,
                               const char **var_names,
                               uint8_t var_count);

/**
 * Load a named profile's variable list.
 *
 * @param boxcode       Boxcode string
 * @param name          Profile name to load (NULL/"" → load active)
 * @param var_names     Output buffer array (caller allocates [][32])
 * @param max_vars      Size of the var_names outer array
 * @param var_count     Output: number of variables loaded
 * @return ESP_OK if profile exists, ESP_ERR_NOT_FOUND if no such profile,
 *         ESP_ERR_INVALID_ARG on bad name
 */
esp_err_t logger_profile_load(const char *boxcode,
                               const char *name,
                               char var_names[][LOGGER_PROFILE_NAME_MAX_LEN],
                               uint8_t max_vars,
                               uint8_t *var_count);

/**
 * Delete a named profile. If it was active, the active marker is
 * cleared (next apply falls back to defaults).
 */
esp_err_t logger_profile_delete(const char *boxcode, const char *name);

/**
 * Rename a profile. If old_name was active, active marker tracks
 * the new name.
 */
esp_err_t logger_profile_rename(const char *boxcode,
                                 const char *old_name,
                                 const char *new_name);

/**
 * Mark a named profile as active. The next can_task apply cycle
 * (triggered by logger_manager_force_reconfigure) loads from it.
 */
esp_err_t logger_profile_set_active(const char *boxcode, const char *name);

/**
 * Read the currently active profile name into name_out (NUL-terminated).
 * Returns ESP_ERR_NOT_FOUND if no active marker is set.
 */
esp_err_t logger_profile_get_active(const char *boxcode,
                                     char *name_out,
                                     size_t name_max);

/**
 * Enumerate stored profile names for a boxcode (no var data).
 *
 * @param boxcode       Boxcode string
 * @param names_out     Output buffer array (caller allocates [][LOGGER_PROFILE_NAME_MAX_LEN])
 * @param max_names     Capacity of names_out
 * @param count_out     Output: number of names written
 */
esp_err_t logger_profile_list(const char *boxcode,
                               char names_out[][LOGGER_PROFILE_NAME_MAX_LEN],
                               uint8_t max_names,
                               uint8_t *count_out);

/**
 * True if at least one named profile exists for the boxcode (including
 * legacy single-file form that hasn't been migrated yet).
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

/**
 * P-72: pin logger_profile_apply() to a single owner task so the
 * apply path (clear + add required + load file + add saved) cannot
 * race with itself across tasks. The can_task — which is the sole
 * caller via connection_manager_update() → handle_check_logger_config()
 * — calls this once at task start. Any subsequent apply() invocation
 * from another task (e.g. a misguided WS command handler) is rejected
 * with an ESP_LOGE and returns false. WS handlers must instead call
 * logger_manager_force_reconfigure() and let the owner re-apply on
 * its next state-machine tick.
 *
 * Set s_owner_task to NULL to disable the check (host-test builds).
 */
void logger_profile_set_owner_task(TaskHandle_t owner);

/**
 * Callback fired after logger_profile_apply() successfully populates
 * logger_manager. Modules that need a populated logger_manager
 * (e.g. wot_logger's recorder, which snapshots variables_per_sample
 * at init time) register here from their own init() and complete
 * late-stage setup inside the callback. Called in registration order.
 *
 * Added 2026-05-21 to fix P-28 — wot_logger_init() ran at boot before
 * any logger profile existed, so the recorder init failed with 0 vars
 * and FEATURE_WOT_LOGGING never registered.
 *
 * @param boxcode  The boxcode whose profile was just applied.
 *                 NUL-terminated; valid only for the duration of the call.
 */
typedef void (*logger_profile_on_apply_fn_t)(const char *boxcode);

/**
 * Register an on-apply callback. Returns ESP_ERR_INVALID_ARG on NULL,
 * ESP_ERR_NO_MEM if the registry is full
 * (LOGGER_PROFILE_MAX_ON_APPLY_CBS).
 */
esp_err_t logger_profile_register_on_apply(logger_profile_on_apply_fn_t cb);

#endif // LOGGER_PROFILE_H
