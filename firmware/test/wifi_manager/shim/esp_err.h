/* Host shim for esp_err.h — see firmware/test/feature_manager/shim/esp_err.h
 * for the canonical version. Duplicated locally so this test compiles
 * standalone. On-target, ESP-IDF supplies the real header. */
#ifndef HOST_SHIM_ESP_ERR_H
#define HOST_SHIM_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                          0
#define ESP_FAIL                       -1

#define ESP_ERR_NO_MEM                  0x101
#define ESP_ERR_INVALID_ARG             0x102
#define ESP_ERR_INVALID_STATE           0x103
#define ESP_ERR_INVALID_SIZE            0x104
#define ESP_ERR_NOT_FOUND               0x105
#define ESP_ERR_NOT_SUPPORTED           0x106
#define ESP_ERR_TIMEOUT                 0x107

#define ESP_ERR_WIFI_BASE               0x3000
#define ESP_ERR_WIFI_NOT_INIT          (ESP_ERR_WIFI_BASE + 1)
#define ESP_ERR_WIFI_NOT_STARTED       (ESP_ERR_WIFI_BASE + 2)

const char *esp_err_to_name(esp_err_t code);

#endif /* HOST_SHIM_ESP_ERR_H */
