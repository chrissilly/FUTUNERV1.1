#include "logger_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "LOGGER_CFG";

typedef struct {
    uint16_t upper_addr;
    uint8_t size;
    uint8_t var_indices[LOGGER_MAX_VARIABLES];
    uint8_t var_count;
} variable_group_t;

static int compare_variables_for_grouping(const void *a, const void *b, void *config_ptr) {
    logger_config_t *config = (logger_config_t *)config_ptr;
    uint8_t idx_a = *(uint8_t *)a;
    uint8_t idx_b = *(uint8_t *)b;
    
    logger_variable_t *var_a = &config->variables[idx_a];
    logger_variable_t *var_b = &config->variables[idx_b];
    
    if (var_a->size != var_b->size) {
        return var_b->size - var_a->size;
    }
    
    uint16_t upper_a = (var_a->address >> 16) & 0xFFFF;
    uint16_t upper_b = (var_b->address >> 16) & 0xFFFF;
    
    if (upper_a != upper_b) {
        return upper_a - upper_b;
    }
    
    return (var_a->address & 0xFFFF) - (var_b->address & 0xFFFF);
}

void logger_config_init(logger_config_t *config) {
    memset(config, 0, sizeof(logger_config_t));
}

bool logger_config_add_variable(logger_config_t *config, 
                                uint32_t address, 
                                uint8_t size,
                                float scale,
                                float offset,
                                const char *name) {
    if (config->variable_count >= LOGGER_MAX_VARIABLES) {
        ESP_LOGE(TAG, "Maximum variables reached");
        return false;
    }
    
    if (size != 1 && size != 2 && size != 4) {
        ESP_LOGE(TAG, "Invalid variable size: %d (must be 1, 2, or 4)", size);
        return false;
    }
    
    logger_variable_t *var = &config->variables[config->variable_count];
    var->address = address;
    var->size = size;
    var->scale = scale;
    var->offset = offset;
    strncpy(var->name, name, sizeof(var->name) - 1);
    var->name[sizeof(var->name) - 1] = '\0';
    
    config->variable_count++;
    config->needs_reconfigure = true;
    config->is_configured = false;
    
    ESP_LOGI(TAG, "Added variable: %s @ 0x%08lX (size=%d)", name, address, size);
    return true;
}

void logger_config_clear_variables(logger_config_t *config) {
    config->variable_count = 0;
    config->config_data_size = 0;
    config->is_configured = false;
    config->needs_reconfigure = true;
    ESP_LOGI(TAG, "Cleared all variables");
}

bool logger_config_build_configuration(logger_config_t *config, 
                                       uint32_t buffer_base,
                                       uint16_t buffer_size) {
    if (config->variable_count == 0) {
        ESP_LOGE(TAG, "No variables to configure");
        return false;
    }
    
    uint8_t sorted_indices[LOGGER_MAX_VARIABLES];
    for (uint8_t i = 0; i < config->variable_count; i++) {
        sorted_indices[i] = i;
    }
    
    qsort_r(sorted_indices, config->variable_count, sizeof(uint8_t),
            compare_variables_for_grouping, config);
    
    variable_group_t groups[LOGGER_MAX_VARIABLES];
    uint8_t group_count = 0;
    
    for (uint8_t i = 0; i < config->variable_count; i++) {
        uint8_t var_idx = sorted_indices[i];
        logger_variable_t *var = &config->variables[var_idx];
        uint16_t upper_addr = (var->address >> 16) & 0xFFFF;

        /* Group by upper address only — mixed sizes within a group are OK.
         * The ECU expects one group per upper address, not split by size. */
        bool found_group = false;
        for (uint8_t g = 0; g < group_count; g++) {
            if (groups[g].upper_addr == upper_addr) {
                groups[g].var_indices[groups[g].var_count++] = var_idx;
                found_group = true;
                break;
            }
        }
        
        if (!found_group) {
            groups[group_count].upper_addr = upper_addr;
            groups[group_count].size = var->size;
            groups[group_count].var_indices[0] = var_idx;
            groups[group_count].var_count = 1;
            group_count++;
        }
    }
    
    uint16_t offset = 0;
    for (uint8_t g = 0; g < group_count; g++) {
        /* H5 fix: bounds check before each write to config_data */
        uint16_t needed = 3 + (groups[g].var_count * 3);
        if (offset + needed + 1 > LOGGER_CONFIG_BUFFER_SIZE) {
            ESP_LOGE(TAG, "Config buffer overflow prevented at group %d (offset=%d, need=%d)",
                     g, offset, needed + 1);
            break;
        }

        config->config_data[offset++] = groups[g].var_count;
        config->config_data[offset++] = (groups[g].upper_addr >> 8) & 0xFF;
        config->config_data[offset++] = groups[g].upper_addr & 0xFF;

        for (uint8_t v = 0; v < groups[g].var_count; v++) {
            uint8_t var_idx = groups[g].var_indices[v];
            logger_variable_t *var = &config->variables[var_idx];
            uint16_t lower_addr = var->address & 0xFFFF;

            config->config_data[offset++] = var->size;
            config->config_data[offset++] = (lower_addr >> 8) & 0xFF;
            config->config_data[offset++] = lower_addr & 0xFF;
        }
    }

    if (offset < LOGGER_CONFIG_BUFFER_SIZE) {
        config->config_data[offset++] = 0x00;
    }
    config->config_data_size = offset;
    
    config->memory_pointer = buffer_base + buffer_size - config->config_data_size - 1;
    
    ESP_LOGI(TAG, "Built configuration: %d bytes, %d groups, pointer: 0x%08lX",
             config->config_data_size, group_count, config->memory_pointer);
    
    return true;
}

bool logger_config_build_config_message(logger_config_t *config,
                                        uint8_t *message,
                                        uint16_t *message_len,
                                        uint16_t max_len) {
    /* Header: [0x3E][0x32][addr4][len2] + config_data (already includes terminator 0x00) */
    uint16_t required_len = 1 + 1 + 4 + 2 + config->config_data_size;
    if (required_len > max_len) {
        ESP_LOGE(TAG, "Message buffer too small: need %d, have %d", required_len, max_len);
        return false;
    }
    
    uint16_t offset = 0;
    message[offset++] = 0x3E;
    message[offset++] = 0x32;

    /* Memory pointer as 4-byte big-endian address */
    message[offset++] = (config->memory_pointer >> 24) & 0xFF;
    message[offset++] = (config->memory_pointer >> 16) & 0xFF;
    message[offset++] = (config->memory_pointer >> 8) & 0xFF;
    message[offset++] = config->memory_pointer & 0xFF;

    /* Config data size as 2-byte big-endian */
    message[offset++] = (config->config_data_size >> 8) & 0xFF;
    message[offset++] = config->config_data_size & 0xFF;

    memcpy(&message[offset], config->config_data, config->config_data_size);
    offset += config->config_data_size;
    
    *message_len = offset;
    ESP_LOGI(TAG, "Built config message: %d bytes", *message_len);
    /* Hex dump for debugging */
    char hex[512];
    int hpos = 0;
    for (uint16_t i = 0; i < *message_len && hpos < (int)sizeof(hex) - 4; i++) {
        hpos += snprintf(hex + hpos, sizeof(hex) - hpos, "%02X ", message[i]);
    }
    ESP_LOGI(TAG, "Config hex: %s", hex);
    return true;
}

bool logger_config_build_poll_message(logger_config_t *config,
                                      uint8_t *message,
                                      uint16_t *message_len) {
    if (!config->is_configured) {
        ESP_LOGE(TAG, "Logger not configured, cannot poll");
        return false;
    }
    
    message[0] = 0x3E;
    message[1] = 0x33;
    message[2] = (config->memory_pointer >> 24) & 0xFF;
    message[3] = (config->memory_pointer >> 16) & 0xFF;
    message[4] = (config->memory_pointer >> 8) & 0xFF;
    message[5] = config->memory_pointer & 0xFF;
    
    *message_len = 6;
    return true;
}

bool logger_config_parse_poll_response(logger_config_t *config,
                                       const uint8_t *response,
                                       uint16_t response_len,
                                       float *values_out) {
    if (response_len < 1 || response[0] != 0x7E) {
        ESP_LOGE(TAG, "Invalid poll response header");
        return false;
    }
    
    uint16_t offset = 1;
    
    uint8_t sorted_indices[LOGGER_MAX_VARIABLES];
    for (uint8_t i = 0; i < config->variable_count; i++) {
        sorted_indices[i] = i;
    }
    qsort_r(sorted_indices, config->variable_count, sizeof(uint8_t),
            compare_variables_for_grouping, config);
    
    for (uint8_t i = 0; i < config->variable_count; i++) {
        uint8_t var_idx = sorted_indices[i];
        logger_variable_t *var = &config->variables[var_idx];
        
        if (offset + var->size > response_len) {
            ESP_LOGE(TAG, "Response too short for variable %s", var->name);
            return false;
        }
        
        int32_t raw_value = 0;
        if (var->size == 1) {
            raw_value = (int8_t)response[offset];
        } else if (var->size == 2) {
            raw_value = (int16_t)(response[offset] | (response[offset + 1] << 8));
        } else if (var->size == 4) {
            raw_value = (int32_t)(response[offset] | 
                                 (response[offset + 1] << 8) |
                                 (response[offset + 2] << 16) |
                                 (response[offset + 3] << 24));
        }
        
        values_out[var_idx] = (raw_value + var->offset) * var->scale;
        offset += var->size;
    }
    
    return true;
}

uint8_t logger_config_get_variable_count(const logger_config_t *config) {
    return config->variable_count;
}

uint16_t logger_config_get_total_response_size(const logger_config_t *config) {
    uint16_t size = 1;
    for (uint8_t i = 0; i < config->variable_count; i++) {
        size += config->variables[i].size;
    }
    return size;
}

