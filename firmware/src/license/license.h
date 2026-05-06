#ifndef LICENSE_H
#define LICENSE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "license_config.h"
#include "feature_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * license — VIN-lifetime license cache + cloud sync.
 *
 * Per docs/SCALE_ARCHITECTURE_PROPOSAL.md §6 the licensing model is
 * a single `paid` flag per VIN, forever-cached on the dongle, with
 * server-side revoke for fraud/safety reasons. The license module
 * owns:
 *
 *   - The persisted cache (NVS keys defined in license_config.h):
 *     paid (u8), vin (str), revoked (u8), revoked_reason (str),
 *     last_sync (u32 ms), present (u8).
 *
 *   - The cloud round-trips the dongle uses to publish/refresh state:
 *       POST /api/v1/device/register   — publishes mac/vin/boxcode
 *       GET  /api/v1/license            — fetches paid/revoked state
 *
 *   - The query API used by every gated feature:
 *       license_can_run_feature(id, ecu_vin, reason_out, reason_cap)
 *     Diagnostic features (FEATURE_DTC, FEATURE_VIN_PAIRING) are
 *     always allowed. Phase-1 / Phase-2 features (FEATURE_WOT_LOGGING,
 *     FEATURE_LIVE_TUNE, FEATURE_PHASE2_FLASH, FEATURE_BLE_PAIRING)
 *     require:
 *       (a) cache present + paid + not revoked, AND
 *       (b) cached VIN equals current ECU VIN under ISO-3779
 *           normalization (uppercase, leading/trailing whitespace
 *           trimmed).
 *
 *   The wiring of license_can_run_feature() into the existing
 *   command handlers (wot_log_commands, future live_tune, future
 *   phase2_flash) is deferred per Sean's directive — the gate API
 *   ships in this PR; per-feature gate-add lands with each feature's
 *   own prompt.
 *
 * Transport injection: HTTP and NVS are accessed exclusively through
 * function-pointer interfaces (license_http_iface_t, license_nvs_iface_t)
 * supplied at init. On target, the adapters wrap esp_http_client and
 * nvs_manager. In the host unit test, mocks replace them so 200 / 401
 * / 5xx / VIN-mismatch / revoke can be exercised deterministically.
 */

/* ------------------------------------------------------------------ */
/* In-memory license state                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    bool        present;                                /* false until first cache load or sync */
    bool        paid;
    bool        revoked;
    char        vin[LICENSE_VIN_BUF_LEN];               /* VIN this license applies to. Empty = unbound. */
    char        revoked_reason[LICENSE_REVOKE_REASON_MAX];
    uint32_t    last_sync_ms;                           /* monotonic clock ms; 0 if never synced */
} license_state_t;

/* ------------------------------------------------------------------ */
/* HTTP transport interface                                             */
/* ------------------------------------------------------------------ */

/*
 * Synchronous HTTP GET with optional Bearer auth.
 *
 * url:         full URL (host + path).
 * bearer:      auth token string (without "Bearer " prefix). NULL → no
 *              Authorization header. Empty string → still no header.
 * body_out:    response body buffer (caller-owned).
 * body_cap:    capacity of body_out.
 * body_len_out:bytes written into body_out (clamped at body_cap).
 * timeout_ms:  per-call deadline.
 * user_ctx:    opaque pointer carried through from the iface struct.
 *
 * Return value:
 *   > 0 — HTTP status code (200, 401, 503, …).
 *   < 0 — transport error (DNS / connect / TLS / timeout). The exact
 *         negative value is opaque; callers only check the sign.
 */
typedef int (*license_http_get_fn_t)(const char *url,
                                     const char *bearer,
                                     uint8_t    *body_out,
                                     size_t      body_cap,
                                     size_t     *body_len_out,
                                     uint32_t    timeout_ms,
                                     void       *user_ctx);

/*
 * Synchronous HTTP POST with JSON body. Same return semantics as
 * license_http_get_fn_t. body is the request body (already JSON-
 * encoded by the caller).
 */
typedef int (*license_http_post_fn_t)(const char    *url,
                                      const char    *bearer,
                                      const uint8_t *body,
                                      size_t         body_len,
                                      uint8_t       *resp_out,
                                      size_t         resp_cap,
                                      size_t        *resp_len_out,
                                      uint32_t       timeout_ms,
                                      void          *user_ctx);

typedef struct {
    license_http_get_fn_t  get;
    license_http_post_fn_t post;
    void                  *user_ctx;
} license_http_iface_t;

/* ------------------------------------------------------------------ */
/* NVS transport interface                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    esp_err_t (*save_string)(const char *key, const char *value, void *user_ctx);
    esp_err_t (*load_string)(const char *key, char *value_out, size_t max_len, void *user_ctx);
    esp_err_t (*save_uint32)(const char *key, uint32_t value, void *user_ctx);
    esp_err_t (*load_uint32)(const char *key, uint32_t *value_out, void *user_ctx);
    void *user_ctx;
} license_nvs_iface_t;

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    license_http_iface_t http;
    license_nvs_iface_t  nvs;
} license_module_config_t;

/*
 * Initialize the license module. Loads the persisted cache from NVS
 * into the in-memory state (best effort — missing keys leave state at
 * defaults). Idempotent.
 */
esp_err_t license_init(const license_module_config_t *cfg);
void      license_deinit(void);

/* ------------------------------------------------------------------ */
/* Persistence                                                          */
/* ------------------------------------------------------------------ */

esp_err_t license_load_cache(void);
esp_err_t license_save_cache(void);

/* ------------------------------------------------------------------ */
/* Cloud round-trips                                                    */
/* ------------------------------------------------------------------ */

/*
 * POST /api/v1/device/register with the supplied identifiers. Returns
 * the HTTP status code (or ESP_FAIL on transport error). On 409 the
 * caller should NOT proceed to license_fetch — the cloud believes
 * this device is bound to a different VIN.
 *
 * mac/vin/boxcode may be empty strings; mac is required.
 */
esp_err_t license_post_register(const char *mac,
                                const char *vin,
                                const char *boxcode,
                                int        *http_status_out,
                                char       *err_out,
                                size_t      err_cap);

/*
 * GET /api/v1/license, parse the JSON, update the in-memory cache,
 * persist via license_save_cache(). On 401 the in-memory cache is
 * cleared (token invalid). On 5xx / network error the cache is left
 * untouched (offline grace per §6.3 is forever).
 */
esp_err_t license_fetch(int    *http_status_out,
                        char   *err_out,
                        size_t  err_cap);

/* ------------------------------------------------------------------ */
/* Query API                                                            */
/* ------------------------------------------------------------------ */

/*
 * Returns true if `feature_id` is allowed to run given the current
 * cached license + the supplied ECU VIN. Diagnostic features
 * (FEATURE_DTC, FEATURE_VIN_PAIRING) are always allowed regardless of
 * license state.
 *
 * current_ecu_vin may be NULL or empty — in that case the VIN-match
 * check is skipped (e.g. when no ECU is connected) and the result is
 * gated only by paid + not-revoked + cache-present.
 *
 * reason_out / reason_cap: optional human-readable reason populated
 * when the function returns false. May be NULL/0.
 */
bool license_can_run_feature(feature_id_t  feature_id,
                             const char   *current_ecu_vin,
                             char         *reason_out,
                             size_t        reason_cap);

/* Snapshot accessor — returns a pointer to the module's in-memory
 * state. Stable across calls; caller must not mutate. */
const license_state_t *license_get_state(void);

/* ------------------------------------------------------------------ */
/* Test scaffolding                                                     */
/* ------------------------------------------------------------------ */

/*
 * Test-only: directly seed the in-memory state. Bypasses NVS load.
 * Used by host unit tests to set up a known cached state without
 * round-tripping through save_cache + load_cache. On target this
 * should not be called.
 */
void license_test_seed(const license_state_t *seed);

/*
 * Normalize a VIN per ISO 3779: trim leading/trailing whitespace,
 * uppercase. Output buffer must be >= LICENSE_VIN_BUF_LEN bytes.
 * NULL or empty input → empty output. Exposed so other modules and
 * tests can normalize at the comparison site without re-implementing.
 */
void license_normalize_vin(const char *in, char *out, size_t out_cap);

/*
 * Returns true if the auth_token key exists in NVS with a non-empty
 * value. Cheap predicate used by vin_pairing to refuse the start when
 * the device has not been enrolled.
 */
bool license_has_auth_token(void);

#ifdef __cplusplus
}
#endif

#endif /* LICENSE_H */
