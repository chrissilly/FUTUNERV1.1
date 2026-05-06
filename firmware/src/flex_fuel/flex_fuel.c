#include "flex_fuel.h"
#include "esp_log.h"

static const char *TAG = "flex_fuel";

esp_err_t flex_fuel_init(void)
{
    ESP_LOGI(TAG, "Flex fuel module initialized (stub)");
    return ESP_OK;
}

void flex_fuel_update(void)
{
    /* Stub — will implement blend engine polling loop */
}
