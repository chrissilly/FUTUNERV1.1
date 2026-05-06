#include "can_sniffer.h"
#include "can/can_driver.h"
#include "websocket/ws_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "CAN_SNIFF";

static bool sniffer_active = false;
static uint32_t filter_id = 0;
static bool filter_enabled = false;
static uint32_t frame_count = 0;
static uint32_t start_time_ms = 0;

bool can_sniffer_is_active(void) {
    return sniffer_active;
}

void can_sniffer_on_frame(uint32_t id, const uint8_t *data, uint8_t len) {
    if (!sniffer_active) return;

    /* Apply ID filter if set */
    if (filter_enabled && id != filter_id) return;

    frame_count++;

    /* Build JSON event */
    char json[256];
    char hex_data[24];
    int hpos = 0;
    for (uint8_t i = 0; i < len && hpos < (int)sizeof(hex_data) - 3; i++) {
        hpos += snprintf(hex_data + hpos, sizeof(hex_data) - hpos, "%02X", data[i]);
    }
    hex_data[hpos] = '\0';

    char data_arr[64];
    int dpos = 0;
    dpos += snprintf(data_arr + dpos, sizeof(data_arr) - dpos, "[");
    for (uint8_t i = 0; i < len; i++) {
        if (i > 0) dpos += snprintf(data_arr + dpos, sizeof(data_arr) - dpos, ",");
        dpos += snprintf(data_arr + dpos, sizeof(data_arr) - dpos, "%d", data[i]);
    }
    dpos += snprintf(data_arr + dpos, sizeof(data_arr) - dpos, "]");

    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    snprintf(json, sizeof(json),
             "{\"event\":\"can_frame\",\"id\":%lu,\"hex_id\":\"0x%03lX\",\"data\":%s,\"hex\":\"%s\",\"len\":%d,\"ts\":%lu}",
             (unsigned long)id, (unsigned long)id, data_arr, hex_data, len, (unsigned long)(now - start_time_ms));

    ws_server_broadcast_text(json);
}

esp_err_t cmd_can_sniff_start(int fd, const char *params, char *response, size_t response_size) {
    filter_enabled = false;
    filter_id = 0;

    /* Parse optional filter */
    if (params) {
        cJSON *root = cJSON_Parse(params);
        if (root) {
            cJSON *f = cJSON_GetObjectItem(root, "filter");
            if (cJSON_IsNumber(f)) {
                filter_id = (uint32_t)f->valuedouble;
                filter_enabled = true;
            } else if (cJSON_IsString(f)) {
                filter_id = (uint32_t)strtoul(f->valuestring, NULL, 16);
                filter_enabled = true;
            }
            cJSON_Delete(root);
        }
    }

    sniffer_active = true;
    frame_count = 0;
    start_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (filter_enabled) {
        ESP_LOGI(TAG, "CAN sniffer started (filter: 0x%03lX)", (unsigned long)filter_id);
        snprintf(response, response_size, "{\"sniffer\":\"started\",\"filter\":\"0x%03lX\"}", (unsigned long)filter_id);
    } else {
        ESP_LOGI(TAG, "CAN sniffer started (all IDs)");
        snprintf(response, response_size, "{\"sniffer\":\"started\",\"filter\":\"none\"}");
    }

    return ESP_OK;
}

esp_err_t cmd_can_sniff_stop(int fd, const char *params, char *response, size_t response_size) {
    sniffer_active = false;
    ESP_LOGI(TAG, "CAN sniffer stopped (%lu frames captured)", (unsigned long)frame_count);
    snprintf(response, response_size, "{\"sniffer\":\"stopped\",\"frames\":%lu}", (unsigned long)frame_count);
    return ESP_OK;
}

esp_err_t cmd_can_sniff_status(int fd, const char *params, char *response, size_t response_size) {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t elapsed = sniffer_active ? (now - start_time_ms) : 0;

    snprintf(response, response_size,
             "{\"active\":%s,\"frames\":%lu,\"elapsed_ms\":%lu,\"filter\":\"%s\"}",
             sniffer_active ? "true" : "false",
             (unsigned long)frame_count,
             (unsigned long)elapsed,
             filter_enabled ? "set" : "none");
    return ESP_OK;
}

esp_err_t cmd_can_send_raw(int fd, const char *params, char *response, size_t response_size) {
    if (!params) {
        snprintf(response, response_size, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(params);
    if (!root) {
        snprintf(response, response_size, "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    cJSON *data_item = cJSON_GetObjectItem(root, "data");
    cJSON *len_item = cJSON_GetObjectItem(root, "len");

    if (!cJSON_IsNumber(id_item) || !cJSON_IsArray(data_item)) {
        cJSON_Delete(root);
        snprintf(response, response_size, "Missing id or data array");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t can_id = (uint32_t)id_item->valuedouble;
    uint8_t data[8] = {0};
    uint8_t len = (uint8_t)cJSON_GetArraySize(data_item);
    if (len > 8) len = 8;

    for (uint8_t i = 0; i < len; i++) {
        cJSON *byte_item = cJSON_GetArrayItem(data_item, i);
        if (cJSON_IsNumber(byte_item)) {
            data[i] = (uint8_t)byte_item->valueint;
        }
    }

    if (cJSON_IsNumber(len_item)) {
        len = (uint8_t)len_item->valueint;
        if (len > 8) len = 8;
    }

    cJSON_Delete(root);

    esp_err_t err = can_driver_send(can_id, data, len);
    if (err == ESP_OK) {
        char hex[24];
        int hpos = 0;
        for (uint8_t i = 0; i < len; i++) {
            hpos += snprintf(hex + hpos, sizeof(hex) - hpos, "%02X", data[i]);
        }
        snprintf(response, response_size,
                 "{\"sent\":true,\"id\":\"0x%03lX\",\"hex\":\"%s\",\"len\":%d}",
                 (unsigned long)can_id, hex, len);
    } else {
        snprintf(response, response_size,
                 "{\"sent\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    }

    return err;
}
