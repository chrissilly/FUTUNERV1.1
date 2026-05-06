#ifndef LOGGER_VARIABLES_H
#define LOGGER_VARIABLES_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *name;
    const char *display_name;
    const char *unit;
    uint32_t address;
    uint8_t size;
    float scale;
    float offset;
    bool is_required;
    bool is_signed;
} logger_variable_def_t;

typedef struct {
    const char *boxcode;
    uint8_t is_big_endian;
    uint8_t write_mid_byte;
    uint32_t write_address_offset;
    uint32_t ethanol_memory_address;
    uint32_t speed_display_memory_address;
    const logger_variable_def_t *variables;
    uint8_t variable_count;
} boxcode_config_t;

bool logger_variables_set_boxcode(const char *boxcode);
const char* logger_variables_get_current_boxcode(void);
bool logger_variables_is_boxcode_supported(const char *boxcode);

const logger_variable_def_t* logger_variables_find_by_name(const char *name);
bool logger_variables_add_all_required(void);
bool logger_variables_add_by_name(const char *name);

uint8_t logger_variables_get_boxcode_count(void);
const char* logger_variables_get_boxcode_name(uint8_t index);

const boxcode_config_t* logger_variables_get_current_config(void);
uint8_t logger_variables_get_write_mid_byte(void);
uint32_t logger_variables_get_write_address_offset(void);
uint32_t logger_variables_get_ethanol_address(void);
uint32_t logger_variables_get_speed_display_address(void);

/**
 * Get the variable definition at a given index within the current boxcode catalog.
 * Returns NULL if index is out of range or no boxcode is selected.
 */
const logger_variable_def_t* logger_variables_get_by_index(uint8_t index);

/**
 * Get the total number of variables in the current boxcode catalog.
 */
uint8_t logger_variables_get_catalog_count(void);

#endif // LOGGER_VARIABLES_H

