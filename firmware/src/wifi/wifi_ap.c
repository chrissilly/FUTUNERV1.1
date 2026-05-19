#include "wifi_ap.h"
#include "config/wifi_config.h"
#include "nvs/nvs_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "websocket/ws_server.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WIFI_AP";

static bool wifi_initialized = false;
static bool wifi_running = false;

#ifdef WIFI_AP_HOST_TEST
/* Host-test-only hatch. wifi_ap_init/_start never run host-side, so
 * `wifi_running` stays false and wifi_client_connect short-circuits the
 * test setup before reaching the esp_wifi spy layer. Production builds
 * never define WIFI_AP_HOST_TEST. */
void wifi_ap_test_force_running(bool v) { wifi_running = v; }
#endif
static uint8_t connected_clients = 0;
static esp_netif_t *ap_netif = NULL;
static esp_netif_t *sta_netif = NULL;
static uint64_t device_serial = 0;
static char device_ssid[32] = {0};
static bool sta_connected = false;
static char sta_ip_str[16] = {0};
static uint8_t sta_retry_count = 0;
/* STA_MAX_RETRIES relocated to config/wifi_config.h */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(sta_ip_str, sizeof(sta_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        sta_connected = true;
        sta_retry_count = 0;
        ESP_LOGI(TAG, "STA connected — IP: %s", sta_ip_str);
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        sta_connected = false;
        sta_ip_str[0] = '\0';
        sta_retry_count++;
        if (sta_retry_count <= STA_MAX_RETRIES) {
            ESP_LOGW(TAG, "STA disconnected reason=%d (retry %d/%d)", d->reason, sta_retry_count, STA_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA gave up after %d retries (last reason=%d)", sta_retry_count, d->reason);
        }
        return;
    }
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_STACONNECTED:
                connected_clients++;
                ESP_LOGI(TAG, "Client connected (total: %d)", connected_clients);
                /* WS server is started unconditionally by wifi_ap_start()
                 * since the 2026-05-17 smoke-test prompt — first-AP-client
                 * gating was a designed optimization that blocked
                 * over-LAN headless reach (STA-side or first-time setup
                 * via cable). Server start is idempotent (ws_server_start
                 * returns ESP_OK if `server != NULL`). */
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                if (connected_clients > 0) {
                    connected_clients--;
                }
                ESP_LOGI(TAG, "Client disconnected (total: %d)", connected_clients);
                /* Last client left: shut down the web server to save resources */
                if (connected_clients == 0 && ws_server_is_running()) {
                    ESP_LOGI(TAG, "No clients remaining - stopping web server");
                    ws_server_stop();
                }
                break;
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "Access Point started");
                wifi_running = true;
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "Access Point stopped");
                wifi_running = false;
                connected_clients = 0;
                /* WS server stays up across AP-stop — the operator may be
                 * driving over STA-side LAN. Stopping it would tear down
                 * the only remaining management surface. The server gets
                 * torn down only on wifi_ap_stop() (full shutdown). */
                break;
            default:
                break;
        }
    }
}

static void calculate_serial_and_ssid(void) {
    uint8_t mac[6];
    esp_err_t ret = esp_efuse_mac_get_default(mac);
    
    if (ret == ESP_OK) {
        device_serial = ((uint64_t)mac[0] << 40) |
                       ((uint64_t)mac[1] << 32) |
                       ((uint64_t)mac[2] << 24) |
                       ((uint64_t)mac[3] << 16) |
                       ((uint64_t)mac[4] << 8) |
                       ((uint64_t)mac[5]);
        
        uint32_t folded = (device_serial & 0xFFFFFF) ^ ((device_serial >> 24) & 0xFFFFFF);
        
        snprintf(device_ssid, sizeof(device_ssid), "FUTUNER_%06lx", (unsigned long)folded);
        
        ESP_LOGI(TAG, "Device Serial: 0x%012llX", device_serial);
        ESP_LOGI(TAG, "Device SSID: %s", device_ssid);
    } else {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(ret));
        snprintf(device_ssid, sizeof(device_ssid), "FUTUNER_ERROR");
    }
}

esp_err_t wifi_ap_init(void) {
    if (wifi_initialized) {
        ESP_LOGW(TAG, "WiFi already initialized");
        return ESP_OK;
    }
    
    calculate_serial_and_ssid();

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TCP/IP stack: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }
    sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGW(TAG, "Failed to create STA netif (AP-only mode)");
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_str_to_ip4(WIFI_AP_IP, &ip_info.ip);
    esp_netif_str_to_ip4(WIFI_AP_GATEWAY, &ip_info.gw);
    esp_netif_str_to_ip4(WIFI_AP_NETMASK, &ip_info.netmask);

    ret = esp_netif_dhcps_stop(ap_netif);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop DHCP server: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_set_ip_info(ap_netif, &ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set IP info: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_dhcps_start(ap_netif);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start DHCP server: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Also handle IP_EVENT for STA got-IP */
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi AP initialized");
    return ESP_OK;
}

esp_err_t wifi_ap_start(void) {
    if (!wifi_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (wifi_running) {
        ESP_LOGW(TAG, "WiFi AP already running");
        return ESP_OK;
    }

    wifi_config_t wifi_config = {0};
    
    strncpy((char *)wifi_config.ap.ssid, device_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(device_ssid);
    wifi_config.ap.channel = WIFI_AP_CHANNEL;
    /* C1 fix: load WiFi password from NVS, fall back to default */
    char wifi_password[64] = {0};
    if (nvs_manager_load_string(WIFI_AP_PASSWORD_NVS_KEY, wifi_password, sizeof(wifi_password)) != ESP_OK
        || wifi_password[0] == '\0') {
        strncpy(wifi_password, WIFI_AP_PASSWORD_DEFAULT, sizeof(wifi_password) - 1);
    }

    strncpy((char *)wifi_config.ap.password, wifi_password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONNECTIONS;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;

    if (strlen(wifi_password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    /* APSTA: AP stays up, STA can join external WiFi simultaneously */
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Limit TX power to reduce current spikes that cause brownout resets.
    // esp_wifi_set_max_tx_power takes units of 0.25 dBm, so 32 = 8 dBm.
    // Plenty of range for an in-car OBD dongle.
    int8_t tx_power = 32; // 8 dBm
    esp_wifi_set_max_tx_power(tx_power);

    ESP_LOGI(TAG, "WiFi AP started - SSID: %s, IP: %s, TX: %d dBm", device_ssid, WIFI_AP_IP, tx_power / 4);

    /* Apply the persisted mode intent (delegate to a small helper so the
     * host eval gate can exercise the boot-time gating contract without
     * going through esp_wifi_init/start). */
    wifi_ap_boot_apply_sta_intent();

    /* Start the WS server unconditionally so the dongle is reachable over
     * either netif as soon as the AP banner is up. Idempotent — see
     * ws_server.c's `server != NULL` guard. Pre-2026-05-17 the server
     * gated on first AP client; that prevented over-LAN tooling and
     * headless setup. P-25 (security review of always-listening posture)
     * filed in docs/PHASE_2_PREREQUISITES.md. */
    if (!ws_server_is_running()) {
        esp_err_t ws_rc = ws_server_start();
        if (ws_rc != ESP_OK) {
            ESP_LOGW(TAG, "WS server start at boot returned %s",
                     esp_err_to_name(ws_rc));
        }
    } else {
        ESP_LOGD(TAG, "WS server already running (boot idempotency path)");
    }

    return ESP_OK;
}

/*
 * Honor the persisted `wifi_mode` intent at boot. If the operator's last
 * set was AP_ONLY (NVS `wifi_mode` = "ap"), keep STA down even when creds
 * remain stored for a later toggle. If APSTA (default-when-missing, or
 * explicit "sta"), load stored creds and call into the existing
 * esp_wifi_set_config + esp_wifi_connect path.
 *
 * Closes the contract gap caught at Tier 1 of the 2026-05-17 smoke test.
 * Default-when-missing is APSTA so a firmware upgrade onto a customer
 * dongle with stored creds preserves the legacy auto-reconnect.
 */
void wifi_ap_boot_apply_sta_intent(void) {
    if (wifi_get_mode_intent() == WIFI_MODE_INTENT_AP_ONLY) {
        ESP_LOGI(TAG, "Skipping STA auto-connect: intent=AP_ONLY");
        return;
    }

    char sta_ssid[33] = {0};
    char sta_pass[65] = {0};
    if (nvs_manager_load_string(WIFI_STA_SSID_NVS_KEY, sta_ssid, sizeof(sta_ssid)) != ESP_OK
        || sta_ssid[0] == '\0') {
        return;  /* no stored creds — nothing to auto-connect */
    }
    nvs_manager_load_string(WIFI_STA_PASS_NVS_KEY, sta_pass, sizeof(sta_pass));
    ESP_LOGI(TAG, "Auto-connecting STA to: %s", sta_ssid);
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, sta_pass, sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();
}

esp_err_t wifi_client_connect(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!wifi_running) {
        ESP_LOGE(TAG, "WiFi not running");
        return ESP_ERR_INVALID_STATE;
    }
    esp_wifi_disconnect();
    /* Persist for auto-reconnect on boot */
    nvs_manager_save_string(WIFI_STA_SSID_NVS_KEY, ssid);
    nvs_manager_save_string(WIFI_STA_PASS_NVS_KEY, password ? password : "");

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (password) strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);

    esp_err_t r = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (r != ESP_OK) return r;
    sta_retry_count = 0;
    ESP_LOGI(TAG, "Connecting STA to: %s", ssid);
    return esp_wifi_connect();
}

esp_err_t wifi_client_disconnect(void) {
    sta_connected = false;
    sta_ip_str[0] = '\0';
    nvs_manager_save_string(WIFI_STA_SSID_NVS_KEY, "");
    return esp_wifi_disconnect();
}

bool wifi_client_is_connected(void) {
    return sta_connected;
}

const char* wifi_client_get_ip(void) {
    return sta_ip_str;
}

uint64_t wifi_ap_get_serial_number(void) {
    return device_serial;
}

const char* wifi_ap_get_ssid(void) {
    return device_ssid;
}

esp_err_t wifi_ap_stop(void) {
    if (!wifi_running) {
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

bool wifi_ap_is_running(void) {
    return wifi_running;
}

uint8_t wifi_ap_get_client_count(void) {
    return connected_clients;
}

/* ====================================================================== */
/* STA-credential family (NVS-only — no radio side effects)               */
/* ====================================================================== */

esp_err_t wifi_client_set_creds(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && password[0] != '\0'
        && strlen(password) < WIFI_STA_PASSWORD_MIN_LEN) {
        /* WPA2 floor: either empty (open net) or ≥ WIFI_STA_PASSWORD_MIN_LEN. */
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t r = nvs_manager_save_string(WIFI_STA_SSID_NVS_KEY, ssid);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "failed to save STA SSID: %s", esp_err_to_name(r));
        return r;
    }
    r = nvs_manager_save_string(WIFI_STA_PASS_NVS_KEY, password ? password : "");
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "failed to save STA pass: %s", esp_err_to_name(r));
        return r;
    }
    ESP_LOGI(TAG, "STA creds stored for SSID '%s'", ssid);
    return ESP_OK;
}

esp_err_t wifi_client_clear_creds(void) {
    esp_err_t r = nvs_manager_save_string(WIFI_STA_SSID_NVS_KEY, "");
    if (r != ESP_OK) return r;
    return nvs_manager_save_string(WIFI_STA_PASS_NVS_KEY, "");
}

bool wifi_client_creds_stored(void) {
    char ssid[33] = {0};
    esp_err_t r = nvs_manager_load_string(WIFI_STA_SSID_NVS_KEY, ssid, sizeof(ssid));
    if (r != ESP_OK) return false;
    return ssid[0] != '\0';
}

/* ====================================================================== */
/* WiFi mode intent                                                       */
/* ====================================================================== */

/* Internal helper: drop the STA association without touching NVS. Used
 * by wifi_set_mode_intent(AP_ONLY); preserves stored creds so a later
 * wifi_set_mode_intent(APSTA) can rejoin without re-running wifi_sta_set. */
static esp_err_t sta_radio_stop_preserving_creds(void) {
    sta_connected = false;
    sta_ip_str[0] = '\0';
    sta_retry_count = STA_MAX_RETRIES + 1;  /* suppress auto-reconnect */
    esp_err_t r = esp_wifi_disconnect();
    if (r == ESP_ERR_WIFI_NOT_STARTED || r == ESP_ERR_WIFI_NOT_INIT) {
        /* Radio not running yet — disconnect is a no-op, not an error,
         * the operator just set intent early. */
        return ESP_OK;
    }
    return r;
}

esp_err_t wifi_set_mode_intent(wifi_mode_intent_t intent) {
    if (intent != WIFI_MODE_INTENT_AP_ONLY && intent != WIFI_MODE_INTENT_APSTA) {
        return ESP_ERR_INVALID_ARG;
    }

    if (intent == WIFI_MODE_INTENT_APSTA) {
        char ssid[33] = {0};
        char pass[65] = {0};
        esp_err_t r = nvs_manager_load_string(WIFI_STA_SSID_NVS_KEY, ssid, sizeof(ssid));
        if (r != ESP_OK || ssid[0] == '\0') {
            ESP_LOGW(TAG, "wifi_set_mode_intent(APSTA): no stored STA creds");
            return ESP_ERR_NOT_FOUND;
        }
        /* pass may legitimately be empty (open network) — ignore load error. */
        nvs_manager_load_string(WIFI_STA_PASS_NVS_KEY, pass, sizeof(pass));

        /* Delegate to existing connect path. wifi_client_connect's NVS
         * re-save is idempotent. Eval scenario #6 pins this delegation. */
        r = wifi_client_connect(ssid, pass);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "wifi_set_mode_intent(APSTA): connect failed: %s",
                     esp_err_to_name(r));
            return r;
        }

        esp_err_t save_rc = nvs_manager_save_string(
            WIFI_MODE_INTENT_NVS_KEY, WIFI_MODE_INTENT_NVS_VAL_STA);
        if (save_rc != ESP_OK) {
            ESP_LOGW(TAG, "wifi_set_mode_intent: NVS write failed: %s",
                     esp_err_to_name(save_rc));
            /* Connection is up; persistence failed. Surface but don't roll back. */
        }
        return ESP_OK;
    }

    /* AP_ONLY: drop STA without wiping creds. */
    esp_err_t stop_rc = sta_radio_stop_preserving_creds();
    esp_err_t save_rc = nvs_manager_save_string(
        WIFI_MODE_INTENT_NVS_KEY, WIFI_MODE_INTENT_NVS_VAL_AP);
    if (save_rc != ESP_OK) {
        ESP_LOGW(TAG, "wifi_set_mode_intent(AP_ONLY): NVS write failed: %s",
                 esp_err_to_name(save_rc));
    }
    return stop_rc;
}

wifi_mode_intent_t wifi_get_mode_intent(void) {
    char val[8] = {0};
    esp_err_t r = nvs_manager_load_string(WIFI_MODE_INTENT_NVS_KEY, val, sizeof(val));
    if (r != ESP_OK) {
        /* Default-when-missing is APSTA so a firmware upgrade onto a
         * customer dongle that has stored creds but no mode_intent key
         * preserves the legacy auto-reconnect behavior. Explicit
         * `wifi_mode ap` writes the "ap" value and turns the boot-time
         * STA gate on permanently. Caught at Tier 1 of the
         * 2026-05-17 smoke test. */
        return WIFI_MODE_INTENT_APSTA;
    }
    if (strcmp(val, WIFI_MODE_INTENT_NVS_VAL_AP) == 0) {
        return WIFI_MODE_INTENT_AP_ONLY;
    }
    /* Any other value (including the explicit "sta" string) → APSTA. */
    return WIFI_MODE_INTENT_APSTA;
}

bool wifi_feature_uses_cloud_network(int feature_id) {
    /* Keep in sync with feature_manager.h's feature_id_t enum. The
     * predicate is "active feature touches the cloud network at all" —
     * cmd_wifi_mode uses this to refuse a radio swap mid-upload. The
     * int parameter avoids pulling feature_manager.h into wifi_ap.h. */
    enum {
        FM_NONE = 0,
        FM_WOT_LOGGING,
        FM_LIVE_TUNE,
        FM_PHASE2_FLASH,
        FM_DTC,
        FM_BLE_PAIRING,
        FM_VIN_PAIRING,
    };
    switch (feature_id) {
        case FM_VIN_PAIRING:   /* /register + /license */
        case FM_WOT_LOGGING:   /* WOT log upload to cloud */
            return true;
        default:
            return false;
    }
}

