#ifndef CAN_MANAGER_H
#define CAN_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "can_config.h"

#define ISOTP_BUFSIZE ISOTP_BUFFER_SIZE

#define ECU_TX_ID ECU_PHYSICAL_TX_ID
#define ECU_RX_ID ECU_PHYSICAL_RX_ID

typedef enum {
    CAN_MGR_STATE_UNINITIALIZED,
    CAN_MGR_STATE_INITIALIZED,
    CAN_MGR_STATE_RUNNING,
    CAN_MGR_STATE_ERROR
} can_manager_state_t;

esp_err_t can_manager_init(void);
esp_err_t can_manager_deinit(void);
esp_err_t can_manager_start(void);
esp_err_t can_manager_stop(void);

void can_manager_poll(void);

esp_err_t can_manager_send_isotp(const uint8_t *payload, uint16_t size);
esp_err_t can_manager_send_isotp_with_id(uint32_t id, const uint8_t *payload, uint16_t size);

esp_err_t can_manager_receive_isotp(uint8_t *payload, uint16_t payload_size, uint16_t *out_size);

can_manager_state_t can_manager_get_state(void);
bool can_manager_is_busy(void);

#endif // CAN_MANAGER_H

