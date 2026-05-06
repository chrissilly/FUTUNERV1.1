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

static const char *TAG = "LOGGER_PROFILE";

/* Maximum JSON file size for a profile (64 var names × ~32 chars + overhead) */
#define PROFILE_MAX_FILE_SIZE  4096

/**
 * Build the full filesystem path for a boxcode's profile.
 * Result: "/cal/profiles/<boxcode>.json"
 */
static void build_profile_path(const char *boxcode, char *path, size_t path_len) {
    snprintf(path, path_len, "/cal/%s/%s.json", LOGGER_PROFILE_DIR, boxcode);
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
                               const char **var_names,
                               uint8_t var_count) {
    if (!boxcode || !var_names) {
        return ESP_ERR_INVALID_ARG;
    }

    if (var_count > LOGGER_PROFILE_MAX_SELECTED) {
        ESP_LOGW(TAG, "Clamping var_count from %d to %d", var_count, LOGGER_PROFILE_MAX_SELECTED);
        var_count = LOGGER_PROFILE_MAX_SELECTED;
    }

    /* Build JSON: {"vars":["name1","name2",...]} */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }

    cJSON *arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < var_count; i++) {
        if (var_names[i] && strlen(var_names[i]) > 0) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(var_names[i]));
        }
    }
    cJSON_AddItemToObject(root, "vars", arr);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_ERR_NO_MEM;
    }

    /* Write to file */
    char path[128];
    build_profile_path(boxcode, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open profile for writing: %s", path);
        free(json_str);
        return ESP_FAIL;
    }

    size_t json_len = strlen(json_str);
    size_t written = fwrite(json_str, 1, json_len, f);
    fclose(f);
    free(json_str);

    if (written != json_len) {
        ESP_LOGE(TAG, "Profile write incomplete: %d/%d bytes", (int)written, (int)json_len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved profile for %s: %d optional variables (%d bytes)",
             boxcode, var_count, (int)json_len);
    return ESP_OK;
}

esp_err_t logger_profile_load(const char *boxcode,
                               char var_names[][32],
                               uint8_t max_vars,
                               uint8_t *var_count) {
    if (!boxcode || !var_names || !var_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *var_count = 0;

    char path[128];
    build_profile_path(boxcode, path, sizeof(path));

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
            ESP_LOGW(TAG, "Profile truncated at %d variables (max %d)", *var_count, max_vars);
            break;
        }
        if (cJSON_IsString(item) && item->valuestring) {
            strncpy(var_names[*var_count], item->valuestring, 31);
            var_names[*var_count][31] = '\0';
            (*var_count)++;
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Loaded profile for %s: %d optional variables", boxcode, *var_count);
    return ESP_OK;
}

esp_err_t logger_profile_delete(const char *boxcode) {
    if (!boxcode) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    build_profile_path(boxcode, path, sizeof(path));

    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "Failed to delete profile: %s (may not exist)", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted profile for %s", boxcode);
    return ESP_OK;
}

bool logger_profile_exists(const char *boxcode) {
    if (!boxcode) return false;

    char path[128];
    build_profile_path(boxcode, path, sizeof(path));

    struct stat st;
    return (stat(path, &st) == 0);
}

bool logger_profile_apply(const char *boxcode) {
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

    /* Load saved profile */
    char saved_vars[LOGGER_PROFILE_MAX_SELECTED][32];
    uint8_t saved_count = 0;

    esp_err_t err = logger_profile_load(boxcode, saved_vars, LOGGER_PROFILE_MAX_SELECTED, &saved_count);

    if (err == ESP_ERR_NOT_FOUND) {
        /* No saved profile — add all optional variables as default */
        ESP_LOGI(TAG, "No saved profile for %s, using all available optional variables", boxcode);
        for (uint8_t i = 0; i < config->variable_count; i++) {
            if (!config->variables[i].is_required) {
                logger_variables_add_by_name(config->variables[i].name);
            }
        }
        return true;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load profile, using required-only");
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
    return true;
}
