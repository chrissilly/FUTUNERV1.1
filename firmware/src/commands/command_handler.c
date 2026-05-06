#include "command_handler.h"
#include "commands.h"
#include "websocket/ws_server.h"
#include "nvs/nvs_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "CMD_HANDLER";

#define MAX_AUTHENTICATED_CLIENTS 8

/* H1 fix: use -1 as empty sentinel instead of 0 (fd=0 is valid) */
static int authenticated_clients[MAX_AUTHENTICATED_CLIENTS];

/* C1 fix: password loaded from NVS at init, not hardcoded */
static char s_password[64] = {0};

/* H6 fix: simple rate limiting for auth attempts */
#define AUTH_MAX_ATTEMPTS 5
#define AUTH_LOCKOUT_MS   30000

static uint8_t s_auth_fail_count = 0;
static uint32_t s_auth_lockout_until = 0;

static bool is_client_authenticated(int fd) {
    for (uint8_t i = 0; i < MAX_AUTHENTICATED_CLIENTS; i++) {
        if (authenticated_clients[i] == fd) {
            return true;
        }
    }
    return false;
}

static void set_client_authenticated(int fd, bool authenticated) {
    if (authenticated) {
        for (uint8_t i = 0; i < MAX_AUTHENTICATED_CLIENTS; i++) {
            /* H1 fix: check for -1 sentinel */
            if (authenticated_clients[i] == -1) {
                authenticated_clients[i] = fd;
                ESP_LOGI(TAG, "Client fd=%d authenticated", fd);
                return;
            }
        }
        ESP_LOGW(TAG, "Could not authenticate client, max limit reached");
    } else {
        for (uint8_t i = 0; i < MAX_AUTHENTICATED_CLIENTS; i++) {
            if (authenticated_clients[i] == fd) {
                /* H1 fix: set to -1 not 0 */
                authenticated_clients[i] = -1;
                ESP_LOGI(TAG, "Client fd=%d de-authenticated", fd);
                return;
            }
        }
    }
}

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

static void handle_unlock_command(int fd, const char *password) {
    /* H6 fix: rate limiting */
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_auth_lockout_until > 0 && now < s_auth_lockout_until) {
        send_response(fd, "unlock", false, "Too many attempts, try again later", NULL);
        return;
    }

    /* C1 fix: compare against NVS-loaded password */
    if (strcmp(password, s_password) == 0) {
        s_auth_fail_count = 0;
        set_client_authenticated(fd, true);
        send_response(fd, "unlock", true, "Authentication successful", NULL);
    } else {
        s_auth_fail_count++;
        if (s_auth_fail_count >= AUTH_MAX_ATTEMPTS) {
            s_auth_lockout_until = now + AUTH_LOCKOUT_MS;
            s_auth_fail_count = 0;
            ESP_LOGW(TAG, "Auth rate limit triggered, locked for %d ms", AUTH_LOCKOUT_MS);
        }
        send_response(fd, "unlock", false, "Invalid password", NULL);
    }
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
    /* H1 fix: init with -1 sentinel */
    memset(authenticated_clients, -1, sizeof(authenticated_clients));

    /* C1 fix: load password from NVS, fall back to default */
    if (nvs_manager_load_string(CMD_PASSWORD_NVS_KEY, s_password, sizeof(s_password)) != ESP_OK
        || s_password[0] == '\0') {
        strncpy(s_password, CMD_PASSWORD_DEFAULT, sizeof(s_password) - 1);
        s_password[sizeof(s_password) - 1] = '\0';
    }

    /* C1 fix: do NOT log the password */
    ESP_LOGI(TAG, "Command handler initialized (password source: %s)",
             (nvs_manager_load_string(CMD_PASSWORD_NVS_KEY, (char[64]){0}, 64) == ESP_OK) ? "NVS" : "default");
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

    if (strcmp(command, "unlock") == 0) {
        cJSON *password_item = cJSON_GetObjectItem(root, "password");
        if (cJSON_IsString(password_item)) {
            handle_unlock_command(fd, password_item->valuestring);
        } else {
            send_response(fd, "unlock", false, "Missing 'password' field", NULL);
        }
        cJSON_Delete(root);
        return;
    }

    const command_def_t *cmd = find_command(command);
    if (cmd == NULL) {
        send_response(fd, command, false, "Unknown command", NULL);
        cJSON_Delete(root);
        return;
    }

    if (cmd->security == CMD_SECURITY_SECURED && !is_client_authenticated(fd)) {
        send_response(fd, command, false, "Authentication required", NULL);
        cJSON_Delete(root);
        return;
    }

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

bool command_handler_is_client_authenticated(int fd) {
    return is_client_authenticated(fd);
}

void command_handler_clear_authentication(int fd) {
    set_client_authenticated(fd, false);
}
