#include "logger_manager.h"
#include "logger_config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "LOGGER_MGR";

static logger_config_t logger_config;
static float current_values[LOGGER_MAX_VALUES];
static logger_data_callback_t data_callback = NULL;
static bool has_received_data = false;

// For preserving values across reconfiguration
static logger_variable_t preserved_variables[LOGGER_MAX_VALUES];
static float preserved_values[LOGGER_MAX_VALUES];
static uint8_t preserved_count = 0;

static uint32_t buffer_address = 0;
static uint16_t buffer_size = 0;

/* P-55: most-recent ECU poll response bytes, retained for the
 * get_logger_data_raw WS command. Written by logger_manager_handle_poll_response
 * BEFORE the parse step so off-vehicle A2L cross-checking can validate
 * what the ECU actually sent vs. what the scale/offset formulas produced.
 * Length 0 means no poll response yet. */
static uint8_t  last_raw_response[LOGGER_MGR_RAW_RESPONSE_MAX];
static uint16_t last_raw_response_len = 0;

esp_err_t logger_manager_init(uint32_t buf_addr, uint16_t buf_size) {
    buffer_address = buf_addr;
    buffer_size = buf_size;
    
    logger_config_init(&logger_config);
    memset(current_values, 0, sizeof(current_values));
    memset(preserved_values, 0, sizeof(preserved_values));
    preserved_count = 0;
    has_received_data = false;
    
    ESP_LOGI(TAG, "Logger manager initialized (buffer: 0x%08lX, size: %d)", 
             buffer_address, buffer_size);
    return ESP_OK;
}

bool logger_manager_add_variable(uint32_t address,
                                 uint8_t size,
                                 float scale,
                                 float offset,
                                 bool is_signed,
                                 bool is_big_endian,
                                 const char *name) {
    bool result = logger_config_add_variable(&logger_config, address, size,
                                             scale, offset,
                                             is_signed, is_big_endian, name);
    
    if (result) {
        // Check if this variable existed in the preserved set
        if (preserved_count > 0 && name != NULL) {
            for (uint8_t i = 0; i < preserved_count; i++) {
                if (strcmp(preserved_variables[i].name, name) == 0) {
                    // Found matching variable - restore its old value
                    uint8_t new_index = logger_config.variable_count - 1;
                    current_values[new_index] = preserved_values[i];
                    ESP_LOGD(TAG, "Restored value for '%s': %.2f", name, preserved_values[i]);
                    break;
                }
            }
        }
        
        if (buffer_address != 0) {
            logger_config_build_configuration(&logger_config, buffer_address, buffer_size);
        }
        
        // If this is the last variable being added after a reconfiguration,
        // clear the preserved data
        // (We can't know when we're "done" adding, so we keep preserved data
        // until the next clear or until first new poll)
    }
    
    return result;
}

void logger_manager_clear_variables(void) {
    // Preserve old configuration and values if we have received data
    if (has_received_data && logger_config.variable_count > 0) {
        preserved_count = logger_config.variable_count;
        memcpy(preserved_variables, logger_config.variables, sizeof(preserved_variables));
        memcpy(preserved_values, current_values, sizeof(preserved_values));
        ESP_LOGI(TAG, "Preserving %d variables during reconfiguration", preserved_count);
    } else {
        preserved_count = 0;
    }
    
    // Clear the configuration
    logger_config_clear_variables(&logger_config);
    
    // Zero out current values (will be restored for matching variables)
    memset(current_values, 0, sizeof(current_values));
    
    // Note: has_received_data stays true - we still have valid data for preserved variables
}

bool logger_manager_is_configured(void) {
    return logger_config.is_configured;
}

bool logger_manager_needs_reconfigure(void) {
    return logger_config.needs_reconfigure && logger_config.variable_count > 0;
}

void logger_manager_set_data_callback(logger_data_callback_t callback) {
    data_callback = callback;
}

uint8_t logger_manager_get_variable_count(void) {
    return logger_config_get_variable_count(&logger_config);
}

const char* logger_manager_get_variable_name(uint8_t index) {
    if (index >= logger_config.variable_count) {
        return NULL;
    }
    return logger_config.variables[index].name;
}

float logger_manager_get_variable_value(uint8_t index) {
    if (index >= logger_config.variable_count) {
        return 0.0f;
    }
    return current_values[index];
}

logger_config_t* logger_manager_get_config(void) {
    return &logger_config;
}

float logger_manager_get_variable_value_by_name(const char *name) {
    for (uint8_t i = 0; i < logger_config.variable_count; i++) {
        if (strcmp(logger_config.variables[i].name, name) == 0) {
            return current_values[i];
        }
    }
    ESP_LOGW(TAG, "Variable not found: %s", name);
    return 0.0f;
}

bool logger_manager_send_poll_request(void) {
    if (!logger_config.is_configured) {
        ESP_LOGW(TAG, "Logger not configured, cannot poll");
        return false;
    }
    
    uint8_t poll_msg[6];
    uint16_t poll_len;
    
    if (!logger_config_build_poll_message(&logger_config, poll_msg, &poll_len)) {
        ESP_LOGE(TAG, "Failed to build poll message");
        return false;
    }
    
    return true;
}

bool logger_manager_handle_poll_response(const uint8_t *response, uint16_t response_len) {
    if (!logger_config.is_configured) {
        ESP_LOGW(TAG, "Logger not configured, ignoring response");
        return false;
    }

    /* P-55: snapshot the raw bytes BEFORE the parse step so
     * get_logger_data_raw can expose what the ECU actually returned
     * (independent of scale/offset formula correctness). */
    if (response != NULL && response_len > (uint16_t)0) {
        uint16_t copy_len = response_len;
        if (copy_len > (uint16_t)LOGGER_MGR_RAW_RESPONSE_MAX) {
            copy_len = (uint16_t)LOGGER_MGR_RAW_RESPONSE_MAX;
        }
        memcpy(last_raw_response, response, copy_len);
        last_raw_response_len = copy_len;
    }

    if (!logger_config_parse_poll_response(&logger_config, response, response_len, current_values)) {
        ESP_LOGE(TAG, "Failed to parse poll response");
        return false;
    }
    
    // Clear preserved data now that we have fresh poll data
    if (preserved_count > 0) {
        ESP_LOGD(TAG, "New poll received, clearing preserved data");
        preserved_count = 0;
    }
    
    has_received_data = true;  // Mark that we now have valid data
    
    if (data_callback != NULL) {
        data_callback(current_values, logger_config.variable_count);
    }
    
    return true;
}

bool logger_manager_has_data(void) {
    return has_received_data && logger_config.is_configured && logger_config.variable_count > 0;
}

uint16_t logger_manager_get_last_raw_response(uint8_t *out_buf, uint16_t out_cap) {
    if (out_buf == NULL || out_cap == (uint16_t)0) return 0;
    if (last_raw_response_len == (uint16_t)0) return 0;
    uint16_t copy_len = last_raw_response_len;
    if (copy_len > out_cap) copy_len = out_cap;
    memcpy(out_buf, last_raw_response, copy_len);
    return copy_len;
}

