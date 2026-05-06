#include "vin_pair_commands.h"

#include "vin_pairing/vin_pairing.h"
#include "vin_pairing_config.h"
#include "license/license.h"
#include "license_config.h"
#include "nvs/nvs_manager.h"

#include "esp_log.h"
#include "cJSON.h"

#include <string.h>

// vin_pair_commands — see vin_pair_commands.h. Thin cJSON shim over
// vin_pairing_run_now / nvs_manager_save_string / license_get_state.
//
// Response shapes:
//   vin_pair_now    → {ok: true, message: "..."} | {ok: false, error: "..."}
//   set_auth_token  → {ok: true} | {ok: false, error: "..."}
//   license_status  → {ok: true, present, paid, revoked, vin,
//                      revoked_reason, last_sync_ms} | {ok: false, error: "..."}

static const char *TAG = "VIN_PAIR_CMDS";

static void emit_object(char *response, size_t response_size, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        strncpy(response, json, response_size - (size_t)1);
        response[response_size - (size_t)1] = '\0';
        free(json);
    }
    cJSON_Delete(root);
}

esp_err_t cmd_vin_pair_now(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    char err[VIN_PAIRING_ERR_BUF_MAX] = {0};
    esp_err_t rc = vin_pairing_run_now(err, sizeof(err));

    cJSON *root = cJSON_CreateObject();
    if (rc == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "message", "VIN paired and license refreshed");
        ESP_LOGI(TAG, "vin_pair_now OK");
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", err[0] != '\0' ? err : "vin_pair_now failed");
        ESP_LOGW(TAG, "vin_pair_now rc=%d (%s)", (int)rc, err);
    }
    emit_object(response, response_size, root);
    return ESP_OK;
}

esp_err_t cmd_set_auth_token(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;

    cJSON *root = cJSON_CreateObject();
    cJSON *parsed = params != NULL ? cJSON_Parse(params) : NULL;
    cJSON *tok    = parsed != NULL ? cJSON_GetObjectItem(parsed, "token") : NULL;

    if (parsed == NULL || !cJSON_IsString(tok) || tok->valuestring == NULL || tok->valuestring[0] == '\0') {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
                                "missing 'token' string in params (32-hex from /admin/devices)");
        if (parsed != NULL) cJSON_Delete(parsed);
        emit_object(response, response_size, root);
        return ESP_OK;
    }

    /* The cloud emits 32-hex tokens (secrets.token_hex(16)). Soft
     * size check: anything within LICENSE_AUTH_TOKEN_MAX is accepted
     * so future format changes don't require a firmware roll. */
    if (strlen(tok->valuestring) >= (size_t)LICENSE_AUTH_TOKEN_MAX) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
                                "token too long (LICENSE_AUTH_TOKEN_MAX exceeded)");
        cJSON_Delete(parsed);
        emit_object(response, response_size, root);
        return ESP_OK;
    }

    esp_err_t rc = nvs_manager_save_string(LICENSE_NVS_AUTH_TOKEN_KEY, tok->valuestring);
    cJSON_Delete(parsed);
    if (rc == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        ESP_LOGI(TAG, "auth_token written to NVS");
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "nvs_manager_save_string failed");
        ESP_LOGE(TAG, "nvs_manager_save_string rc=%d", (int)rc);
    }
    emit_object(response, response_size, root);
    return ESP_OK;
}

esp_err_t cmd_license_status(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;

    const license_state_t *st = license_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "present", st->present);
    cJSON_AddBoolToObject(root, "paid", st->paid);
    cJSON_AddBoolToObject(root, "revoked", st->revoked);
    cJSON_AddStringToObject(root, "vin", st->vin);
    cJSON_AddStringToObject(root, "revoked_reason", st->revoked_reason);
    cJSON_AddNumberToObject(root, "last_sync_ms", (double)st->last_sync_ms);
    emit_object(response, response_size, root);
    return ESP_OK;
}
