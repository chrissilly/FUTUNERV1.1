#ifndef LOGGER_MANAGER_H
#define LOGGER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "logger_config.h"

#define LOGGER_MAX_VALUES 32

/* P-55: max bytes retained from the most-recent ECU poll response, for
 * the get_logger_data_raw WS command. Sized to match the connection
 * manager's rx_buffer (256 B) so any response that fits the receive
 * path also fits here. Per CLAUDE.md Rule 3 — adjust here, not in
 * logger_manager.c. */
#define LOGGER_MGR_RAW_RESPONSE_MAX  256

typedef void (*logger_data_callback_t)(const float *values, uint8_t count);

esp_err_t logger_manager_init(uint32_t buffer_address, uint16_t buffer_size);

bool logger_manager_add_variable(uint32_t address, 
                                 uint8_t size,
                                 float scale,
                                 float offset,
                                 const char *name);

void logger_manager_clear_variables(void);

bool logger_manager_is_configured(void);
bool logger_manager_needs_reconfigure(void);
bool logger_manager_has_data(void);

void logger_manager_set_data_callback(logger_data_callback_t callback);

uint8_t logger_manager_get_variable_count(void);
const char* logger_manager_get_variable_name(uint8_t index);
float logger_manager_get_variable_value(uint8_t index);
float logger_manager_get_variable_value_by_name(const char *name);

logger_config_t* logger_manager_get_config(void);

bool logger_manager_send_poll_request(void);
bool logger_manager_handle_poll_response(const uint8_t *response, uint16_t response_len);

/* P-55: copy the bytes from the most-recent successful poll response
 * into the caller's buffer. Used by the get_logger_data_raw WS command
 * to expose pre-parse hex to off-vehicle A2L cross-checking. Returns
 * the number of bytes written (capped at out_cap and at the actual
 * captured length); 0 if no response has been received yet. */
uint16_t logger_manager_get_last_raw_response(uint8_t *out_buf, uint16_t out_cap);

#endif // LOGGER_MANAGER_H

