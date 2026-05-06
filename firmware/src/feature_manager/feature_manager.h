#ifndef FEATURE_MANAGER_H
#define FEATURE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * feature_manager — central arbiter for FUTUNER user-visible features.
 *
 * Every feature (WOT logging, live tune, Phase 2 flash, DTC read/clear,
 * BLE pairing, VIN pairing, …) defaults to inactive at boot, runs only
 * when explicitly started, and yields cleanly before another feature
 * starts.
 *
 * Hard rule: AT MOST ONE feature is active at any time. The manager
 * enforces this — features must not bypass it.
 *
 * See FUTV1.1/CLAUDE.md §"Feature ON/OFF discipline" for the project
 * rule this module exists to enforce.
 */

typedef enum {
    FEATURE_NONE = 0,
    FEATURE_WOT_LOGGING,
    FEATURE_LIVE_TUNE,
    FEATURE_PHASE2_FLASH,
    FEATURE_DTC,
    FEATURE_BLE_PAIRING,
    FEATURE_VIN_PAIRING,
    FEATURE_COUNT
} feature_id_t;

typedef struct {
    feature_id_t id;
    const char  *name;
    esp_err_t  (*start)(void);
    esp_err_t  (*stop)(void);
    bool       (*is_running)(void);
} feature_descriptor_t;

/*
 * Initialize the manager. Must be called once at boot, before any
 * register/start/stop call. Idempotent.
 */
esp_err_t feature_manager_init(void);

/*
 * Register a feature descriptor. The caller retains ownership of `desc`
 * and the strings it points at — they must outlive the manager (a static
 * const struct in the feature's translation unit is the expected shape).
 *
 * Returns:
 *   ESP_OK                  on success.
 *   ESP_ERR_INVALID_STATE   if init() was not called, or this id was
 *                           already registered.
 *   ESP_ERR_INVALID_ARG     desc is NULL, id == FEATURE_NONE, id >=
 *                           FEATURE_COUNT, or any callback / name is NULL.
 *   ESP_ERR_TIMEOUT         could not acquire the manager mutex.
 */
esp_err_t feature_manager_register(const feature_descriptor_t *desc);

/*
 * Request that feature `id` become active.
 *
 * Behavior:
 *   id == FEATURE_NONE or id >= FEATURE_COUNT
 *       → ESP_ERR_INVALID_ARG; err_out populated.
 *   id not registered
 *       → ESP_ERR_NOT_FOUND; err_out populated.
 *   already active == id
 *       → ESP_OK, idempotent (start() is NOT called again).
 *   active == FEATURE_NONE
 *       → calls id.start(); on success, becomes active.
 *   active == other
 *       → logs warning, calls other.stop(), polls until
 *         other.is_running() returns false (bounded by
 *         FEATURE_MGR_STOP_TIMEOUT_MS), then calls id.start().
 *         If stop() returns non-ESP_OK or is_running times out, the
 *         swap aborts: active stays on the failed-stop feature, X.start()
 *         is NOT called, and err_out is populated.
 *
 * err_out / err_len may be NULL/0 to suppress error message capture.
 */
esp_err_t feature_manager_request_start(feature_id_t id, char *err_out, size_t err_len);

/*
 * Request that feature `id` stop.
 *
 * Behavior:
 *   id == FEATURE_NONE
 *       → ESP_OK, no-op (consistent with "stop a non-active feature is OK").
 *   id >= FEATURE_COUNT
 *       → ESP_ERR_INVALID_ARG.
 *   active == id
 *       → calls id.stop(); on success, active becomes FEATURE_NONE and
 *         the function returns the value stop() returned. If stop()
 *         returned non-OK, active stays on this feature.
 *   active != id
 *       → ESP_OK, no-op (the feature is already not active).
 */
esp_err_t feature_manager_request_stop(feature_id_t id);

/*
 * Returns the currently active feature, or FEATURE_NONE if no feature is
 * active. Snapshot at the time of the call; can change on next call.
 */
feature_id_t feature_manager_active(void);

/*
 * Returns a stable, human-readable name for the active feature. Returns
 * "none" when no feature is active. Pointer is valid for the lifetime of
 * the registered descriptor (string literal expected).
 */
const char *feature_manager_active_name(void);

#ifdef __cplusplus
}
#endif

#endif /* FEATURE_MANAGER_H */
