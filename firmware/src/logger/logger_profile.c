#include "logger_profile.h"
#include "logger_variables.h"
#include "logger_manager.h"
#include "filesystem/fs_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>

static const char *TAG = "LOGGER_PROFILE";

/* Maximum JSON file size for a profile (64 var names × ~32 chars + overhead) */
#define PROFILE_MAX_FILE_SIZE  4096

/* P-28: on-apply callback registry. Modules that need a populated
 * logger_manager (e.g. wot_logger's recorder, which snapshots the
 * variable list at init time) register here from their own init() and
 * complete late-stage setup inside the callback. Cap from
 * logger_profile.h. */
static logger_profile_on_apply_fn_t s_on_apply_cbs[LOGGER_PROFILE_MAX_ON_APPLY_CBS];
static uint8_t s_on_apply_count = 0;

/* P-72: task-affinity guard. Apply mutates shared logger_manager
 * state; concurrent calls from WS task + can_task interleave clear/
 * add operations and produce duplicate entries that take a full
 * CONN_MGR cascade to settle. Pin to the can_task; any caller from
 * another context is rejected at the door so the bug surfaces in
 * the serial log instead of corrupting state. NULL = guard
 * disabled (host-test). */
static TaskHandle_t s_owner_task = NULL;

void logger_profile_set_owner_task(TaskHandle_t owner) {
    s_owner_task = owner;
    ESP_LOGI(TAG, "apply() owner pinned to task %p", (void *)owner);
}

esp_err_t logger_profile_register_on_apply(logger_profile_on_apply_fn_t cb) {
    if (cb == NULL) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < s_on_apply_count; i++) {
        if (s_on_apply_cbs[i] == cb) return ESP_OK;  /* idempotent */
    }
    if (s_on_apply_count >= LOGGER_PROFILE_MAX_ON_APPLY_CBS) {
        ESP_LOGE(TAG, "on_apply callback registry full (cap=%d)",
                 LOGGER_PROFILE_MAX_ON_APPLY_CBS);
        return ESP_ERR_NO_MEM;
    }
    s_on_apply_cbs[s_on_apply_count++] = cb;
    return ESP_OK;
}

static void fire_on_apply(const char *boxcode) {
    for (uint8_t i = 0; i < s_on_apply_count; i++) {
        if (s_on_apply_cbs[i] != NULL) s_on_apply_cbs[i](boxcode);
    }
}

/* P-75 path helpers — per-boxcode subdir contains named profile files
 * plus a ".active" marker. Legacy /cal/profiles/<boxcode>.json is
 * migrated on first apply via migrate_legacy_profile_if_needed(). */

static void build_boxcode_dir(const char *boxcode, char *path, size_t n) {
    snprintf(path, n, "/cal/%s/%s", LOGGER_PROFILE_DIR, boxcode);
}

static void build_named_profile_path(const char *boxcode, const char *name,
                                      char *path, size_t n) {
    snprintf(path, n, "/cal/%s/%s/%s.json", LOGGER_PROFILE_DIR, boxcode, name);
}

static void build_active_marker_path(const char *boxcode, char *path, size_t n) {
    snprintf(path, n, "/cal/%s/%s/.active", LOGGER_PROFILE_DIR, boxcode);
}

static void build_legacy_profile_path(const char *boxcode, char *path, size_t n) {
    snprintf(path, n, "/cal/%s/%s.json", LOGGER_PROFILE_DIR, boxcode);
}

static esp_err_t ensure_boxcode_dir(const char *boxcode) {
    char dir[160];
    build_boxcode_dir(boxcode, dir, sizeof(dir));
    struct stat st;
    if (stat(dir, &st) == 0) return ESP_OK;
    if (mkdir(dir, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to mkdir %s", dir);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Created profile dir %s", dir);
    return ESP_OK;
}

bool logger_profile_name_is_valid(const char *name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len == 0 || len > LOGGER_PROFILE_NAME_MAX_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                  || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/* Migrate legacy /cal/profiles/<boxcode>.json into
 * <boxcode>/<LOGGER_PROFILE_DEFAULT_NAME>.json + .active = default.
 * Idempotent: no-op if either the subdir already exists OR the legacy
 * file is absent. */
static void migrate_legacy_profile_if_needed(const char *boxcode) {
    char dir[160];
    build_boxcode_dir(boxcode, dir, sizeof(dir));
    struct stat st;
    if (stat(dir, &st) == 0) return;  /* Subdir already exists, nothing to migrate. */
    char legacy[160];
    build_legacy_profile_path(boxcode, legacy, sizeof(legacy));
    if (stat(legacy, &st) != 0) return;  /* No legacy file. */
    if (ensure_boxcode_dir(boxcode) != ESP_OK) return;
    char dest[160];
    build_named_profile_path(boxcode, LOGGER_PROFILE_DEFAULT_NAME, dest, sizeof(dest));
    /* rename() is atomic on the same FS; LittleFS supports it. */
    if (rename(legacy, dest) != 0) {
        ESP_LOGW(TAG, "Legacy migrate rename failed (%s -> %s)", legacy, dest);
        return;
    }
    char active_path[160];
    build_active_marker_path(boxcode, active_path, sizeof(active_path));
    FILE *f = fopen(active_path, "wb");
    if (f) {
        fputs(LOGGER_PROFILE_DEFAULT_NAME, f);
        fclose(f);
    }
    ESP_LOGI(TAG, "P-75 migrated legacy profile -> %s (active=%s)",
             dest, LOGGER_PROFILE_DEFAULT_NAME);
}

esp_err_t logger_profile_init(void) {
    if (!fs_manager_is_mounted(FS_PARTITION_STORAGE)) {
        ESP_LOGE(TAG, "Storage partition not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create profiles directory if it doesn't exist */
    char dir_path[128];
    snprintf(dir_path, sizeof(dir_path), "/cal/%s", LOGGER_PROFILE_DIR);

    struct stat st;
    if (stat(dir_path, &st) != 0) {
        ESP_LOGI(TAG, "Creating profiles directory: %s", dir_path);
        if (mkdir(dir_path, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create profiles directory");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Logger profile system initialized");
    return ESP_OK;
}

esp_err_t logger_profile_save(const char *boxcode,
                               const char *name,
                               const char **var_names,
                               uint8_t var_count) {
    if (!boxcode || !var_names) return ESP_ERR_INVALID_ARG;
    if (!logger_profile_name_is_valid(name)) return ESP_ERR_INVALID_ARG;

    if (var_count > LOGGER_PROFILE_MAX_SELECTED) {
        ESP_LOGW(TAG, "Clamping var_count %d -> %d", var_count, LOGGER_PROFILE_MAX_SELECTED);
        var_count = LOGGER_PROFILE_MAX_SELECTED;
    }

    migrate_legacy_profile_if_needed(boxcode);
    if (ensure_boxcode_dir(boxcode) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "name", name);
    cJSON *arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < var_count; i++) {
        if (var_names[i] && strlen(var_names[i]) > 0) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(var_names[i]));
        }
    }
    cJSON_AddItemToObject(root, "vars", arr);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    char path[160];
    build_named_profile_path(boxcode, name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open for write failed: %s", path);
        free(json_str);
        return ESP_FAIL;
    }
    size_t json_len = strlen(json_str);
    size_t written = fwrite(json_str, 1, json_len, f);
    fclose(f);
    free(json_str);
    if (written != json_len) {
        ESP_LOGE(TAG, "Profile write short: %d/%d", (int)written, (int)json_len);
        return ESP_FAIL;
    }

    /* Mark active. Save-with-name == load-this-name-from-now-on. */
    esp_err_t a = logger_profile_set_active(boxcode, name);
    if (a != ESP_OK) {
        ESP_LOGW(TAG, "saved %s but set_active failed (rc=%d)", name, a);
    }
    ESP_LOGI(TAG, "Saved profile '%s' for %s: %d vars (%d bytes)",
             name, boxcode, var_count, (int)json_len);
    return ESP_OK;
}

esp_err_t logger_profile_load(const char *boxcode,
                               const char *name,
                               char var_names[][LOGGER_PROFILE_NAME_MAX_LEN],
                               uint8_t max_vars,
                               uint8_t *var_count) {
    if (!boxcode || !var_names || !var_count) return ESP_ERR_INVALID_ARG;
    *var_count = 0;

    /* Resolve name. NULL/empty → use active marker. */
    char resolved[LOGGER_PROFILE_NAME_MAX_LEN + 1] = {0};
    if (!name || name[0] == '\0') {
        esp_err_t r = logger_profile_get_active(boxcode, resolved, sizeof(resolved));
        if (r != ESP_OK) return ESP_ERR_NOT_FOUND;
        name = resolved;
    }
    if (!logger_profile_name_is_valid(name)) return ESP_ERR_INVALID_ARG;

    char path[160];
    build_named_profile_path(boxcode, name, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGD(TAG, "No profile found for %s", boxcode);
        return ESP_ERR_NOT_FOUND;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > PROFILE_MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "Invalid profile file size: %ld", file_size);
        fclose(f);
        return ESP_FAIL;
    }

    /* Read file into heap buffer */
    char *json_buf = malloc(file_size + 1);
    if (!json_buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(json_buf, 1, file_size, f);
    fclose(f);
    json_buf[read_bytes] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse profile JSON for %s", boxcode);
        return ESP_FAIL;
    }

    cJSON *vars_arr = cJSON_GetObjectItem(root, "vars");
    if (!cJSON_IsArray(vars_arr)) {
        ESP_LOGE(TAG, "Profile missing 'vars' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, vars_arr) {
        if (*var_count >= max_vars) {
            ESP_LOGW(TAG, "Profile truncated at %d (max %d)", *var_count, max_vars);
            break;
        }
        if (cJSON_IsString(item) && item->valuestring) {
            strncpy(var_names[*var_count], item->valuestring, LOGGER_PROFILE_NAME_MAX_LEN - 1);
            var_names[*var_count][LOGGER_PROFILE_NAME_MAX_LEN - 1] = '\0';
            (*var_count)++;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded profile '%s' for %s: %d vars", name, boxcode, *var_count);
    return ESP_OK;
}

/* P-75 set_active: write "<name>" into <boxcode>/.active. */
esp_err_t logger_profile_set_active(const char *boxcode, const char *name) {
    if (!boxcode || !name) return ESP_ERR_INVALID_ARG;
    if (!logger_profile_name_is_valid(name)) return ESP_ERR_INVALID_ARG;
    if (ensure_boxcode_dir(boxcode) != ESP_OK) return ESP_FAIL;

    char path[160];
    build_active_marker_path(boxcode, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t n = strlen(name);
    size_t w = fwrite(name, 1, n, f);
    fclose(f);
    if (w != n) return ESP_FAIL;
    ESP_LOGI(TAG, "Active profile for %s = '%s'", boxcode, name);
    return ESP_OK;
}

/* P-75 get_active: read <boxcode>/.active into name_out (NUL-terminated). */
esp_err_t logger_profile_get_active(const char *boxcode, char *name_out, size_t name_max) {
    if (!boxcode || !name_out || name_max == 0) return ESP_ERR_INVALID_ARG;
    name_out[0] = '\0';
    char path[160];
    build_active_marker_path(boxcode, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t n = fread(name_out, 1, name_max - 1, f);
    fclose(f);
    name_out[n] = '\0';
    /* Trim trailing whitespace/newlines defensively. */
    while (n > 0 && (name_out[n-1] == '\n' || name_out[n-1] == '\r' || name_out[n-1] == ' ')) {
        name_out[--n] = '\0';
    }
    if (n == 0 || !logger_profile_name_is_valid(name_out)) {
        ESP_LOGW(TAG, "Active marker for %s is empty / invalid", boxcode);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static void clear_active_marker(const char *boxcode) {
    char path[160];
    build_active_marker_path(boxcode, path, sizeof(path));
    unlink(path);  /* Best-effort; missing marker == "use defaults". */
}

esp_err_t logger_profile_delete(const char *boxcode, const char *name) {
    if (!boxcode || !logger_profile_name_is_valid(name)) return ESP_ERR_INVALID_ARG;

    char path[160];
    build_named_profile_path(boxcode, name, path, sizeof(path));
    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "delete %s failed (may not exist)", path);
        return ESP_FAIL;
    }

    /* If the deleted profile was active, clear the marker so apply
     * falls back to defaults rather than chasing a dead reference. */
    char active[LOGGER_PROFILE_NAME_MAX_LEN + 1] = {0};
    if (logger_profile_get_active(boxcode, active, sizeof(active)) == ESP_OK &&
        strcmp(active, name) == 0) {
        clear_active_marker(boxcode);
        ESP_LOGI(TAG, "Cleared active marker (was '%s')", name);
    }
    ESP_LOGI(TAG, "Deleted profile '%s' for %s", name, boxcode);
    return ESP_OK;
}

esp_err_t logger_profile_rename(const char *boxcode,
                                 const char *old_name,
                                 const char *new_name) {
    if (!boxcode) return ESP_ERR_INVALID_ARG;
    if (!logger_profile_name_is_valid(old_name)) return ESP_ERR_INVALID_ARG;
    if (!logger_profile_name_is_valid(new_name)) return ESP_ERR_INVALID_ARG;
    if (strcmp(old_name, new_name) == 0) return ESP_OK;

    char old_path[160], new_path[160];
    build_named_profile_path(boxcode, old_name, old_path, sizeof(old_path));
    build_named_profile_path(boxcode, new_name, new_path, sizeof(new_path));

    struct stat st;
    if (stat(old_path, &st) != 0) return ESP_ERR_NOT_FOUND;
    if (stat(new_path, &st) == 0) {
        ESP_LOGW(TAG, "rename target '%s' already exists", new_name);
        return ESP_ERR_INVALID_STATE;
    }
    if (rename(old_path, new_path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed", old_path, new_path);
        return ESP_FAIL;
    }
    /* If active pointed at old name, update it to track. */
    char active[LOGGER_PROFILE_NAME_MAX_LEN + 1] = {0};
    if (logger_profile_get_active(boxcode, active, sizeof(active)) == ESP_OK &&
        strcmp(active, old_name) == 0) {
        logger_profile_set_active(boxcode, new_name);
    }
    ESP_LOGI(TAG, "Renamed profile %s: '%s' -> '%s'", boxcode, old_name, new_name);
    return ESP_OK;
}

esp_err_t logger_profile_list(const char *boxcode,
                               char names_out[][LOGGER_PROFILE_NAME_MAX_LEN],
                               uint8_t max_names,
                               uint8_t *count_out) {
    if (!boxcode || !names_out || !count_out) return ESP_ERR_INVALID_ARG;
    *count_out = 0;

    migrate_legacy_profile_if_needed(boxcode);

    char dir_path[160];
    build_boxcode_dir(boxcode, dir_path, sizeof(dir_path));

    DIR *d = opendir(dir_path);
    if (!d) return ESP_OK;  /* No directory == no profiles, not an error. */

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && *count_out < max_names) {
        if (ent->d_name[0] == '.') continue;       /* Skip .active + . + .. */
        size_t n = strlen(ent->d_name);
        if (n < 6) continue;                       /* Need at least ".json" + 1 char */
        if (strcmp(ent->d_name + n - 5, ".json") != 0) continue;
        /* Strip ".json" suffix to recover the profile name. */
        size_t bare = n - 5;
        if (bare >= LOGGER_PROFILE_NAME_MAX_LEN) continue;
        memcpy(names_out[*count_out], ent->d_name, bare);
        names_out[*count_out][bare] = '\0';
        if (!logger_profile_name_is_valid(names_out[*count_out])) continue;
        (*count_out)++;
    }
    closedir(d);
    return ESP_OK;
}

bool logger_profile_exists(const char *boxcode) {
    if (!boxcode) return false;

    /* Quick check: does the boxcode subdir hold at least one .json file? */
    char names[1][LOGGER_PROFILE_NAME_MAX_LEN];
    uint8_t cnt = 0;
    if (logger_profile_list(boxcode, names, 1, &cnt) == ESP_OK && cnt > 0) return true;

    /* Legacy fallback (pre-migration) */
    char legacy[160];
    build_legacy_profile_path(boxcode, legacy, sizeof(legacy));
    struct stat st;
    return (stat(legacy, &st) == 0);
}

bool logger_profile_apply(const char *boxcode) {
    /* P-72 task-affinity guard. */
    if (s_owner_task != NULL) {
        TaskHandle_t cur = xTaskGetCurrentTaskHandle();
        if (cur != s_owner_task) {
            ESP_LOGE(TAG, "logger_profile_apply called from non-owner "
                          "task (cur=%p owner=%p) — RACE; "
                          "use logger_manager_force_reconfigure()",
                     (void *)cur, (void *)s_owner_task);
            return false;
        }
    }

    if (!boxcode) {
        ESP_LOGE(TAG, "No boxcode provided");
        return false;
    }

    const boxcode_config_t *config = logger_variables_get_current_config();
    if (!config) {
        ESP_LOGE(TAG, "No boxcode config set");
        return false;
    }

    /* Always start with required variables */
    logger_manager_clear_variables();

    if (!logger_variables_add_all_required()) {
        ESP_LOGE(TAG, "Failed to add required variables");
        return false;
    }

    /* P-75: migrate legacy single-file layout the FIRST time apply runs
     * after upgrade. After this, all loads go through named-profile
     * paths. Cheap no-op once the subdir exists. */
    migrate_legacy_profile_if_needed(boxcode);

    /* Load whichever profile the active marker points at. Passing
     * name=NULL tells load() to resolve from .active. */
    char saved_vars[LOGGER_PROFILE_MAX_SELECTED][LOGGER_PROFILE_NAME_MAX_LEN];
    uint8_t saved_count = 0;

    esp_err_t err = logger_profile_load(boxcode, NULL, saved_vars,
                                         LOGGER_PROFILE_MAX_SELECTED, &saved_count);

    if (err == ESP_ERR_NOT_FOUND) {
        /* No saved profile — add all optional variables as default */
        ESP_LOGI(TAG, "No saved profile for %s, using all available optional variables", boxcode);
        for (uint8_t i = 0; i < config->variable_count; i++) {
            if (!config->variables[i].is_required) {
                logger_variables_add_by_name(config->variables[i].name);
            }
        }
        fire_on_apply(boxcode);
        return true;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load profile, using required-only");
        fire_on_apply(boxcode);
        return true;  /* Still functional with just required vars */
    }

    /* Apply saved optional variables */
    uint8_t applied = 0;
    for (uint8_t i = 0; i < saved_count; i++) {
        if (logger_variables_add_by_name(saved_vars[i])) {
            applied++;
        } else {
            ESP_LOGW(TAG, "Profile variable '%s' not found in catalog — skipping", saved_vars[i]);
        }
    }

    ESP_LOGI(TAG, "Applied profile for %s: %d required + %d optional (%d from profile)",
             boxcode,
             (int)(logger_manager_get_variable_count() - applied),
             applied, saved_count);
    fire_on_apply(boxcode);
    return true;
}
