#include "system_commands.h"
#include "commands.h"
#include "state_machine/connection_manager.h"
#include "error/error_tracker.h"
#include "wifi/wifi_ap.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "SYSTEM_CMD";

/* P-59: delay between cmd_reboot ACK send and the actual esp_restart()
 * so the WS server has time to flush the response to the client.
 * 500 ms covers AP-side and STA-side round-trips with margin and still
 * feels responsive. Per CLAUDE.md Rule 3 (no magic numbers). */
#define SYSTEM_CMD_REBOOT_ACK_DELAY_MS  500

esp_err_t cmd_get_status(int fd, const char *params, char *response, size_t response_size) {
    cJSON *root = cJSON_CreateObject();
    
    cJSON_AddBoolToObject(root, "connected", connection_manager_is_connected());
    cJSON_AddBoolToObject(root, "patched", connection_manager_is_patched());
    cJSON_AddBoolToObject(root, "paired", connection_manager_is_paired());
    cJSON_AddStringToObject(root, "state", 
                           connection_manager_get_state_name(connection_manager_get_state()));
    
    if (connection_manager_is_connected()) {
        cJSON_AddStringToObject(root, "boxcode", connection_manager_get_boxcode());
        
        if (connection_manager_is_patched()) {
            cJSON_AddNumberToObject(root, "patch_version", 
                                   connection_manager_get_patch_version());
            cJSON_AddNumberToObject(root, "log_buffer_address", 
                                   connection_manager_get_log_buffer_address());
            cJSON_AddNumberToObject(root, "logger_variable_count", 
                                   connection_manager_logger_get_variable_count());
        }
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t cmd_list_commands(int fd, const char *params, char *response, size_t response_size) {
    cJSON *root = cJSON_CreateObject();
    cJSON *commands_array = cJSON_CreateArray();
    
    for (uint8_t i = 0; i < COMMAND_COUNT; i++) {
        cJSON *cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "name", COMMAND_REGISTRY[i].name);
        cJSON_AddStringToObject(cmd, "description", COMMAND_REGISTRY[i].description);
        /* P-75: every command is unsecured at the dongle layer.
         * Field kept for client back-compat; always "unsecured". */
        cJSON_AddStringToObject(cmd, "security", "unsecured");
        cJSON_AddItemToArray(commands_array, cmd);
    }
    
    cJSON_AddItemToObject(root, "commands", commands_array);
    cJSON_AddNumberToObject(root, "count", COMMAND_COUNT);
    
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t cmd_get_errors(int fd, const char *params, char *response, size_t response_size) {
    cJSON *root = cJSON_CreateObject();
    
    uint16_t total_count = error_tracker_get_count();
    uint16_t error_count = error_tracker_get_error_count();
    uint16_t warning_count = error_tracker_get_warning_count();
    
    cJSON_AddNumberToObject(root, "total", total_count);
    cJSON_AddNumberToObject(root, "errors", error_count);
    cJSON_AddNumberToObject(root, "warnings", warning_count);
    
    cJSON *errors_array = cJSON_CreateArray();
    
    for (uint16_t i = 0; i < total_count; i++) {
        const error_entry_t *entry = error_tracker_get_entry(i);
        if (entry && entry->active) {
            cJSON *error_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(error_obj, "timestamp", entry->timestamp_ms);
            cJSON_AddStringToObject(error_obj, "category", 
                                   error_tracker_category_to_string(entry->category));
            cJSON_AddStringToObject(error_obj, "severity", 
                                   error_tracker_severity_to_string(entry->severity));
            cJSON_AddStringToObject(error_obj, "message", entry->message);
            cJSON_AddItemToArray(errors_array, error_obj);
        }
    }
    
    cJSON_AddItemToObject(root, "entries", errors_array);
    
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t cmd_clear_errors(int fd, const char *params, char *response, size_t response_size) {
    error_tracker_clear();
    snprintf(response, response_size, "Error log cleared");
    return ESP_OK;
}


esp_err_t cmd_wifi_connect(int fd, const char *params, char *response, size_t response_size) {
    if (!params) {
        snprintf(response, response_size, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(params);
    if (!root) { snprintf(response, response_size, "Invalid JSON"); return ESP_ERR_INVALID_ARG; }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid_item)) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Missing 'ssid'");
        return ESP_ERR_INVALID_ARG;
    }
    char ssid[33] = {0}, pass[65] = {0};
    strncpy(ssid, ssid_item->valuestring, 32);
    if (cJSON_IsString(pass_item)) strncpy(pass, pass_item->valuestring, 64);
    cJSON_Delete(root);

    esp_err_t r = wifi_client_connect(ssid, pass);
    if (r == ESP_OK) {
        snprintf(response, response_size, "{\"connecting\":\"%s\"}", ssid);
    } else {
        snprintf(response, response_size, "{\"error\":\"%s\"}", esp_err_to_name(r));
    }
    return r;
}

esp_err_t cmd_wifi_disconnect(int fd, const char *params, char *response, size_t response_size) {
    esp_err_t r = wifi_client_disconnect();
    snprintf(response, response_size, "{\"result\":\"%s\"}", esp_err_to_name(r));
    return r;
}

esp_err_t cmd_wifi_status(int fd, const char *params, char *response, size_t response_size) {
    bool up = wifi_client_is_connected();
    const char *ip = wifi_client_get_ip();
    snprintf(response, response_size, "{\"connected\":%s,\"ip\":\"%s\"}",
             up ? "true" : "false", ip ? ip : "");
    return ESP_OK;
}

esp_err_t cmd_logger_start(int fd, const char *params, char *response, size_t response_size) {
    /* P-80: per-fd polling refcount. Dashboard tab's Start sends
     * logger_start → this fd holds a ref. Dashboard Stop sends
     * logger_stop → ref drops. Polling continues only if some
     * other consumer (WOT, Live Tune, another WS client) still
     * holds a ref. */
    connection_manager_logger_ws_acquire(fd);
    snprintf(response, response_size,
             "{\"logger\":\"started\",\"refcount\":%u}",
             (unsigned)connection_manager_logger_refcount());
    return ESP_OK;
}

esp_err_t cmd_logger_stop(int fd, const char *params, char *response, size_t response_size) {
    connection_manager_logger_ws_release(fd);
    snprintf(response, response_size,
             "{\"logger\":\"stopped\",\"refcount\":%u}",
             (unsigned)connection_manager_logger_refcount());
    return ESP_OK;
}

/* P-59: one-shot deferred-restart task. Delays
 * SYSTEM_CMD_REBOOT_ACK_DELAY_MS so the WS server can flush the
 * cmd_reboot ACK to the client, then calls esp_restart(). */
static void reboot_task(void *arg) {
    (void)arg;
    ESP_LOGW(TAG, "reboot requested; restarting in %d ms", SYSTEM_CMD_REBOOT_ACK_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(SYSTEM_CMD_REBOOT_ACK_DELAY_MS));
    esp_restart();
}

esp_err_t cmd_reboot(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;
    /* Send the ACK first; the deferred task triggers esp_restart() after
     * the response has had a chance to flush. SECURED in COMMAND_REGISTRY
     * so unauthenticated clients can't reboot the dongle. */
    snprintf(response, response_size, "{\"ok\":true,\"message\":\"rebooting\"}");
    BaseType_t rc = xTaskCreate(reboot_task, "reboot", 2048, NULL,
                                tskIDLE_PRIORITY + 1, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn reboot_task (rc=%d) — falling back to immediate restart",
                 (int)rc);
        /* Last resort: response won't flush, but caller WILL see a reset. */
        esp_restart();
    }
    return ESP_OK;
}
