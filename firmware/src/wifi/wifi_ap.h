#ifndef WIFI_AP_H
#define WIFI_AP_H

#include "esp_err.h"
#include "config/wifi_config.h"   /* wifi_mode_intent_t + relocated knobs */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NVS key for the (optional) per-device SoftAP password override. The
 * default password lives in wifi_config.h as WIFI_AP_PASSWORD_DEFAULT. */
#define WIFI_AP_PASSWORD_NVS_KEY        "wifi_password"

#define WIFI_AP_IP                      "192.168.10.1"
#define WIFI_AP_GATEWAY                 "192.168.10.1"
#define WIFI_AP_NETMASK                 "255.255.255.0"

/* STA (client) mode — joins external WiFi while AP stays active */
#define WIFI_STA_SSID_NVS_KEY           "sta_ssid"
#define WIFI_STA_PASS_NVS_KEY           "sta_pass"

/* -------------------------------------------------------------------- */
/* AP lifecycle                                                         */
/* -------------------------------------------------------------------- */

esp_err_t wifi_ap_init(void);
esp_err_t wifi_ap_start(void);
esp_err_t wifi_ap_stop(void);

bool      wifi_ap_is_running(void);
uint8_t   wifi_ap_get_client_count(void);

uint64_t    wifi_ap_get_serial_number(void);
const char *wifi_ap_get_ssid(void);

/* -------------------------------------------------------------------- */
/* STA client mode (connects to external WiFi)                          */
/* -------------------------------------------------------------------- */

/* Legacy combined "save creds + start STA" call. Kept stable for the
 * legacy `wifi_connect` command and for first-boot pairing. Internally
 * persists ssid/password to NVS and calls esp_wifi_connect(). */
esp_err_t   wifi_client_connect(const char *ssid, const char *password);

/* Legacy combined "drop STA + wipe stored SSID" call. Kept stable for
 * the legacy `wifi_disconnect` command. NOT used by the mode-intent
 * path (which preserves creds across an AP_ONLY toggle). */
esp_err_t   wifi_client_disconnect(void);

bool        wifi_client_is_connected(void);
const char *wifi_client_get_ip(void);

/* -------------------------------------------------------------------- */
/* STA credential helpers (new in 2026-05-17 — owner-specified API)     */
/* -------------------------------------------------------------------- */

/* Store STA credentials in NVS without touching the radio. SSID must be
 * non-empty and ≤ 32 chars; password must be empty (open network) or
 * ≥ WIFI_STA_PASSWORD_MIN_LEN. */
esp_err_t   wifi_client_set_creds(const char *ssid, const char *password);

/* Erase the stored STA credentials from NVS without touching the radio.
 * After this call wifi_client_creds_stored() returns false. */
esp_err_t   wifi_client_clear_creds(void);

/* True iff a non-empty SSID is currently stored in NVS. */
bool        wifi_client_creds_stored(void);

/* -------------------------------------------------------------------- */
/* WiFi mode intent (new in 2026-05-17 — owner-specified API)           */
/* -------------------------------------------------------------------- */

/*
 * Persist the operator-selected WiFi mode intent and act on it:
 *   WIFI_MODE_INTENT_APSTA   → load stored STA creds and call
 *                              wifi_client_connect(); if no creds are
 *                              stored, returns ESP_ERR_NOT_FOUND and
 *                              does NOT change NVS.
 *   WIFI_MODE_INTENT_AP_ONLY → disconnect any active STA association
 *                              WITHOUT wiping the stored credentials.
 *
 * On success the NVS WIFI_MODE_INTENT_NVS_KEY is updated to the matching
 * string value (WIFI_MODE_INTENT_NVS_VAL_AP / _STA).
 */
esp_err_t           wifi_set_mode_intent(wifi_mode_intent_t intent);

/*
 * Return the currently persisted intent. Defaults to
 * WIFI_MODE_INTENT_AP_ONLY when no intent has ever been stored or NVS
 * reads fail.
 */
wifi_mode_intent_t  wifi_get_mode_intent(void);

/*
 * True iff `feature_id` (a value of feature_manager.h's feature_id_t,
 * passed as int to avoid coupling this header to feature_manager.h)
 * represents a feature that uses the cloud network. Used by
 * cmd_wifi_mode's guard against radio toggling during an active upload
 * or pairing flow.
 */
bool wifi_feature_uses_cloud_network(int feature_id);

/*
 * Apply the persisted mode intent at boot: skip STA if AP_ONLY, otherwise
 * load stored creds and invoke esp_wifi_set_config + esp_wifi_connect.
 * Public so the host eval gate can exercise it without booting wifi.
 */
void wifi_ap_boot_apply_sta_intent(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_AP_H */
