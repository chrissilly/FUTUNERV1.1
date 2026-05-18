#ifndef LOGGER_MANAGER_H
#define LOGGER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "logger_config.h"

#define LOGGER_MAX_VALUES 32

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

#endif // LOGGER_MANAGER_H

