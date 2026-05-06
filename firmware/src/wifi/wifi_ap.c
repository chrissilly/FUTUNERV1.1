#include "wifi_ap.h"
#include "nvs/nvs_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/inet.h"
#include "websocket/ws_server.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WIFI_AP";

static bool wifi_initialized = false;
static bool wifi_running = false;
static uint8_t connected_clients = 0;
static esp_netif_t *ap_netif = NULL;
static esp_netif_t *sta_netif = NULL;
static uint64_t device_serial = 0;
static char device_ssid[32] = {0};
static bool sta_connected = false;
static char sta_ip_str[16] = {0};
static uint8_t sta_retry_count = 0;
#define STA_MAX_RETRIES 5

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
                /* First client: spin up the HTTP + WebSocket server */
                if (connected_clients == 1 && !ws_server_is_running()) {
                    ESP_LOGI(TAG, "First client - starting web server");
                    ws_server_start();
                }
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
                /* AP down - make sure web server is off too */
                if (ws_server_is_running()) {
                    ws_server_stop();
                }
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

    /* Auto-connect STA if NVS has saved credentials */
    char sta_ssid[33] = {0};
    char sta_pass[65] = {0};
    if (nvs_manager_load_string(WIFI_STA_SSID_NVS_KEY, sta_ssid, sizeof(sta_ssid)) == ESP_OK
        && sta_ssid[0] != '\0') {
        nvs_manager_load_string(WIFI_STA_PASS_NVS_KEY, sta_pass, sizeof(sta_pass));
        ESP_LOGI(TAG, "Auto-connecting STA to: %s", sta_ssid);
        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, sta_pass, sizeof(sta_cfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_connect();
    }

    return ESP_OK;
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

