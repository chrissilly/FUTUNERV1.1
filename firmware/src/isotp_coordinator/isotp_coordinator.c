#include "isotp_coordinator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "ISOTP_COORD";

static isotp_owner_t current_owner = ISOTP_OWNER_NONE;
static SemaphoreHandle_t mutex = NULL;

esp_err_t isotp_coordinator_init(void) {
    if (mutex != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    mutex = xSemaphoreCreateMutex();
    if (mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    current_owner = ISOTP_OWNER_NONE;
    ESP_LOGI(TAG, "ISO-TP coordinator initialized");
    return ESP_OK;
}

bool isotp_coordinator_request(isotp_owner_t owner, uint32_t timeout_ms) {
    if (mutex == NULL) {
        ESP_LOGE(TAG, "Not initialized");
        return false;
    }
    
    if (owner == ISOTP_OWNER_NONE) {
        ESP_LOGE(TAG, "Invalid owner NONE");
        return false;
    }
    
    // Try to acquire mutex
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(mutex, ticks) != pdTRUE) {
        return false;  // Busy
    }
    
    // Check if already owned
    if (current_owner != ISOTP_OWNER_NONE) {
        // Someone else has it
        xSemaphoreGive(mutex);
        return false;
    }
    
    // Grant ownership
    current_owner = owner;
    xSemaphoreGive(mutex);
    
    ESP_LOGD(TAG, "Ownership granted to %d", owner);
    return true;
}

void isotp_coordinator_release(isotp_owner_t owner) {
    if (mutex == NULL) {
        ESP_LOGE(TAG, "Not initialized");
        return;
    }
    
    if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
        if (current_owner == owner) {
            current_owner = ISOTP_OWNER_NONE;
            ESP_LOGD(TAG, "Ownership released by %d", owner);
        } else {
            ESP_LOGW(TAG, "Owner %d tried to release, but current owner is %d", 
                     owner, current_owner);
        }
        xSemaphoreGive(mutex);
    }
}

bool isotp_coordinator_has_ownership(isotp_owner_t owner) {
    if (mutex == NULL) {
        return false;
    }
    
    bool result = false;
    if (xSemaphoreTake(mutex, 0) == pdTRUE) {
        result = (current_owner == owner);
        xSemaphoreGive(mutex);
    }
    return result;
}

isotp_owner_t isotp_coordinator_get_owner(void) {
    if (mutex == NULL) {
        return ISOTP_OWNER_NONE;
    }
    
    isotp_owner_t owner = ISOTP_OWNER_NONE;
    if (xSemaphoreTake(mutex, 0) == pdTRUE) {
        owner = current_owner;
        xSemaphoreGive(mutex);
    }
    return owner;
}

bool isotp_coordinator_is_free(void) {
    return isotp_coordinator_get_owner() == ISOTP_OWNER_NONE;
}

