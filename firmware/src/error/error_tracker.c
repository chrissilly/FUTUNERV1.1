#include "error_tracker.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "websocket/ws_server.h"
#include "cJSON.h"
#include <string.h>
#include <stdarg.h>

static const char *TAG = "ERROR_TRACKER";

static error_entry_t error_log[ERROR_LOG_SIZE];
static uint16_t error_write_index = 0;
static uint16_t error_count = 0;
static bool initialized = false;

/* M1 fix: mutex for thread-safe access */
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t error_tracker_init(void) {
    if (initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(error_log, 0, sizeof(error_log));
    error_write_index = 0;
    error_count = 0;
    initialized = true;

    ESP_LOGI(TAG, "Error tracker initialized (buffer size: %d)", ERROR_LOG_SIZE);
    return ESP_OK;
}

static uint32_t get_timestamp_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void error_tracker_log(error_category_t category,
                       error_severity_t severity,
                       const char *format, ...) {
    if (!initialized || !s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    error_entry_t *entry = &error_log[error_write_index];

    entry->timestamp_ms = get_timestamp_ms();
    entry->category = category;
    entry->severity = severity;
    entry->active = true;

    va_list args;
    va_start(args, format);
    vsnprintf(entry->message, ERROR_MESSAGE_MAX_LEN, format, args);
    va_end(args);

    entry->message[ERROR_MESSAGE_MAX_LEN - 1] = '\0';

    error_write_index = (error_write_index + 1) % ERROR_LOG_SIZE;
    if (error_count < ERROR_LOG_SIZE) {
        error_count++;
    }

    xSemaphoreGive(s_mutex);

    const char *sev_str = error_tracker_severity_to_string(severity);
    const char *cat_str = error_tracker_category_to_string(category);
    
    switch (severity) {
        case ERROR_SEVERITY_INFO:
            ESP_LOGI(TAG, "[%s/%s] %s", cat_str, sev_str, entry->message);
            break;
        case ERROR_SEVERITY_WARNING:
            ESP_LOGW(TAG, "[%s/%s] %s", cat_str, sev_str, entry->message);
            break;
        case ERROR_SEVERITY_ERROR:
        case ERROR_SEVERITY_CRITICAL:
            ESP_LOGE(TAG, "[%s/%s] %s", cat_str, sev_str, entry->message);
            break;
    }

    if (ws_server_is_running() && severity >= ERROR_SEVERITY_ERROR) {
        error_tracker_broadcast_error(category, severity, entry->message);
    }
}

uint16_t error_tracker_get_count(void) {
    return error_count;
}

uint16_t error_tracker_get_error_count(void) {
    uint16_t count = 0;
    for (uint16_t i = 0; i < error_count; i++) {
        if (error_log[i].active && error_log[i].severity >= ERROR_SEVERITY_ERROR) {
            count++;
        }
    }
    return count;
}

uint16_t error_tracker_get_warning_count(void) {
    uint16_t count = 0;
    for (uint16_t i = 0; i < error_count; i++) {
        if (error_log[i].active && error_log[i].severity == ERROR_SEVERITY_WARNING) {
            count++;
        }
    }
    return count;
}

const error_entry_t* error_tracker_get_entry(uint16_t index) {
    if (index >= error_count) {
        return NULL;
    }

    uint16_t actual_index;
    if (error_count < ERROR_LOG_SIZE) {
        actual_index = index;
    } else {
        actual_index = (error_write_index + index) % ERROR_LOG_SIZE;
    }

    return &error_log[actual_index];
}

void error_tracker_clear(void) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(error_log, 0, sizeof(error_log));
    error_write_index = 0;
    error_count = 0;
    if (s_mutex) xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Error log cleared");
}

const char* error_tracker_category_to_string(error_category_t category) {
    switch (category) {
        case ERROR_CATEGORY_SYSTEM: return "SYSTEM";
        case ERROR_CATEGORY_WIFI: return "WIFI";
        case ERROR_CATEGORY_CAN: return "CAN";
        case ERROR_CATEGORY_CONNECTION: return "CONNECTION";
        case ERROR_CATEGORY_LOGGER: return "LOGGER";
        case ERROR_CATEGORY_NVS: return "NVS";
        case ERROR_CATEGORY_WEBSOCKET: return "WEBSOCKET";
        case ERROR_CATEGORY_COMMAND: return "COMMAND";
        default: return "UNKNOWN";
    }
}

const char* error_tracker_severity_to_string(error_severity_t severity) {
    switch (severity) {
        case ERROR_SEVERITY_INFO: return "INFO";
        case ERROR_SEVERITY_WARNING: return "WARNING";
        case ERROR_SEVERITY_ERROR: return "ERROR";
        case ERROR_SEVERITY_CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

void error_tracker_broadcast_error(error_category_t category, 
                                   error_severity_t severity, 
                                   const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddNumberToObject(root, "timestamp", get_timestamp_ms());
    cJSON_AddStringToObject(root, "category", error_tracker_category_to_string(category));
    cJSON_AddStringToObject(root, "severity", error_tracker_severity_to_string(severity));
    cJSON_AddStringToObject(root, "message", message);
    
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ws_server_broadcast_text(json_str);
        free(json_str);
    }
    
    cJSON_Delete(root);
}

