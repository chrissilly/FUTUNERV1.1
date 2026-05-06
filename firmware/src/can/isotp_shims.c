#include "isotp_shims.h"
#include "can_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "ISOTP_SHIMS";

int isotp_user_send_can(const uint32_t arbitration_id, const uint8_t* data, const uint8_t size) {
    if (data == NULL || size > 8) {
        ESP_LOGE(TAG, "Invalid parameters for CAN send");
        return -1;
    }

    esp_err_t err = can_driver_send(arbitration_id, data, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send CAN message");
        return -1;
    }

    return 0;
}

uint32_t isotp_user_get_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void isotp_user_debug(const char* message, ...) {
    va_list args;
    va_start(args, message);
    
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), message, args);
    
    ESP_LOGD(TAG, "%s", buffer);
    
    va_end(args);
}

