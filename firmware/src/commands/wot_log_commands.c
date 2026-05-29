#include "wot_log_commands.h"
#include "feature_manager.h"
#include "logger/wot_uploader.h"

#include "esp_log.h"
#include "cJSON.h"

#include <string.h>

// wot_log_commands — see wot_log_commands.h.
//
// Response shape mirrors the rest of the command_handler responses
// (success: bool, command: string, plus an error string when success
// is false). On a successful start/stop the handler's response is
// minimal — the active feature name lets the UI confirm which
// feature is running after arbitration.

static const char *TAG = "WOT_LOG_CMDS";

static void emit_simple(char *response, size_t response_size,
                        const char *command, bool success,
                        const char *error_or_null,
                        const char *active_or_null) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "command", command);
    if (error_or_null != NULL) {
        cJSON_AddStringToObject(root, "error", error_or_null);
    }
    if (active_or_null != NULL) {
        cJSON_AddStringToObject(root, "active_feature", active_or_null);
    }
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        strncpy(response, json, response_size - (size_t)1);
        response[response_size - (size_t)1] = '\0';
        free(json);
    }
    cJSON_Delete(root);
}

esp_err_t cmd_wot_log_start(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    char err[128] = {0};
    esp_err_t rc = feature_manager_request_start(FEATURE_WOT_LOGGING, err, sizeof(err));
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "WOT logging started; active=%s",
                 feature_manager_active_name());
        emit_simple(response, response_size, "wot_log_start",
                    true, NULL, feature_manager_active_name());
        return ESP_OK;
    }

    ESP_LOGW(TAG, "WOT logging start refused rc=%d (%s)", (int)rc, err);
    emit_simple(response, response_size, "wot_log_start",
                false, err[0] != '\0' ? err : "feature_manager rejected start",
                feature_manager_active_name());
    return ESP_OK;
}

/* Status: queue depth + active feature + running flag. Cheap; UI
 * polls this on the WOT panel to keep the stats tiles live. */
esp_err_t cmd_wot_log_status(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    const char *active = feature_manager_active_name();
    bool running = (active && strcmp(active, "wot_logger") == 0);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "command", "wot_log_status");
    cJSON_AddStringToObject(root, "active_feature", active ? active : "none");
    cJSON_AddBoolToObject(root, "running", running);
    cJSON_AddNumberToObject(root, "queue_count", (double)wot_uploader_queue_count());

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        strncpy(response, json, response_size - 1);
        response[response_size - 1] = '\0';
        free(json);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t cmd_wot_log_stop(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    esp_err_t rc = feature_manager_request_stop(FEATURE_WOT_LOGGING);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "WOT logging stopped; active=%s",
                 feature_manager_active_name());
        emit_simple(response, response_size, "wot_log_stop",
                    true, NULL, feature_manager_active_name());
        return ESP_OK;
    }

    ESP_LOGW(TAG, "WOT logging stop returned rc=%d", (int)rc);
    emit_simple(response, response_size, "wot_log_stop",
                false, "feature_manager rejected stop",
                feature_manager_active_name());
    return ESP_OK;
}
