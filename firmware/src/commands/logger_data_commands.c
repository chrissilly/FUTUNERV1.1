#include "logger_data_commands.h"
#include "state_machine/connection_manager.h"
#include "logger/logger_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "LOGGER_DATA_CMDS";

static void emit_error(cJSON *root, const char *cmd, const char *msg,
                       char *response, size_t response_size) {
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "command", cmd);
    cJSON_AddStringToObject(root, "error", msg);
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }
    cJSON_Delete(root);
}

esp_err_t cmd_get_logger_data(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    cJSON *root = cJSON_CreateObject();

    if (!connection_manager_is_connected()) {
        emit_error(root, "get_logger_data", "Not connected to ECU", response, response_size);
        return ESP_OK;
    }
    if (!connection_manager_is_patched()) {
        emit_error(root, "get_logger_data", "ECU not patched", response, response_size);
        return ESP_OK;
    }
    if (!connection_manager_is_logger_configured()) {
        emit_error(root, "get_logger_data", "Logger not configured", response, response_size);
        return ESP_OK;
    }
    if (!connection_manager_has_logger_data()) {
        emit_error(root, "get_logger_data", "No logger data received yet", response, response_size);
        return ESP_OK;
    }

    uint8_t count = logger_manager_get_variable_count();

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "command", "get_logger_data");

    /* UI expects msg.data as a flat {name: value, ...} object, not an array. */
    cJSON *data = cJSON_CreateObject();
    for (uint8_t i = 0; i < count; i++) {
        const char *name = logger_manager_get_variable_name(i);
        if (!name) continue;
        float value = logger_manager_get_variable_value(i);
        cJSON_AddNumberToObject(data, name, value);
    }
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddNumberToObject(root, "count", count);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }
    cJSON_Delete(root);

    ESP_LOGD(TAG, "Sent logger data: %d variables", count);
    return ESP_OK;
}

esp_err_t cmd_get_single_variable(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;

    cJSON *root = cJSON_CreateObject();

    if (!connection_manager_is_connected()) {
        emit_error(root, "get_single_variable", "Not connected to ECU", response, response_size);
        return ESP_OK;
    }
    if (!connection_manager_is_patched()) {
        emit_error(root, "get_single_variable", "ECU not patched", response, response_size);
        return ESP_OK;
    }
    if (!connection_manager_is_logger_configured()) {
        emit_error(root, "get_single_variable", "Logger not configured", response, response_size);
        return ESP_OK;
    }
    if (!connection_manager_has_logger_data()) {
        emit_error(root, "get_single_variable", "No logger data received yet", response, response_size);
        return ESP_OK;
    }

    cJSON *params_json = cJSON_Parse(params);
    if (!params_json) {
        emit_error(root, "get_single_variable", "Invalid JSON parameters", response, response_size);
        return ESP_OK;
    }
    cJSON *name_param = cJSON_GetObjectItem(params_json, "name");
    if (!name_param || !cJSON_IsString(name_param)) {
        cJSON_Delete(params_json);
        emit_error(root, "get_single_variable", "Missing or invalid 'name' parameter", response, response_size);
        return ESP_OK;
    }
    const char *variable_name = name_param->valuestring;

    bool found = false;
    for (uint8_t i = 0; i < logger_manager_get_variable_count(); i++) {
        const char *n = logger_manager_get_variable_name(i);
        if (n && strcmp(n, variable_name) == 0) { found = true; break; }
    }
    if (!found) {
        cJSON_Delete(params_json);
        emit_error(root, "get_single_variable", "Variable not found or not logged", response, response_size);
        return ESP_OK;
    }

    float value = logger_manager_get_variable_value_by_name(variable_name);

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "command", "get_single_variable");
    cJSON_AddStringToObject(root, "name", variable_name);
    cJSON_AddNumberToObject(root, "value", value);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }
    cJSON_Delete(params_json);
    cJSON_Delete(root);

    ESP_LOGD(TAG, "Sent single variable data: %s = %f", variable_name, value);
    return ESP_OK;
}
