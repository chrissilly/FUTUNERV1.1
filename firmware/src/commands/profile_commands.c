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

    /* P-75: include the active-named-profile pointer so the UI's Load
     * dropdown can pre-select the right entry on render. Empty string
     * if no .active marker (logger is running defaults). */
    char active_name[LOGGER_PROFILE_NAME_MAX_LEN + 1] = {0};
    logger_profile_get_active(config->boxcode, active_name, sizeof(active_name));
    cJSON_AddStringToObject(root, "active_name", active_name);

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

    /* P-75: name is REQUIRED. UI prompts for it; firmware validates. */
    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name_item) || !logger_profile_name_is_valid(name_item->valuestring)) {
        cJSON_Delete(root);
        snprintf(response, response_size,
                 "Missing or invalid 'name' (must match [A-Za-z0-9_-]{1,32})");
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

    /* Save by name (writes file + marks active). Must run before
     * cJSON_Delete because var_names[] point into the cJSON pool. */
    esp_err_t err = logger_profile_save(config->boxcode,
                                         name_item->valuestring,
                                         var_names, var_count);

    /* Stash the name so we can echo it back after cJSON_Delete. */
    char saved_name[LOGGER_PROFILE_NAME_MAX_LEN + 1];
    strncpy(saved_name, name_item->valuestring, LOGGER_PROFILE_NAME_MAX_LEN);
    saved_name[LOGGER_PROFILE_NAME_MAX_LEN] = '\0';

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
    cJSON_AddStringToObject(resp, "active", saved_name);
    cJSON_AddNumberToObject(resp, "saved_count", var_count);
    cJSON_AddNumberToObject(resp, "invalid_count", invalid_count);
    /* P-72: total_active dropped from the response. With deferred
     * apply, the count at this point is the pre-save value; reading
     * it would mislead the UI. UI consumes only saved_count +
     * invalid_count + active. */

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

    const boxcode_config_t *config = logger_variables_get_current_config();
    if (!config) {
        snprintf(response, response_size, "No boxcode selected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!params) {
        snprintf(response, response_size, "Missing 'name' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(params);
    if (!root) {
        snprintf(response, response_size, "Invalid JSON parameters");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name_item) || !logger_profile_name_is_valid(name_item->valuestring)) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Missing or invalid 'name'");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t rc = logger_profile_delete(config->boxcode, name_item->valuestring);
    cJSON_Delete(root);
    if (rc != ESP_OK) {
        snprintf(response, response_size, "{\"status\":\"error\",\"message\":\"Profile not found or delete failed\"}");
        return ESP_FAIL;
    }
    /* P-72: defer apply. If the deleted profile was active, .active
     * marker is cleared by logger_profile_delete and apply falls
     * through to "all optional vars" defaults. */
    logger_manager_force_reconfigure();
    snprintf(response, response_size, "{\"status\":\"success\"}");
    return ESP_OK;
}

/* ── list_logger_profiles (P-75) ──────────────────────────────────── */

esp_err_t cmd_list_logger_profiles(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    const boxcode_config_t *config = logger_variables_get_current_config();
    if (!config) {
        snprintf(response, response_size,
                 "{\"status\":\"error\",\"message\":\"No boxcode selected\"}");
        return ESP_OK;
    }

    char names[LOGGER_PROFILE_MAX_PROFILES][LOGGER_PROFILE_NAME_MAX_LEN];
    uint8_t name_count = 0;
    logger_profile_list(config->boxcode, names, LOGGER_PROFILE_MAX_PROFILES, &name_count);

    char active[LOGGER_PROFILE_NAME_MAX_LEN + 1] = {0};
    logger_profile_get_active(config->boxcode, active, sizeof(active));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "boxcode", config->boxcode);
    cJSON_AddStringToObject(root, "active", active);

    cJSON *arr = cJSON_CreateArray();
    char vars[LOGGER_PROFILE_MAX_SELECTED][LOGGER_PROFILE_NAME_MAX_LEN];
    uint8_t var_count;
    for (uint8_t i = 0; i < name_count; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", names[i]);
        var_count = 0;
        if (logger_profile_load(config->boxcode, names[i], vars,
                                 LOGGER_PROFILE_MAX_SELECTED, &var_count) == ESP_OK) {
            cJSON *varr = cJSON_CreateArray();
            for (uint8_t v = 0; v < var_count; v++) {
                cJSON_AddItemToArray(varr, cJSON_CreateString(vars[v]));
            }
            cJSON_AddItemToObject(p, "vars", varr);
        } else {
            cJSON_AddItemToObject(p, "vars", cJSON_CreateArray());
        }
        cJSON_AddItemToArray(arr, p);
    }
    cJSON_AddItemToObject(root, "profiles", arr);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) {
        strncpy(response, s, response_size - 1);
        response[response_size - 1] = '\0';
        free(s);
    }
    return ESP_OK;
}

/* ── load_logger_profile (P-75) — set active + reconfigure ───────── */

esp_err_t cmd_load_logger_profile(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;

    const boxcode_config_t *config = logger_variables_get_current_config();
    if (!config) {
        snprintf(response, response_size, "No boxcode selected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!params) {
        snprintf(response, response_size, "Missing 'name' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(params);
    if (!root) {
        snprintf(response, response_size, "Invalid JSON parameters");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name_item) || !logger_profile_name_is_valid(name_item->valuestring)) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Missing or invalid 'name'");
        return ESP_ERR_INVALID_ARG;
    }
    /* Verify the named profile exists + capture its vars so we can
     * include them in the response. UI uses these directly to tick
     * the Logged checkboxes — avoids racing with the deferred apply
     * via a follow-up get_logger_profile that may land mid-clear. */
    char vars[LOGGER_PROFILE_MAX_SELECTED][LOGGER_PROFILE_NAME_MAX_LEN];
    uint8_t vc = 0;
    if (logger_profile_load(config->boxcode, name_item->valuestring,
                             vars, LOGGER_PROFILE_MAX_SELECTED, &vc) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(response, response_size,
                 "{\"status\":\"error\",\"message\":\"Named profile not found\"}");
        return ESP_FAIL;
    }
    esp_err_t rc = logger_profile_set_active(config->boxcode, name_item->valuestring);
    char active_name[LOGGER_PROFILE_NAME_MAX_LEN + 1];
    strncpy(active_name, name_item->valuestring, LOGGER_PROFILE_NAME_MAX_LEN);
    active_name[LOGGER_PROFILE_NAME_MAX_LEN] = '\0';
    cJSON_Delete(root);
    if (rc != ESP_OK) {
        snprintf(response, response_size, "Failed to set active");
        return ESP_FAIL;
    }
    logger_manager_force_reconfigure();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON_AddStringToObject(resp, "active", active_name);
    cJSON *varr = cJSON_CreateArray();
    for (uint8_t i = 0; i < vc; i++) cJSON_AddItemToArray(varr, cJSON_CreateString(vars[i]));
    cJSON_AddItemToObject(resp, "vars", varr);
    /* Required vars are always included regardless of the saved profile —
     * surface them too so the UI can render the full Logged-tick set
     * without a follow-up round-trip. */
    cJSON *rreq = cJSON_CreateArray();
    for (uint8_t i = 0; i < config->variable_count; i++) {
        if (config->variables[i].is_required) {
            cJSON_AddItemToArray(rreq, cJSON_CreateString(config->variables[i].name));
        }
    }
    cJSON_AddItemToObject(resp, "required", rreq);
    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (s) {
        strncpy(response, s, response_size - 1);
        response[response_size - 1] = '\0';
        free(s);
    }
    return ESP_OK;
}

/* ── rename_logger_profile (P-75) ─────────────────────────────────── */

esp_err_t cmd_rename_logger_profile(int fd, const char *params, char *response, size_t response_size) {
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
    cJSON *o = cJSON_GetObjectItem(root, "old_name");
    cJSON *n = cJSON_GetObjectItem(root, "new_name");
    if (!cJSON_IsString(o) || !cJSON_IsString(n) ||
        !logger_profile_name_is_valid(o->valuestring) ||
        !logger_profile_name_is_valid(n->valuestring)) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Missing or invalid 'old_name' / 'new_name'");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t rc = logger_profile_rename(config->boxcode, o->valuestring, n->valuestring);
    char new_name[LOGGER_PROFILE_NAME_MAX_LEN + 1];
    strncpy(new_name, n->valuestring, LOGGER_PROFILE_NAME_MAX_LEN);
    new_name[LOGGER_PROFILE_NAME_MAX_LEN] = '\0';
    cJSON_Delete(root);
    if (rc == ESP_ERR_NOT_FOUND) {
        snprintf(response, response_size,
                 "{\"status\":\"error\",\"message\":\"Source profile not found\"}");
        return ESP_FAIL;
    }
    if (rc == ESP_ERR_INVALID_STATE) {
        snprintf(response, response_size,
                 "{\"status\":\"error\",\"message\":\"Target name already exists\"}");
        return ESP_FAIL;
    }
    if (rc != ESP_OK) {
        snprintf(response, response_size, "Rename failed");
        return ESP_FAIL;
    }
    snprintf(response, response_size, "{\"status\":\"success\",\"new_name\":\"%s\"}", new_name);
    return ESP_OK;
}
