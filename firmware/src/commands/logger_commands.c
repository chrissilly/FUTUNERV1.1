#include "logger_commands.h"
#include "state_machine/connection_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "LOGGER_CMD";

esp_err_t cmd_configure_logger(int fd, const char *params, char *response, size_t response_size) {
    if (!connection_manager_is_connected()) {
        snprintf(response, response_size, "Not connected to ECU");
        return ESP_ERR_INVALID_STATE;
    }

    if (!connection_manager_is_patched()) {
        snprintf(response, response_size, "ECU not patched");
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

    cJSON *variables = cJSON_GetObjectItem(root, "variables");
    if (!cJSON_IsArray(variables)) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Missing 'variables' array");
        return ESP_ERR_INVALID_ARG;
    }

    connection_manager_logger_clear_variables();

    if (!connection_manager_logger_add_all_required()) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Failed to add required variables");
        return ESP_FAIL;
    }

    int added_count = 0;
    cJSON *var = NULL;
    cJSON_ArrayForEach(var, variables) {
        if (cJSON_IsString(var)) {
            if (connection_manager_logger_add_variable_by_name(var->valuestring)) {
                added_count++;
            }
        }
    }

    cJSON_Delete(root);

    uint8_t total = connection_manager_logger_get_variable_count();
    snprintf(response, response_size, 
             "Configured %d total variables (%d requested)", 
             total, added_count);
    return ESP_OK;
}

