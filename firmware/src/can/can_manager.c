#include "can_manager.h"
#include "can_driver.h"
#include "isotp_shims.h"
#include "isotp.h"
#include "commands/can_sniffer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "CAN_MANAGER";

static can_manager_state_t manager_state = CAN_MGR_STATE_UNINITIALIZED;

static IsoTpLink isotp_link;
static uint8_t isotp_recv_buf[ISOTP_BUFSIZE];
static uint8_t isotp_send_buf[ISOTP_BUFSIZE];

esp_err_t can_manager_init(void) {
    if (manager_state != CAN_MGR_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "CAN manager already initialized");
        return ESP_OK;
    }

    esp_err_t err = can_driver_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize CAN driver");
        manager_state = CAN_MGR_STATE_ERROR;
        return err;
    }

    isotp_init_link(&isotp_link, ECU_TX_ID,
                    isotp_send_buf, sizeof(isotp_send_buf),
                    isotp_recv_buf, sizeof(isotp_recv_buf));

    manager_state = CAN_MGR_STATE_INITIALIZED;
    ESP_LOGI(TAG, "CAN manager initialized successfully");
    return ESP_OK;
}

esp_err_t can_manager_deinit(void) {
    if (manager_state == CAN_MGR_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "CAN manager not initialized");
        return ESP_OK;
    }

    if (manager_state == CAN_MGR_STATE_RUNNING) {
        can_manager_stop();
    }

    esp_err_t err = can_driver_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize CAN driver");
        return err;
    }

    manager_state = CAN_MGR_STATE_UNINITIALIZED;
    ESP_LOGI(TAG, "CAN manager deinitialized");
    return ESP_OK;
}

esp_err_t can_manager_start(void) {
    if (manager_state != CAN_MGR_STATE_INITIALIZED) {
        ESP_LOGE(TAG, "CAN manager not in initialized state");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = can_driver_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start CAN driver");
        manager_state = CAN_MGR_STATE_ERROR;
        return err;
    }

    manager_state = CAN_MGR_STATE_RUNNING;
    ESP_LOGI(TAG, "CAN manager started");
    return ESP_OK;
}

esp_err_t can_manager_stop(void) {
    if (manager_state != CAN_MGR_STATE_RUNNING) {
        ESP_LOGW(TAG, "CAN manager not running");
        return ESP_OK;
    }

    esp_err_t err = can_driver_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop CAN driver");
        return err;
    }

    manager_state = CAN_MGR_STATE_INITIALIZED;
    ESP_LOGI(TAG, "CAN manager stopped");
    return ESP_OK;
}

void can_manager_poll(void) {
    if (manager_state != CAN_MGR_STATE_RUNNING) {
        return;
    }

    can_message_t msg;
    esp_err_t err = can_driver_receive(&msg, 0);
    
    if (err == ESP_OK) {
        /* Feed sniffer before ISO-TP processing */
        can_sniffer_on_frame(msg.id, msg.data, msg.len);

        if (msg.id == ECU_RX_ID) {
            isotp_on_can_message(&isotp_link, msg.data, msg.len);
        }
    }

    isotp_poll(&isotp_link);
}

esp_err_t can_manager_send_isotp(const uint8_t *payload, uint16_t size) {
    if (manager_state != CAN_MGR_STATE_RUNNING) {
        ESP_LOGE(TAG, "CAN manager not running");
        return ESP_ERR_INVALID_STATE;
    }

    if (payload == NULL || size == 0) {
        ESP_LOGE(TAG, "Invalid payload");
        return ESP_ERR_INVALID_ARG;
    }

    int ret = isotp_send(&isotp_link, payload, size);
    if (ret != ISOTP_RET_OK) {
        ESP_LOGE(TAG, "Failed to send ISO-TP message: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "ISO-TP message queued for transmission, size: %d", size);
    return ESP_OK;
}

esp_err_t can_manager_send_isotp_with_id(uint32_t id, const uint8_t *payload, uint16_t size) {
    if (manager_state != CAN_MGR_STATE_RUNNING) {
        ESP_LOGE(TAG, "CAN manager not running");
        return ESP_ERR_INVALID_STATE;
    }

    if (payload == NULL || size == 0) {
        ESP_LOGE(TAG, "Invalid payload");
        return ESP_ERR_INVALID_ARG;
    }

    int ret = isotp_send_with_id(&isotp_link, id, payload, size);
    if (ret != ISOTP_RET_OK) {
        ESP_LOGE(TAG, "Failed to send ISO-TP message with ID 0x%03lX: %d", id, ret);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "ISO-TP message queued for transmission, ID: 0x%03lX, size: %d", id, size);
    return ESP_OK;
}

esp_err_t can_manager_receive_isotp(uint8_t *payload, uint16_t payload_size, uint16_t *out_size) {
    if (manager_state != CAN_MGR_STATE_RUNNING) {
        ESP_LOGE(TAG, "CAN manager not running");
        return ESP_ERR_INVALID_STATE;
    }

    if (payload == NULL || out_size == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    int ret = isotp_receive(&isotp_link, payload, payload_size, out_size);
    if (ret == ISOTP_RET_OK) {
        ESP_LOGD(TAG, "Received ISO-TP message, size: %d", *out_size);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

can_manager_state_t can_manager_get_state(void) {
    return manager_state;
}

bool can_manager_is_busy(void) {
    return false;
}

