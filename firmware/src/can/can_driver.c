#include "can_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "CAN_DRIVER";
static bool driver_initialized = false;
static bool driver_started = false;
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t can_driver_init(void) {
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create CAN mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (driver_initialized) {
        ESP_LOGW(TAG, "CAN driver already initialized");
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO_NUM, CAN_RX_GPIO_NUM, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = CAN_BAUDRATE;
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TWAI driver: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    driver_initialized = true;
    xSemaphoreGive(s_mutex);
    /* Boot-log fingerprint for quiet-bench Phase 2 verification. Confirms
     * which pins + bitrate + mode the build actually compiled with so a
     * misconfigured board manifests as a one-line config dump instead of
     * silent TX timeouts later. */
    ESP_LOGI(TAG, "CAN driver initialized: TX=GPIO%d RX=GPIO%d bitrate=500kbps mode=NORMAL filter=ACCEPT_ALL",
             (int)CAN_TX_GPIO_NUM, (int)CAN_RX_GPIO_NUM);
    return ESP_OK;
}

esp_err_t can_driver_deinit(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!driver_initialized) {
        ESP_LOGW(TAG, "CAN driver not initialized");
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    if (driver_started) {
        driver_started = false;
        twai_stop();
    }

    esp_err_t err = twai_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to uninstall TWAI driver: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    driver_initialized = false;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "CAN driver deinitialized");
    return ESP_OK;
}

esp_err_t can_driver_start(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!driver_initialized) {
        ESP_LOGE(TAG, "CAN driver not initialized");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (driver_started) {
        ESP_LOGW(TAG, "CAN driver already started");
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    esp_err_t err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TWAI driver: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    driver_started = true;
    xSemaphoreGive(s_mutex);
    /* Query TWAI state right after start so the boot log reports
     * RUNNING vs BUS_OFF vs STOPPED — a bench dongle with miswired
     * pins still reaches "started" but cannot reach RUNNING. */
    twai_status_info_t status;
    if (twai_get_status_info(&status) == ESP_OK) {
        const char *state_name =
            (status.state == TWAI_STATE_RUNNING) ? "RUNNING (BUS_ON)" :
            (status.state == TWAI_STATE_BUS_OFF) ? "BUS_OFF" :
            (status.state == TWAI_STATE_RECOVERING) ? "RECOVERING" :
            (status.state == TWAI_STATE_STOPPED) ? "STOPPED" : "?";
        ESP_LOGI(TAG, "CAN driver started: TWAI state=%s (%lu)",
                 state_name, (unsigned long)status.state);
    } else {
        ESP_LOGI(TAG, "CAN driver started (twai_get_status_info unavailable)");
    }
    return ESP_OK;
}

esp_err_t can_driver_stop(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!driver_initialized) {
        ESP_LOGE(TAG, "CAN driver not initialized");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!driver_started) {
        ESP_LOGW(TAG, "CAN driver not started");
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    esp_err_t err = twai_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop TWAI driver: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    driver_started = false;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "CAN driver stopped");
    return ESP_OK;
}

esp_err_t can_driver_send(uint32_t id, const uint8_t *data, uint8_t len) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!driver_initialized || !driver_started) {
        ESP_LOGE(TAG, "CAN driver not ready");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_mutex);

    if (len > 8) {
        ESP_LOGE(TAG, "Invalid CAN message length: %d", len);
        return ESP_ERR_INVALID_ARG;
    }

    twai_message_t message = {
        .identifier = id,
        .data_length_code = len,
        .flags = TWAI_MSG_FLAG_NONE,
    };

    if (data != NULL && len > 0) {
        memcpy(message.data, data, len);
    }

    esp_err_t err = twai_transmit(&message, pdMS_TO_TICKS(500));
    if (err != ESP_OK) {
        twai_status_info_t status;
        if (twai_get_status_info(&status) == ESP_OK) {
            ESP_LOGE(TAG, "CAN TX fail: %s | state=%lu tx_err=%lu rx_err=%lu tx_fail=%lu arb_lost=%lu bus_err=%lu",
                     esp_err_to_name(err),
                     (unsigned long)status.state,
                     (unsigned long)status.tx_error_counter,
                     (unsigned long)status.rx_error_counter,
                     (unsigned long)status.tx_failed_count,
                     (unsigned long)status.arb_lost_count,
                     (unsigned long)status.bus_error_count);
        } else {
            ESP_LOGE(TAG, "Failed to transmit CAN message: %s", esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGD(TAG, "Transmitted CAN ID: 0x%03lX, len: %d", (unsigned long)id, len);
    return ESP_OK;
}

esp_err_t can_driver_receive(can_message_t *msg, uint32_t timeout_ms) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!driver_initialized || !driver_started) {
        ESP_LOGE(TAG, "CAN driver not ready");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_mutex);

    if (msg == NULL) {
        ESP_LOGE(TAG, "Invalid message pointer");
        return ESP_ERR_INVALID_ARG;
    }

    twai_message_t rx_message;
    esp_err_t err = twai_receive(&rx_message, pdMS_TO_TICKS(timeout_ms));
    
    if (err == ESP_ERR_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to receive CAN message: %s", esp_err_to_name(err));
        return err;
    }

    msg->id = rx_message.identifier;
    msg->len = rx_message.data_length_code;
    memcpy(msg->data, rx_message.data, rx_message.data_length_code);

    ESP_LOGD(TAG, "Received CAN ID: 0x%03lX, len: %d", msg->id, msg->len);
    return ESP_OK;
}

bool can_driver_is_initialized(void) {
    return driver_initialized;
}

