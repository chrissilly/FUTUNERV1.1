#ifndef LOGGER_CONFIG_H
#define LOGGER_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define LOGGER_MAX_VARIABLES 32
#define LOGGER_CONFIG_BUFFER_SIZE 256

typedef struct {
    uint32_t address;
    uint8_t size;
    float scale;
    float offset;
    char name[32];
} logger_variable_t;

typedef struct {
    logger_variable_t variables[LOGGER_MAX_VARIABLES];
    uint8_t variable_count;
    
    uint8_t config_data[LOGGER_CONFIG_BUFFER_SIZE];
    uint16_t config_data_size;
    
    uint32_t memory_pointer;
    
    bool is_configured;
    bool needs_reconfigure;
} logger_config_t;

void logger_config_init(logger_config_t *config);

bool logger_config_add_variable(logger_config_t *config, 
                                uint32_t address, 
                                uint8_t size,
                                float scale,
                                float offset,
                                const char *name);

void logger_config_clear_variables(logger_config_t *config);

bool logger_config_build_configuration(logger_config_t *config, 
                                       uint32_t buffer_base,
                                       uint16_t buffer_size);

bool logger_config_build_config_message(logger_config_t *config,
                                        uint8_t *message,
                                        uint16_t *message_len,
                                        uint16_t max_len);

bool logger_config_build_poll_message(logger_config_t *config,
                                      uint8_t *message,
                                      uint16_t *message_len);

bool logger_config_parse_poll_response(logger_config_t *config,
                                       const uint8_t *response,
                                       uint16_t response_len,
                                       float *values_out);

uint8_t logger_config_get_variable_count(const logger_config_t *config);
uint16_t logger_config_get_total_response_size(const logger_config_t *config);

#endif // LOGGER_CONFIG_H

