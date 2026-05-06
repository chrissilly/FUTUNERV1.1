#ifndef WIFI_AP_H
#define WIFI_AP_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define WIFI_AP_PASSWORD_DEFAULT "password"
#define WIFI_AP_PASSWORD_NVS_KEY "wifi_password"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONNECTIONS 4
#define WIFI_AP_IP "192.168.10.1"
#define WIFI_AP_GATEWAY "192.168.10.1"
#define WIFI_AP_NETMASK "255.255.255.0"

/* STA (client) mode — joins external WiFi while AP stays active */
#define WIFI_STA_SSID_NVS_KEY "sta_ssid"
#define WIFI_STA_PASS_NVS_KEY "sta_pass"

esp_err_t wifi_ap_init(void);
esp_err_t wifi_ap_start(void);
esp_err_t wifi_ap_stop(void);

bool wifi_ap_is_running(void);
uint8_t wifi_ap_get_client_count(void);

uint64_t wifi_ap_get_serial_number(void);
const char* wifi_ap_get_ssid(void);

/* STA client mode (connects to external WiFi) */
esp_err_t wifi_client_connect(const char *ssid, const char *password);
esp_err_t wifi_client_disconnect(void);
bool wifi_client_is_connected(void);
const char* wifi_client_get_ip(void);

#endif // WIFI_AP_H

