#include "sbf_commands.h"

#include "sbf/sbf_orchestrator.h"
#include "sbf_config.h"

#include "esp_log.h"
#include "cJSON.h"

#include <string.h>

// sbf_commands — see sbf_commands.h. Thin cJSON shim around the
// orchestrator's public API.
//
// Response shapes:
//   live_tune_start  → {ok, message}              | {ok:false, error}
//   live_tune_set    → {ok, message}              | {ok:false, error}
//   live_tune_stop   → {ok}                       | {ok:false, error}
//   live_tune_status → {ok:true, state, current_stage, current_ethanol_pct,
//                       last_apply_ms, last_apply_elapsed_ms,
//                       sbf_filename, last_error}

static const char *TAG = "SBF_CMDS";

static void emit_object(char *response, size_t response_size, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        strncpy(response, json, response_size - (size_t)1);
        response[response_size - (size_t)1] = '\0';
        free(json);
    }
    cJSON_Delete(root);
}

static esp_err_t parse_stage_ethanol(const char *params,
                                     uint8_t    *stage_out,
                                     uint8_t    *eth_out) {
    if (params == NULL) return ESP_ERR_INVALID_ARG;
    cJSON *parsed = cJSON_Parse(params);
    if (parsed == NULL) return ESP_ERR_INVALID_ARG;

    cJSON *stage = cJSON_GetObjectItem(parsed, "stage");
    cJSON *eth   = cJSON_GetObjectItem(parsed, "ethanol_pct");
    if (!cJSON_IsNumber(stage) || !cJSON_IsNumber(eth)) {
        cJSON_Delete(parsed);
        return ESP_ERR_INVALID_ARG;
    }
    int s = stage->valueint;
    int e = eth->valueint;
    cJSON_Delete(parsed);

    if (s < (int)SBF_STAGE_MIN || s > (int)SBF_STAGE_MAX) return ESP_ERR_INVALID_ARG;
    if (e < (int)SBF_ETHANOL_MIN_PCT || e > (int)SBF_ETHANOL_MAX_PCT) return ESP_ERR_INVALID_ARG;
    *stage_out = (uint8_t)s;
    *eth_out   = (uint8_t)e;
    return ESP_OK;
}

esp_err_t cmd_live_tune_start(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    cJSON *root = cJSON_CreateObject();
    uint8_t stage = (uint8_t)0, eth = (uint8_t)0;
    if (parse_stage_ethanol(params, &stage, &eth) != ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
                                "params must be {stage:1..3, ethanol_pct:0..100}");
        emit_object(response, response_size, root);
        return ESP_OK;
    }

    char err[SBF_ERR_BUF_MAX] = {0};
    esp_err_t rc = sbf_orchestrator_live_tune_start(stage, eth, err, sizeof(err));
    if (rc == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "message", "live tune apply enqueued");
        ESP_LOGI(TAG, "live_tune_start stage=%u eth=%u OK", stage, eth);
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", err[0] != '\0' ? err : "live_tune_start failed");
        ESP_LOGW(TAG, "live_tune_start rc=%d (%s)", (int)rc, err);
    }
    emit_object(response, response_size, root);
    return ESP_OK;
}

esp_err_t cmd_live_tune_set(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    cJSON *root = cJSON_CreateObject();
    uint8_t stage = (uint8_t)0, eth = (uint8_t)0;
    if (parse_stage_ethanol(params, &stage, &eth) != ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
                                "params must be {stage:1..3, ethanol_pct:0..100}");
        emit_object(response, response_size, root);
        return ESP_OK;
    }

    char err[SBF_ERR_BUF_MAX] = {0};
    esp_err_t rc = sbf_orchestrator_live_tune_set(stage, eth, err, sizeof(err));
    if (rc == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "message", "re-apply enqueued");
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", err[0] != '\0' ? err : "live_tune_set failed");
        ESP_LOGW(TAG, "live_tune_set rc=%d (%s)", (int)rc, err);
    }
    emit_object(response, response_size, root);
    return ESP_OK;
}

esp_err_t cmd_live_tune_stop(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;
    char err[SBF_ERR_BUF_MAX] = {0};
    esp_err_t rc = sbf_orchestrator_live_tune_stop(err, sizeof(err));
    cJSON *root = cJSON_CreateObject();
    if (rc == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", err[0] != '\0' ? err : "live_tune_stop failed");
    }
    emit_object(response, response_size, root);
    return ESP_OK;
}

esp_err_t cmd_live_tune_status(int fd, const char *params, char *response, size_t response_size) {
    (void)fd;
    (void)params;
    sbf_status_snapshot_t st;
    sbf_orchestrator_live_tune_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "state", (double)st.state);
    cJSON_AddNumberToObject(root, "current_stage", (double)st.current_stage);
    cJSON_AddNumberToObject(root, "current_ethanol_pct", (double)st.current_ethanol_pct);
    cJSON_AddNumberToObject(root, "last_apply_ms", (double)st.last_apply_ms);
    cJSON_AddNumberToObject(root, "last_apply_elapsed_ms", (double)st.last_apply_elapsed_ms);
    cJSON_AddStringToObject(root, "sbf_filename", st.sbf_filename);
    cJSON_AddStringToObject(root, "last_error", st.last_error);
    emit_object(response, response_size, root);
    return ESP_OK;
}
