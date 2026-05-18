#include "ecu_commands.h"
#include "state_machine/connection_manager.h"
#include "nvs/nvs_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ECU_CMD";

esp_err_t cmd_pair_ecu(int fd, const char *params, char *response, size_t response_size) {
    if (!connection_manager_is_connected()) {
        snprintf(response, response_size, "Not connected to ECU");
        return ESP_ERR_INVALID_STATE;
    }

    if (connection_manager_is_paired()) {
        snprintf(response, response_size, "ECU already paired");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = connection_manager_pair_vehicle();
    if (err == ESP_OK) {
        snprintf(response, response_size, "ECU paired successfully");
    } else {
        snprintf(response, response_size, "Failed to pair ECU");
    }

    return err;
}

esp_err_t cmd_remove_pairing(int fd, const char *params, char *response, size_t response_size) {
    if (!connection_manager_is_paired()) {
        snprintf(response, response_size, "No paired ECU");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_manager_clear_ecu_info();
    if (err == ESP_OK) {
        snprintf(response, response_size, "Pairing removed successfully");
        connection_manager_disconnect();
    } else {
        snprintf(response, response_size, "Failed to remove pairing");
    }

    return err;
}

