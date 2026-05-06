/*
 * FUTUNER CAN Sniffer — Standalone passive CAN capture firmware.
 * Build with: idf.py build -DSNIFF_MODE=1 -DCMAKE_C_FLAGS="-DCAN_TX_PIN=5 -DCAN_RX_PIN=4"
 *
 * This is a separate main that does NOT run the connection manager,
 * logger, or any UDS protocol. It only:
 *   1. Initializes CAN in LISTEN_ONLY mode
 *   2. Receives all CAN frames
 *   3. Logs them to serial
 *   4. Optionally broadcasts via WiFi WebSocket
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/twai.h"
#include "can/can_config.h"

static const char *TAG = "SNIFF";

void app_main(void) {
    ESP_LOGI(TAG, "=== FUTUNER CAN SNIFFER ===");
    ESP_LOGI(TAG, "TX pin: %d, RX pin: %d (LISTEN ONLY)", CAN_TX_PIN, CAN_RX_PIN);

    /* Init TWAI in listen-only mode */
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TWAI: %s", esp_err_to_name(err));
        return;
    }

    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TWAI: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "CAN bus listening at 500kbps...");

    uint32_t frame_count = 0;
    uint32_t last_status_ms = 0;

    while (1) {
        twai_message_t msg;
        err = twai_receive(&msg, pdMS_TO_TICKS(100));

        if (err == ESP_OK) {
            frame_count++;

            /* Build hex string */
            char hex[24];
            int pos = 0;
            for (int i = 0; i < msg.data_length_code && pos < 22; i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X", msg.data[i]);
            }
            hex[pos] = '\0';

            ESP_LOGI(TAG, "0x%03lX [%d] %s",
                     (unsigned long)msg.identifier,
                     msg.data_length_code,
                     hex);
        }

        /* Status every 5 seconds */
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - last_status_ms >= 5000) {
            twai_status_info_t status;
            twai_get_status_info(&status);
            ESP_LOGI(TAG, "--- %lu frames | state=%lu tx_err=%lu rx_err=%lu bus_err=%lu ---",
                     (unsigned long)frame_count,
                     (unsigned long)status.state,
                     (unsigned long)status.tx_error_counter,
                     (unsigned long)status.rx_error_counter,
                     (unsigned long)status.bus_error_count);
            last_status_ms = now;
        }
    }
}
