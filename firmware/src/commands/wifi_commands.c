#include "wifi_commands.h"

#include "config/wifi_config.h"
#include "wifi/wifi_ap.h"
#include "feature_manager/feature_manager.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WIFI_CMD";

/* -------------------------------------------------------------------- */
/* Helpers                                                              */
/* -------------------------------------------------------------------- */

static void emit_json(char *response, size_t response_size, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        strncpy(response, json, response_size - (size_t)1);
        response[response_size - (size_t)1] = '\0';
        free(json);
    }
    cJSON_Delete(root);
}

/* Predicate lives in wifi_ap.c as `wifi_feature_uses_cloud_network(int)`
 * so the host eval gate can exercise it without dragging cJSON into the
 * test build. See `firmware/test/wifi_manager/test_wifi_manager.c`. */

/* Tolerate both serial and WS params: serial gives the raw rest-of-line,
 * WS pre-prints a JSON object. Returns NULL on parse failure. Caller
 * must cJSON_Delete the result. */
static cJSON *parse_params_object(const char *params) {
    if (params == NULL || params[0] == '\0') return NULL;
    cJSON *obj = cJSON_Parse(params);
    if (obj != NULL && cJSON_IsObject(obj)) return obj;
    if (obj != NULL) cJSON_Delete(obj);
    return NULL;
}

/* -------------------------------------------------------------------- */
/* cmd_wifi_sta_set                                                     */
/* -------------------------------------------------------------------- */

esp_err_t cmd_wifi_sta_set(int fd, const char *params,
                           char *response, size_t response_size) {
    (void)fd;
    cJSON *root = cJSON_CreateObject();

    /* Parse: { "ssid":"...", "password":"..." } via WS or
     * "<ssid> <password>" via serial (the serial console pre-wraps). */
    char ssid[33] = {0};
    char pass[65] = {0};

    cJSON *parsed = parse_params_object(params);
    if (parsed != NULL) {
        cJSON *s = cJSON_GetObjectItem(parsed, "ssid");
        cJSON *p = cJSON_GetObjectItem(parsed, "password");
        if (cJSON_IsString(s)) strncpy(ssid, s->valuestring, sizeof(ssid) - 1);
        if (cJSON_IsString(p)) strncpy(pass, p->valuestring, sizeof(pass) - 1);
        cJSON_Delete(parsed);
    } else if (params != NULL && params[0] != '\0') {
        /* Serial-style "<ssid> <password>" — split on the first space. */
        const char *space = strchr(params, ' ');
        if (space != NULL) {
            size_t n = (size_t)(space - params);
            if (n >= sizeof(ssid)) n = sizeof(ssid) - 1;
            memcpy(ssid, params, n);
            ssid[n] = '\0';
            const char *p = space + 1;
            while (*p == ' ') p++;
            strncpy(pass, p, sizeof(pass) - 1);
        } else {
            strncpy(ssid, params, sizeof(ssid) - 1);
        }
    }

    if (ssid[0] == '\0') {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "missing_ssid");
        emit_json(response, response_size, root);
        return ESP_OK;
    }
    if (pass[0] != '\0' && strlen(pass) < WIFI_STA_PASSWORD_MIN_LEN) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "password_too_short");
        cJSON_AddNumberToObject(root, "min_len", (double)WIFI_STA_PASSWORD_MIN_LEN);
        emit_json(response, response_size, root);
        return ESP_OK;
    }

    esp_err_t r = wifi_client_set_creds(ssid, pass);
    if (r != ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(r));
        emit_json(response, response_size, root);
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "creds", "set");
    cJSON_AddStringToObject(root, "ssid", ssid);
    ESP_LOGI(TAG, "wifi_sta_set ok ssid=%s", ssid);
    emit_json(response, response_size, root);
    return ESP_OK;
}

/* -------------------------------------------------------------------- */
/* cmd_wifi_mode                                                        */
/* -------------------------------------------------------------------- */

static bool params_match(const char *params, const char *needle) {
    if (params == NULL || params[0] == '\0') return false;
    /* Accept "<arg>", "{\"mode\":\"<arg>\"}", or anything containing needle. */
    return strstr(params, needle) != NULL;
}

esp_err_t cmd_wifi_mode(int fd, const char *params,
                        char *response, size_t response_size) {
    (void)fd;
    cJSON *root = cJSON_CreateObject();

    bool want_sta = params_match(params, "sta");
    bool want_ap  = params_match(params, "ap");
    if (want_sta == want_ap) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "missing_or_ambiguous_mode");
        cJSON_AddStringToObject(root, "expected", "ap|sta");
        emit_json(response, response_size, root);
        return ESP_OK;
    }

    if (want_sta) {
        if (!wifi_client_creds_stored()) {
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error", "no_sta_creds");
            emit_json(response, response_size, root);
            return ESP_OK;
        }
        feature_id_t active = feature_manager_active();
        if (wifi_feature_uses_cloud_network((int)active)) {
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error", "feature_active");
            cJSON_AddStringToObject(root, "feature",
                                    feature_manager_active_name());
            emit_json(response, response_size, root);
            return ESP_OK;
        }

        esp_err_t r = wifi_set_mode_intent(WIFI_MODE_INTENT_APSTA);
        if (r != ESP_OK) {
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error",
                r == ESP_ERR_NOT_FOUND ? "no_sta_creds" : esp_err_to_name(r));
            emit_json(response, response_size, root);
            return ESP_OK;
        }

        /* Wait briefly for IP_EVENT_STA_GOT_IP. The underlying retry
         * loop keeps trying after we time out — this is just the
         * command-response deadline. */
        const TickType_t poll = pdMS_TO_TICKS(WIFI_STA_CONNECT_POLL_MS);
        const TickType_t budget = pdMS_TO_TICKS(WIFI_STA_CONNECT_TIMEOUT_MS);
        TickType_t waited = 0;
        while (!wifi_client_is_connected() && waited < budget) {
            vTaskDelay(poll);
            waited += poll;
        }

        char ssid[33] = {0};
        /* Best-effort echo of stored SSID; ignore read errors. */
        extern esp_err_t nvs_manager_load_string(const char *, char *, size_t);
        nvs_manager_load_string(WIFI_STA_SSID_NVS_KEY, ssid, sizeof(ssid));

        if (!wifi_client_is_connected()) {
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error", "sta_connect_timeout");
            cJSON_AddStringToObject(root, "ssid", ssid);
            emit_json(response, response_size, root);
            return ESP_OK;
        }

        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "mode", "sta");
        cJSON_AddStringToObject(root, "ssid", ssid);
        cJSON_AddStringToObject(root, "ip", wifi_client_get_ip());
        ESP_LOGI(TAG, "wifi_mode sta ok ssid=%s ip=%s", ssid, wifi_client_get_ip());
        emit_json(response, response_size, root);
        return ESP_OK;
    }

    /* AP intent. */
    esp_err_t r = wifi_set_mode_intent(WIFI_MODE_INTENT_AP_ONLY);
    if (r != ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(r));
        emit_json(response, response_size, root);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "mode", "ap");
    cJSON_AddStringToObject(root, "ssid", wifi_ap_get_ssid());
    cJSON_AddStringToObject(root, "ip", WIFI_AP_IP);
    ESP_LOGI(TAG, "wifi_mode ap ok");
    emit_json(response, response_size, root);
    return ESP_OK;
}

/* -------------------------------------------------------------------- */
/* cmd_wifi_clear                                                       */
/* -------------------------------------------------------------------- */

esp_err_t cmd_wifi_clear(int fd, const char *params,
                         char *response, size_t response_size) {
    (void)fd;
    (void)params;
    cJSON *root = cJSON_CreateObject();

    /* Force AP intent first so any in-progress reconnect attempt stops,
     * then wipe NVS creds. Order matters: if a disconnect lands after
     * the wipe, the auto-reconnect-on-boot path can't repopulate them. */
    esp_err_t mode_rc = wifi_set_mode_intent(WIFI_MODE_INTENT_AP_ONLY);
    esp_err_t clear_rc = wifi_client_clear_creds();

    if (clear_rc != ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(clear_rc));
        emit_json(response, response_size, root);
        return ESP_OK;
    }
    (void)mode_rc;  /* AP-intent failure is surfaced via subsequent wifi_status */

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "cleared", true);
    cJSON_AddStringToObject(root, "mode", "ap");
    ESP_LOGI(TAG, "wifi_clear ok");
    emit_json(response, response_size, root);
    return ESP_OK;
}

/* -------------------------------------------------------------------- */
/* cmd_wifi_status2 — the new shape                                     */
/* -------------------------------------------------------------------- */

esp_err_t cmd_wifi_status2(int fd, const char *params,
                           char *response, size_t response_size) {
    (void)fd;
    (void)params;
    cJSON *root = cJSON_CreateObject();

    wifi_mode_intent_t intent = wifi_get_mode_intent();
    const bool sta_up = wifi_client_is_connected();
    const bool creds_stored = wifi_client_creds_stored();

    cJSON_AddStringToObject(root, "mode",
        intent == WIFI_MODE_INTENT_APSTA ? "sta" : "ap");

    /* For intent=sta: show STA's SSID + IP (live association).
     * For intent=ap : show AP's SSID + IP. Single ssid/ip field per spec. */
    if (intent == WIFI_MODE_INTENT_APSTA) {
        char stored_ssid[33] = {0};
        extern esp_err_t nvs_manager_load_string(const char *, char *, size_t);
        nvs_manager_load_string(WIFI_STA_SSID_NVS_KEY, stored_ssid, sizeof(stored_ssid));
        cJSON_AddStringToObject(root, "ssid", stored_ssid);
        cJSON_AddStringToObject(root, "ip", wifi_client_get_ip());
    } else {
        cJSON_AddStringToObject(root, "ssid", wifi_ap_get_ssid());
        cJSON_AddStringToObject(root, "ip", WIFI_AP_IP);
    }

    cJSON_AddBoolToObject(root, "sta_connected", sta_up);
    cJSON_AddBoolToObject(root, "sta_creds_stored", creds_stored);

    emit_json(response, response_size, root);
    return ESP_OK;
}
