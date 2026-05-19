/* Host shim for esp_wifi.h — provides enough types/symbols for wifi_ap.c
 * to compile. The actual ESP-IDF wifi calls (init/start/connect/...) are
 * stubbed in host_shim.c as observable no-ops. The new STA-creds and
 * mode-intent functions only reach esp_wifi_disconnect / esp_wifi_connect
 * / esp_wifi_set_config, which the spy in host_shim.c records for
 * test_wifi_mode_sta_invokes_existing_wifi_client_connect and
 * test_wifi_mode_ap_clears_active_sta_connection. */
#ifndef HOST_SHIM_ESP_WIFI_H
#define HOST_SHIM_ESP_WIFI_H

#include "esp_err.h"
#include "esp_event.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    WIFI_MODE_NULL  = 0,
    WIFI_MODE_STA   = 1,
    WIFI_MODE_AP    = 2,
    WIFI_MODE_APSTA = 3,
} wifi_mode_t;

typedef enum {
    WIFI_IF_STA = 0,
    WIFI_IF_AP  = 1,
} wifi_interface_t;

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WPA2_PSK = 3,
} wifi_auth_mode_t;

typedef struct {
    bool required;
} wifi_pmf_config_t;

typedef struct {
    uint8_t              ssid[32];
    uint8_t              password[64];
    uint8_t              ssid_len;
    uint8_t              channel;
    wifi_auth_mode_t     authmode;
    uint8_t              ssid_hidden;
    uint8_t              max_connection;
    wifi_pmf_config_t    pmf_cfg;
} wifi_ap_config_t;

typedef struct {
    uint8_t  ssid[32];
    uint8_t  password[64];
} wifi_sta_config_t;

typedef union {
    wifi_ap_config_t  ap;
    wifi_sta_config_t sta;
} wifi_config_t;

typedef struct {
    int dummy;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})

/* Event symbols referenced by the event handler in wifi_ap.c. */
extern esp_event_base_t WIFI_EVENT;
extern esp_event_base_t IP_EVENT;

#define WIFI_EVENT_AP_STACONNECTED      100
#define WIFI_EVENT_AP_STADISCONNECTED   101
#define WIFI_EVENT_AP_START             102
#define WIFI_EVENT_AP_STOP              103
#define WIFI_EVENT_STA_DISCONNECTED     104
#define IP_EVENT_STA_GOT_IP             200

typedef struct {
    uint8_t reason;
} wifi_event_sta_disconnected_t;

esp_err_t esp_wifi_init           (const wifi_init_config_t *cfg);
esp_err_t esp_wifi_start          (void);
esp_err_t esp_wifi_stop           (void);
esp_err_t esp_wifi_set_mode       (wifi_mode_t mode);
esp_err_t esp_wifi_set_config     (wifi_interface_t iface, wifi_config_t *cfg);
esp_err_t esp_wifi_connect        (void);
esp_err_t esp_wifi_disconnect     (void);
esp_err_t esp_wifi_set_max_tx_power(int8_t power);

#endif /* HOST_SHIM_ESP_WIFI_H */
