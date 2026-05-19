/* Host shim for feature_manager.h — the wifi_manager host test does not
 * exercise feature_manager itself; the predicate
 * `wifi_feature_uses_cloud_network` runs against the bare feature_id_t
 * enum. Keep this subset in sync with the real header. */
#ifndef HOST_SHIM_FEATURE_MANAGER_H
#define HOST_SHIM_FEATURE_MANAGER_H

#include "esp_err.h"

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

feature_id_t       feature_manager_active(void);
const char        *feature_manager_active_name(void);

#endif /* HOST_SHIM_FEATURE_MANAGER_H */
