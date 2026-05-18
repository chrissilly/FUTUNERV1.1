#ifndef ECU_WRITE_H
#define ECU_WRITE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// UDS write constants
#define UDS_WRITE_RAM_SUBFUNCTION 0x39
#define UDS_WRITE_FIXED_BYTE 0x01
#define UDS_WRITE_MAX_CHUNK_SIZE 64
#define UDS_WRITE_FOLLOWUP_REQUEST_SIZE 3

// Write operation callback
typedef void (*ecu_write_callback_t)(bool success, void *user_data);

/**
 * @brief Write data to ECU memory address
 * 
 * This function handles chunked writes automatically if data exceeds 64 bytes.
 * It also handles ECU "not ready" responses with automatic retry.
 * 
 * @param address Target ECU memory address
 * @param data Data buffer to write
 * @param size Size of data in bytes
 * @param mid_byte Mid byte from boxcode config (e.g., 0x80, 0x09)
 * @param address_offset Address offset from boxcode config (subtracted from target address)
 * @param callback Completion callback (optional)
 * @param user_data User data passed to callback (optional)
 * @return esp_err_t ESP_OK if write started successfully
 */
esp_err_t ecu_write_data(uint32_t address, 
                         const uint8_t *data, 
                         size_t size,
                         uint8_t mid_byte,
                         uint32_t address_offset,
                         ecu_write_callback_t callback,
                         void *user_data);

/**
 * @brief Check if an ECU write operation is currently in progress
 * 
 * @return bool True if write is in progress
 */
bool ecu_write_is_busy(void);

/**
 * @brief Cancel any ongoing write operation
 */
void ecu_write_cancel(void);

/**
 * @brief Poll for write operation updates (call from main loop)
 */
void ecu_write_poll(void);

/**
 * @brief Handle ISO-TP response for write operation
 * 
 * @param response Response data
 * @param response_len Response length
 */
void ecu_write_poll_response(const uint8_t *response, uint16_t response_len);

#endif // ECU_WRITE_H

