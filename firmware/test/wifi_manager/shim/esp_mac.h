#ifndef HOST_SHIM_ESP_MAC_H
#define HOST_SHIM_ESP_MAC_H

#include "esp_err.h"
#include <stdint.h>

esp_err_t esp_efuse_mac_get_default(uint8_t *mac);

#endif /* HOST_SHIM_ESP_MAC_H */
