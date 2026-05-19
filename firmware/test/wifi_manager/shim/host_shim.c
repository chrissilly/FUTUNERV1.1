/*
 * host_shim.c — host-side stubs for the symbols wifi_ap.c references
 * but the wifi_manager unit tests do not directly exercise (esp_wifi_*,
 * esp_event_*, esp_netif_*, esp_mac, ws_server). Plus a minimal in-RAM
 * NVS that backs nvs_manager_save_string / load_string for the tests
 * that do care.
 *
 * Spies (observable from test_wifi_manager.c):
 *   - g_esp_wifi_disconnect_count
 *   - g_esp_wifi_connect_count
 *   - g_esp_wifi_set_config_iface / .ssid / .password (last call captured)
 *
 * NVS mock:
 *   - nvs_mock_clear(), nvs_mock_set(key, value), nvs_mock_get(key)
 *     are exposed via the wifi_test_mocks.h header below for the tests.
 */

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "websocket/ws_server.h"
#include "nvs/nvs_manager.h"
#include "feature_manager/feature_manager.h"
#include "wifi_test_mocks.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* --------------------------------------------------------------------- */
/* esp_err_to_name — small enough to keep here                            */
/* --------------------------------------------------------------------- */
const char *esp_err_to_name(esp_err_t code) {
    static char buf[24];
    switch (code) {
        case ESP_OK:                    return "ESP_OK";
        case ESP_FAIL:                  return "ESP_FAIL";
        case ESP_ERR_NO_MEM:            return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG:       return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE:     return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND:         return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_TIMEOUT:           return "ESP_ERR_TIMEOUT";
        case ESP_ERR_WIFI_NOT_INIT:     return "ESP_ERR_WIFI_NOT_INIT";
        case ESP_ERR_WIFI_NOT_STARTED:  return "ESP_ERR_WIFI_NOT_STARTED";
        default:
            snprintf(buf, sizeof(buf), "ERR_0x%x", (unsigned)code);
            return buf;
    }
}

/* --------------------------------------------------------------------- */
/* esp_event globals — must initialise from a string literal directly,    */
/* not from another static, to satisfy C's constant-expression rule.      */
/* --------------------------------------------------------------------- */
esp_event_base_t WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t IP_EVENT   = "IP_EVENT";

esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base,
                                              int32_t event_id,
                                              esp_event_handler_t handler,
                                              void *arg,
                                              void *instance) {
    (void)base; (void)event_id; (void)handler; (void)arg; (void)instance;
    return ESP_OK;
}

/* --------------------------------------------------------------------- */
/* esp_wifi spies                                                         */
/* --------------------------------------------------------------------- */
int  g_esp_wifi_disconnect_count   = 0;
int  g_esp_wifi_connect_count      = 0;
int  g_esp_wifi_set_config_count   = 0;
char g_last_set_config_ssid[33]    = {0};
char g_last_set_config_password[65] = {0};

void wifi_test_reset_spies(void) {
    g_esp_wifi_disconnect_count = 0;
    g_esp_wifi_connect_count = 0;
    g_esp_wifi_set_config_count = 0;
    memset(g_last_set_config_ssid, 0, sizeof(g_last_set_config_ssid));
    memset(g_last_set_config_password, 0, sizeof(g_last_set_config_password));
}

esp_err_t esp_wifi_init   (const wifi_init_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t esp_wifi_start  (void) { return ESP_OK; }
esp_err_t esp_wifi_stop   (void) { return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) { (void)mode; return ESP_OK; }

esp_err_t esp_wifi_set_config(wifi_interface_t iface, wifi_config_t *cfg) {
    g_esp_wifi_set_config_count++;
    if (cfg != NULL && iface == WIFI_IF_STA) {
        memcpy(g_last_set_config_ssid, cfg->sta.ssid, sizeof(g_last_set_config_ssid) - 1);
        memcpy(g_last_set_config_password, cfg->sta.password, sizeof(g_last_set_config_password) - 1);
    }
    return ESP_OK;
}

esp_err_t esp_wifi_connect   (void) { g_esp_wifi_connect_count++;    return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { g_esp_wifi_disconnect_count++; return ESP_OK; }
esp_err_t esp_wifi_set_max_tx_power(int8_t power) { (void)power; return ESP_OK; }

/* --------------------------------------------------------------------- */
/* esp_netif / esp_mac / lwip stubs                                       */
/* --------------------------------------------------------------------- */
esp_err_t    esp_netif_init                 (void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_ap (void) { return (esp_netif_t *)1; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return (esp_netif_t *)2; }
esp_err_t    esp_netif_str_to_ip4 (const char *src, esp_ip4_addr_t *dst) {
    (void)src; if (dst) dst->addr = 0; return ESP_OK;
}
esp_err_t    esp_netif_dhcps_stop  (esp_netif_t *n) { (void)n; return ESP_OK; }
esp_err_t    esp_netif_dhcps_start (esp_netif_t *n) { (void)n; return ESP_OK; }
esp_err_t    esp_netif_set_ip_info (esp_netif_t *n, const esp_netif_ip_info_t *i) {
    (void)n; (void)i; return ESP_OK;
}

esp_err_t esp_efuse_mac_get_default(uint8_t *mac) {
    if (!mac) return ESP_ERR_INVALID_ARG;
    static const uint8_t fake[6] = {0x30,0xed,0xa0,0xa5,0x54,0x8c};
    memcpy(mac, fake, sizeof(fake));
    return ESP_OK;
}

/* --------------------------------------------------------------------- */
/* websocket/ws_server stubs                                              */
/* --------------------------------------------------------------------- */
bool      ws_server_is_running(void) { return false; }
esp_err_t ws_server_start    (void) { return ESP_OK; }
esp_err_t ws_server_stop     (void) { return ESP_OK; }

/* --------------------------------------------------------------------- */
/* FreeRTOS / semaphore stubs                                             */
/* --------------------------------------------------------------------- */
static TickType_t g_ticks;
void       vTaskDelay(TickType_t ticks)         { (void)ticks; }
TickType_t xTaskGetTickCount(void)              { return ++g_ticks; }

SemaphoreHandle_t xSemaphoreCreateMutex(void)                       { return NULL; }
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s)               { (void)s; return pdTRUE; }

/* --------------------------------------------------------------------- */
/* In-memory NVS mock                                                     */
/* --------------------------------------------------------------------- */

#define NVS_MOCK_SLOTS 16
#define NVS_KEY_MAX    32
#define NVS_VAL_MAX    96

typedef struct {
    bool used;
    char key[NVS_KEY_MAX];
    char value[NVS_VAL_MAX];
} nvs_slot_t;

static nvs_slot_t g_nvs[NVS_MOCK_SLOTS];

void nvs_mock_clear(void) {
    memset(g_nvs, 0, sizeof(g_nvs));
}

const char *nvs_mock_get(const char *key) {
    for (int i = 0; i < NVS_MOCK_SLOTS; i++) {
        if (g_nvs[i].used && strcmp(g_nvs[i].key, key) == 0) {
            return g_nvs[i].value;
        }
    }
    return NULL;
}

void nvs_mock_set(const char *key, const char *value) {
    /* Update existing. */
    for (int i = 0; i < NVS_MOCK_SLOTS; i++) {
        if (g_nvs[i].used && strcmp(g_nvs[i].key, key) == 0) {
            strncpy(g_nvs[i].value, value ? value : "", NVS_VAL_MAX - 1);
            g_nvs[i].value[NVS_VAL_MAX - 1] = '\0';
            return;
        }
    }
    /* Insert. */
    for (int i = 0; i < NVS_MOCK_SLOTS; i++) {
        if (!g_nvs[i].used) {
            g_nvs[i].used = true;
            strncpy(g_nvs[i].key, key, NVS_KEY_MAX - 1);
            g_nvs[i].key[NVS_KEY_MAX - 1] = '\0';
            strncpy(g_nvs[i].value, value ? value : "", NVS_VAL_MAX - 1);
            g_nvs[i].value[NVS_VAL_MAX - 1] = '\0';
            return;
        }
    }
    /* Out of slots — test bug. */
    fprintf(stderr, "FATAL: NVS mock out of slots writing key '%s'\n", key);
    exit(2);
}

esp_err_t nvs_manager_save_string(const char *key, const char *value) {
    if (!key) return ESP_ERR_INVALID_ARG;
    nvs_mock_set(key, value ? value : "");
    return ESP_OK;
}

esp_err_t nvs_manager_load_string(const char *key, char *value, size_t max_len) {
    if (!key || !value || max_len == 0) return ESP_ERR_INVALID_ARG;
    const char *v = nvs_mock_get(key);
    if (v == NULL) return ESP_ERR_NOT_FOUND;
    strncpy(value, v, max_len - 1);
    value[max_len - 1] = '\0';
    return ESP_OK;
}

/* --------------------------------------------------------------------- */
/* feature_manager mock                                                   */
/* --------------------------------------------------------------------- */
static feature_id_t g_fm_active = FEATURE_NONE;
static const char  *g_fm_active_name = "none";

void wifi_test_set_feature_active(int id, const char *name) {
    g_fm_active = (feature_id_t)id;
    g_fm_active_name = name ? name : "(test)";
}

feature_id_t feature_manager_active(void)     { return g_fm_active; }
const char  *feature_manager_active_name(void){ return g_fm_active_name; }
