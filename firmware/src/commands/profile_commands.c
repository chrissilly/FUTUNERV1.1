#include "profile_commands.h"
#include "logger/logger_variables.h"
#include "logger/logger_profile.h"
#include "logger/logger_manager.h"
#include "state_machine/connection_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "PROFILE_CMD";

/* ── list_available_vars ──────────────────────────────────────────── */

esp_err_t cmd_list_available_vars(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    const boxcode_config_t *config = logger_variables_get_current_config();
    if (!config) {
        snprintf(response, response_size,
                 "{\"status\":\"error\",\"message\":\"No boxcode selected — connect to ECU first\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "boxcode", config->boxcode);

    cJSON *vars_arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < config->variable_count; i++) {
        const logger_variable_def_t *v = &config->variables[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", v->name);
        cJSON_AddStringToObject(obj, "display_name", v->display_name);
        cJSON_AddStringToObject(obj, "unit", v->unit);
        cJSON_AddNumberToObject(obj, "size", v->size);
        cJSON_AddBoolToObject(obj, "required", v->is_required);
        cJSON_AddBoolToObject(obj, "signed", v->is_signed);
        cJSON_AddItemToArray(vars_arr, obj);
    }

    cJSON_AddItemToObject(root, "variables", vars_arr);
    cJSON_AddNumberToObject(root, "count", config->variable_count);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }

    return ESP_OK;
}

/* ── get_logger_profile ───────────────────────────────────────────── */

esp_err_t cmd_get_logger_profile(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    const boxcode_config_t *config = logger_variables_get_current_config();
    if (!config) {
        snprintf(response, response_size,
                 "{\"status\":\"error\",\"message\":\"No boxcode selected\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "boxcode", config->boxcode);
    cJSON_AddBoolToObject(root, "has_saved_profile", logger_profile_exists(config->boxcode));

    /* Report which optional vars are currently active in the logger */
    cJSON *selected = cJSON_CreateArray();
    uint8_t active_count = logger_manager_get_variable_count();
    for (uint8_t i = 0; i < active_count; i++) {
        const char *name = logger_manager_get_variable_name(i);
        if (!name) continue;

        /* Check if this is a required variable — skip those */
        const logger_variable_def_t *def = logger_variables_find_by_name(name);
        if (def && !def->is_required) {
            cJSON_AddItemToArray(selected, cJSON_CreateString(name));
        }
    }

    cJSON_AddItemToObject(root, "selected", selected);

    /* Also include list of required vars for reference */
    cJSON *required = cJSON_CreateArray();
    for (uint8_t i = 0; i < config->variable_count; i++) {
        if (config->variables[i].is_required) {
            cJSON_AddItemToArray(required, cJSON_CreateString(config->variables[i].name));
        }
    }
    cJSON_AddItemToObject(root, "required", required);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }

    return ESP_OK;
}

/* ── set_logger_profile ───────────────────────────────────────────── */

esp_err_t cmd_set_logger_profile(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;

    const boxcode_config_t *config = logger_variables_get_current_config();
    if (!config) {
        snprintf(response, response_size, "No boxcode selected");
        return ESP_ERR_INVALID_STATE;
    }

    if (!params) {
        snprintf(response, response_size, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(params);
    if (!root) {
        snprintf(response, response_size, "Invalid JSON parameters");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *vars_arr = cJSON_GetObjectItem(root, "variables");
    if (!cJSON_IsArray(vars_arr)) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Missing 'variables' array");
        return ESP_ERR_INVALID_ARG;
    }

    /* Collect variable names, validating each against the catalog */
    const char *var_names[LOGGER_PROFILE_MAX_SELECTED];
    uint8_t var_count = 0;
    uint8_t invalid_count = 0;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, vars_arr) {
        if (!cJSON_IsString(item) || !item->valuestring) continue;
        if (var_count >= LOGGER_PROFILE_MAX_SELECTED) break;

        /* Validate: must exist in catalog and must NOT be required */
        const logger_variable_def_t *def = logger_variables_find_by_name(item->valuestring);
        if (!def) {
            ESP_LOGW(TAG, "Ignoring unknown variable: %s", item->valuestring);
            invalid_count++;
            continue;
        }
        if (def->is_required) {
            /* Silently skip required vars — they're always included */
            continue;
        }

        var_names[var_count++] = item->valuestring;
    }

    /* Save profile to filesystem (must happen before cJSON_Delete
       because var_names[] point into the cJSON string pool) */
    esp_err_t err = logger_profile_save(config->boxcode, var_names, var_count);

    /* Now safe to free the parsed JSON */
    cJSON_Delete(root);

    if (err != ESP_OK) {
        snprintf(response, response_size, "Failed to save profile");
        return ESP_FAIL;
    }

    /* P-72: defer the apply to the can_task owner. Calling
     * logger_profile_apply() from the WS task races with can_task's
     * own apply on the CHECK_LOGGER_CONFIG branch and corrupts the
     * shared logger_manager array with duplicate inserts.
     * force_reconfigure() just flips the is_configured / needs_
     * reconfigure flags; can_task picks it up on its next state
     * tick (~1 polling cycle) and runs apply() single-threaded. */
    logger_manager_force_reconfigure();

    ESP_LOGI(TAG, "Profile updated: %d optional vars saved, %d invalid skipped",
             var_count, invalid_count);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON_AddNumberToObject(resp, "saved_count", var_count);
    cJSON_AddNumberToObject(resp, "invalid_count", invalid_count);
    /* P-72: total_active dropped from the response. With deferred
     * apply, the count at this point is the pre-save value; reading
     * it would mislead the UI. UI consumes only saved_count +
     * invalid_count (ui/control_panel.js logcfgSaveProfile callback). */

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }

    return ESP_OK;
}

/* ── delete_logger_profile ────────────────────────────────────────── */

esp_err_t cmd_delete_logger_profile(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    const boxcode_config_t *config = logger_variables_get_current_config();
    if (!config) {
        snprintf(response, response_size, "No boxcode selected");
        return ESP_ERR_INVALID_STATE;
    }

    logger_profile_delete(config->boxcode);

    /* P-72: defer apply to can_task. See cmd_set_logger_profile for
     * the race-condition writeup; same shared-state issue applies
     * here. can_task picks up needs_reconfigure on its next tick and
     * loads the (now-absent) profile, which falls through to "all
     * optional vars" defaults via logger_profile_apply's
     * ESP_ERR_NOT_FOUND branch. */
    logger_manager_force_reconfigure();

    /* total_active dropped for the same reason as in
     * cmd_set_logger_profile — would be the pre-defer count. */
    snprintf(response, response_size,
             "{\"status\":\"success\",\"message\":\"Profile deleted, using defaults\"}");
    return ESP_OK;
}
