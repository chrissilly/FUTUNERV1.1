/* Host shim header for nvs/nvs_manager.h — only the string get/set
 * pair is referenced by wifi_ap.c's new code paths. The remaining real
 * APIs (ECU info, uint32, etc.) are intentionally absent here; if a
 * future test pulls in a call site that needs them, extend this shim. */
#ifndef HOST_SHIM_NVS_MANAGER_H
#define HOST_SHIM_NVS_MANAGER_H

#include "esp_err.h"
#include <stddef.h>

esp_err_t nvs_manager_save_string(const char *key, const char *value);
esp_err_t nvs_manager_load_string(const char *key, char *value, size_t max_len);

#endif /* HOST_SHIM_NVS_MANAGER_H */
