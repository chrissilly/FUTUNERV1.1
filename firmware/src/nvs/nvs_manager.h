#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "ecu_info.h"

esp_err_t nvs_manager_init(void);
esp_err_t nvs_manager_deinit(void);

esp_err_t nvs_manager_save_ecu_info(const ecu_info_t *info);
esp_err_t nvs_manager_load_ecu_info(ecu_info_t *info);
esp_err_t nvs_manager_clear_ecu_info(void);

esp_err_t nvs_manager_erase_ecu_info(void);

esp_err_t nvs_manager_save_string(const char *key, const char *value);
esp_err_t nvs_manager_load_string(const char *key, char *value, size_t max_len);

esp_err_t nvs_manager_save_uint32(const char *key, uint32_t value);
esp_err_t nvs_manager_load_uint32(const char *key, uint32_t *value);

bool nvs_manager_is_initialized(void);

#endif // NVS_MANAGER_H

