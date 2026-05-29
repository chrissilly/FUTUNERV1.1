#include "dtc_commands.h"

#include "dtc/dtc.h"

#include "esp_log.h"
#include "cJSON.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * dtc_commands.c — WS / serial command handlers for DTC read and
 * clear. See dtc_commands.h. Thin cJSON shim over dtc_read() /
 * dtc_clear() in dtc/dtc_feature.c.
 *
 * Response shape (becomes the "data" field of the command_handler
 * envelope):
 *   dtc_read  → {"ok": true, "codes":[{"code":"P0420","status":9,
 *                "description":"..."}, ...]}
 *   dtc_clear → {"ok": true, "cleared_count": 3}
 * On failure, ok=false plus an "error" string carrying the human-
 * readable reason from dtc_feature.c (NRC, transport timeout, etc.).
 */

static const char *TAG = "DTC_CMDS";

static void emit_object(char       *response,
                        size_t      response_size,
                        cJSON      *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        strncpy(response, json, response_size - (size_t)1);
        response[response_size - (size_t)1] = '\0';
        free(json);
    }
    cJSON_Delete(root);
}

esp_err_t cmd_dtc_read(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    dtc_entry_t entries[DTC_MAX_CODES_PER_RESPONSE];
    size_t      count = (size_t)0;
    char        err[128] = {0};

    esp_err_t rc = dtc_read((uint8_t)DTC_DEFAULT_STATUS_MASK,
                            entries,
                            sizeof(entries) / sizeof(entries[0]),
                            &count, err, sizeof(err));

    cJSON *root = cJSON_CreateObject();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "dtc_read rc=%d (%s)", (int)rc, err);
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
                                err[0] != '\0' ? err : "DTC read failed");
        emit_object(response, response_size, root);
        return ESP_OK; /* command_handler infers success from the
                          envelope; we always return ESP_OK and let
                          the JSON body carry the failure mode. */
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *codes = cJSON_CreateArray();
    for (size_t i = (size_t)0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "code", entries[i].code);
        /* P-78: surface the FTB (Failure Type Byte) so the UI can
         * render "P0077.84" vs "P0077.89" — two records with the
         * same DTC code but different sub-fault types. */
        cJSON_AddNumberToObject(entry, "ftb",    (double)entries[i].ftb);
        cJSON_AddNumberToObject(entry, "status", (double)entries[i].status);
        cJSON_AddStringToObject(entry, "description",
                                entries[i].description != NULL
                                    ? entries[i].description
                                    : "");
        cJSON_AddItemToArray(codes, entry);
    }
    cJSON_AddItemToObject(root, "codes", codes);
    ESP_LOGI(TAG, "dtc_read returned %u codes", (unsigned)count);
    emit_object(response, response_size, root);
    return ESP_OK;
}

esp_err_t cmd_dtc_clear(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    uint16_t cleared = (uint16_t)0;
    char     err[128] = {0};

    esp_err_t rc = dtc_clear(&cleared, err, sizeof(err));

    cJSON *root = cJSON_CreateObject();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "dtc_clear rc=%d (%s)", (int)rc, err);
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
                                err[0] != '\0' ? err : "DTC clear failed");
        emit_object(response, response_size, root);
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "cleared_count", (double)cleared);
    ESP_LOGI(TAG, "dtc_clear cleared %u codes", (unsigned)cleared);
    emit_object(response, response_size, root);
    return ESP_OK;
}
