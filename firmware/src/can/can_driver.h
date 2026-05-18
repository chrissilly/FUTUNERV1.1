#ifndef CAN_DRIVER_H
#define CAN_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/twai.h"
#include "can_config.h"

#define CAN_TX_GPIO_NUM CAN_TX_PIN
#define CAN_RX_GPIO_NUM CAN_RX_PIN
#define CAN_BAUDRATE TWAI_TIMING_CONFIG_500KBITS()

typedef struct {
    uint32_t id;
    uint8_t data[8];
    uint8_t len;
} can_message_t;

typedef void (*can_rx_callback_t)(const can_message_t *msg);

esp_err_t can_driver_init(void);
esp_err_t can_driver_deinit(void);
esp_err_t can_driver_start(void);
esp_err_t can_driver_stop(void);

esp_err_t can_driver_send(uint32_t id, const uint8_t *data, uint8_t len);
esp_err_t can_driver_receive(can_message_t *msg, uint32_t timeout_ms);

bool can_driver_is_initialized(void);

#endif // CAN_DRIVER_H

