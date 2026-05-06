#ifndef VIN_PAIRING_H
#define VIN_PAIRING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "vin_pairing_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * vin_pairing — feature lifecycle that drives the dongle ↔ cloud
 * pair-or-refresh sequence.
 *
 * Flow when vin_pairing_run_now() is called (typically from the
 * `vin_pair_now` WS command):
 *   1. Verify ECU VIN is known (connection_manager_get_vin returns
 *      a non-empty string). If not → fail with a clear UI message.
 *   2. Verify Wi-Fi STA is up (wifi_ready callback returns true).
 *      If not → fail; cached license is left untouched.
 *   3. Verify auth_token is set in NVS (license_has_auth_token()).
 *      If not → fail with "device not enrolled" message.
 *   4. POST /api/v1/device/register with {mac, vin, boxcode}.
 *      If 409 → cloud believes the dongle is bound to a different
 *      VIN. Surface the conflict; do NOT call /api/v1/license.
 *   5. GET /api/v1/license. Update license cache, persist to NVS.
 *
 * The whole sequence runs synchronously in vin_pairing_run_now().
 * Internally it brackets the work with feature_manager_request_start
 * (FEATURE_VIN_PAIRING) on entry and feature_manager_request_stop on
 * exit, so the global ON/OFF discipline is honored without the
 * descriptor's start callback doing the work itself (same shape as
 * dtc_read / dtc_clear).
 */

/* Source callbacks — let the host harness inject deterministic
 * fixtures without booting connection_manager + wifi_ap. */
typedef const char * (*vin_pairing_vin_source_fn_t)(void);
typedef const char * (*vin_pairing_boxcode_source_fn_t)(void);
typedef bool         (*vin_pairing_wifi_ready_fn_t)(void);

typedef struct {
    vin_pairing_vin_source_fn_t      vin_source;
    vin_pairing_boxcode_source_fn_t  boxcode_source;
    vin_pairing_wifi_ready_fn_t      wifi_ready;
    /* Device MAC formatted as "AA:BB:CC:DD:EE:FF". Required. */
    const char                      *device_mac;
} vin_pairing_config_t;

esp_err_t vin_pairing_init(const vin_pairing_config_t *cfg);
void      vin_pairing_deinit(void);

/* Idempotent. Hands the FEATURE_VIN_PAIRING descriptor to
 * feature_manager_register(). Called from vin_pairing_init(); also
 * exposed for test scaffolding. */
esp_err_t vin_pairing_register_with_feature_manager(void);

/* Reflects whether vin_pairing currently holds the active slot. */
bool vin_pairing_is_running(void);

/* Trigger the pair-or-refresh sequence. Synchronously runs the
 * register-then-license flow described above. Returns ESP_OK on
 * success; otherwise an esp_err_t code with err_out / err_cap (when
 * non-NULL/0) populated with a human-readable reason for the WS UI. */
esp_err_t vin_pairing_run_now(char *err_out, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* VIN_PAIRING_H */
