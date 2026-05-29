#include "command_handler.h"
#include "commands.h"
#include "websocket/ws_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "CMD_HANDLER";

/* P-75: command auth gate removed (was a compile-time-default
 * password extractable from any shipped binary). Phase 2/3
 * destructive ops need a real auth model — P-76. */

static void send_response(int fd, const char *command, bool success, const char *message, cJSON *data) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command", command);
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "message", message);

    if (data != NULL) {
        cJSON_AddItemToObject(root, "data", data);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ws_server_send_text(fd, json_str);
        free(json_str);
    }

    cJSON_Delete(root);
}

static const command_def_t* find_command(const char *name) {
    extern const command_def_t COMMAND_REGISTRY[];
    extern const uint8_t COMMAND_COUNT;

    for (uint8_t i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(COMMAND_REGISTRY[i].name, name) == 0) {
            return &COMMAND_REGISTRY[i];
        }
    }
    return NULL;
}

esp_err_t command_handler_init(void) {
    ESP_LOGI(TAG, "Command handler initialized (P-75: no auth gate)");
    return ESP_OK;
}

void command_handler_process_message(int fd, const char *message, size_t len) {
    cJSON *root = cJSON_ParseWithLength(message, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Invalid JSON from fd=%d", fd);
        send_response(fd, "unknown", false, "Invalid JSON", NULL);
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd_item)) {
        send_response(fd, "unknown", false, "Missing 'command' field", NULL);
        cJSON_Delete(root);
        return;
    }

    const char *command = cmd_item->valuestring;
    ESP_LOGI(TAG, "Processing command: %s from fd=%d", command, fd);

    const command_def_t *cmd = find_command(command);
    if (cmd == NULL) {
        send_response(fd, command, false, "Unknown command", NULL);
        cJSON_Delete(root);
        return;
    }

    /* P-75: no auth gate. Phase 2/3 destructive ops will gate
     * through a different mechanism (P-76 auth-model RFC). */

    cJSON *params_item = cJSON_GetObjectItem(root, "params");
    const char *params_str = NULL;
    if (cJSON_IsObject(params_item)) {
        params_str = cJSON_PrintUnformatted(params_item);
    }

    char *response = malloc(CMD_MAX_RESPONSE_SIZE);
    if (!response) {
        send_response(fd, command, false, "Out of memory", NULL);
        if (params_str) free((void *)params_str);
        cJSON_Delete(root);
        return;
    }
    response[0] = '\0';

    esp_err_t result = cmd->execute(fd, params_str, response, CMD_MAX_RESPONSE_SIZE);

    if (params_str) {
        free((void *)params_str);
    }

    if (result == ESP_OK) {
        cJSON *response_data = NULL;
        if (strlen(response) > 0) {
            response_data = cJSON_Parse(response);
        }
        send_response(fd, command, true, "Command executed successfully", response_data);
    } else {
        send_response(fd, command, false,
                     strlen(response) > 0 ? response : "Command execution failed",
                     NULL);
    }

    free(response);
    cJSON_Delete(root);
}

