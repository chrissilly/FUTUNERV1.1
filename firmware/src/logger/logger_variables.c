#include "logger_variables.h"
#include "logger_manager.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "LOGGER_VARS";

// Boxcode: 4K0907557G__0003
//
// 0x5001xxxx variables temporarily disabled (2026-05-03): live capture showed
// every poll response coming back too short to parse, with the failure landing
// at the first 0x5001-region variable in sorted order (pvdg_w). Suspected: the
// patched ECU's logger handler bails when one of those addresses is read in
// the current vehicle state. Re-enable one at a time after diagnosing why the
// 0x5001 SRAM region returns a truncated response.
static const logger_variable_def_t VARIABLES_4K0907557G__0003[] = {
    {"nmot_w", "Engine Speed", "rpm", 0x60020618, 2, 0.25f, 0, true, false},
    {"InjSys_ratEthPrtnBascFu", "Ethanol Content", "%", 0x6001522A, 2, 0.00152587167162585f, 0, true, false},
    {"Com_stCrCtlPan", "Cruise Control Status", "-", 0x600206F8, 2, 1.0f, 0, true, false},
    {"rl_w", "Load (actual)", "%", 0x60015660, 2, 0.0234375066758221f, 0, false, false},
    {"tmot", "Coolant Temp", "C", 0x6001BF38, 1, 0.749803921568627f, -48, false, false},
    {"wdkba", "Throttle Position", "%", 0x6001B842, 1, 0.392156862745098f, 0, false, false},
    /* P-74 Batch A — non-0x5001 _msg-suffix variants resolved against
     * A2L MA22G01 (firmware/src/logger/catalogs/4K0907557G__0003__MA22G01.json).
     * All in known-good 0x6001/0x6002/0x7000 regions; expected to poll
     * cleanly. */
    {"tans",        "Intake Air Temp",      "C",   0x60021C63, 1, 0.75f,                   -48, false, false},
    {"ldtvm",       "Charge Air Pressure",  "hPa", 0x6002219C, 1, 0.390625f,                  0, false, false},
    {"fra_w",       "Lambda Bank 1",        "-",   0x700042E2, 2, 3.0517578125e-05f,           0, false, false},
    {"zwoutakt",    "Ignition Angle Actual","deg", 0x6001B6E6, 1, 0.75f,                       0, false, true},  /* SBYTE — signed */
    /* P-74 Batch B canary — single 0x5001 entry. P-55 commit f916b04
     * marked this region as truncating; this test confirms whether
     * that's still true on MA22G01. */
    {"pvdg_w",      "Boost Pressure",       "hPa", 0x5001BA86, 2, 0.078125f,                   0, false, false},
};

// Boxcode: 8W0907559H__0008
static const logger_variable_def_t VARIABLES_8W0907559H__0008[] = {
    {"nmot_w", "Engine Speed", "rpm", 0x51808794, 2, 0.25f, 0, true, false},
    {"InjSys_ratEthPrtnBascFu", "Ethanol Content", "%", 0x4000CD2A, 2, 0.00152587890625f, 0, true, false},
    {"rlp_w", "Load (predicted)", "%", 0x51808580, 2, 0.0234375f, 0, true, false},
    {"Com_stCrCtlPan", "Cruise Control Status", "-", 0x50806B5E, 2, 1.0f, 0, true, false},
    {"gang", "Gear", "-", 0x51809A3E, 1, 1.0f, 0, false, false},
    {"frm", "Short Term Fuel Trim B1", "%", 0x51808A76, 2, 0.000030518043793392844f, 0, false, false},
    {"frm2", "Short Term Fuel Trim B2", "%", 0x5180937E, 2, 0.000030518043793392844f, 0, false, false},
    {"lambts", "Lambda Sensor B1", "-", 0x50806DB8, 2, 0.00024414435034714275f, 0, false, false},
    {"lambts2", "Lambda Sensor B2", "-", 0x50806DB6, 2, 0.00024414435034714275f, 0, false, false},
};

// Boxcode: 4M0906014__0005
static const logger_variable_def_t VARIABLES_4M0906014__0005[] = {
    {"nmot_w", "Engine Speed", "rpm", 0x60020514, 2, 0.25f, 0, true, false},
    {"InjSys_ratEthPrtnBascFu", "Ethanol Content", "%", 0x60015162, 2, 0.00152587890625f, 0, true, false},
    {"rlp_w", "Load (predicted)", "%", 0x5001CB32, 2, 0.0234375f, 0, true, false},
    {"Com_stCrCtlPan", "Cruise Control Status", "-", 0x600205F4, 2, 1.0f, 0, true, false},
};

// Boxcode: 4M0906014B__0003
static const logger_variable_def_t VARIABLES_4M0906014B__0003[] = {
    {"nmot_w", "Engine Speed", "rpm", 0x60020690, 2, 0.25f, 0, true, false},
    {"InjSys_ratEthPrtnBascFu", "Ethanol Content", "%", 0x6001528E, 2, 0.00152587167162585f, 0, true, false},
    {"rlp_w", "Load (predicted)", "%", 0x5001CB08, 2, 0.0234375066758221f, 0, true, false},
    {"Com_stCrCtlPan", "Cruise Control Status", "-", 0x60020770, 2, 1.0f, 0, true, false},
    {"rl_w", "Load (actual)", "%", 0x600156C6, 2, 0.0234375066758221f, 0, false, false},
    {"tmot", "Coolant Temp", "C", 0x6001BFAF, 1, 0.749803921568627f, -48, false, false},
    {"wdkba", "Throttle Position", "%", 0x6001B8B9, 1, 0.392156862745098f, 0, false, false},
    {"pvdg_w", "Boost Pressure", "hPa", 0x5001BA82, 2, 0.0781250019073777f, 0, false, false},
    {"zwoutzyl_w", "Ignition Timing", "deg", 0x5001CD80, 2, 0.1f, 0, false, true},
    {"frm_w", "Short Term Fuel Trim", "-", 0x5001C9A2, 2, 3.05180437933928e-05f, 0, false, false},
};

static const boxcode_config_t BOXCODE_CONFIGS[] = {
    {
        "4K0907557G__0003",
        0,                              // Little endian
        0x80,                           // Write mid byte
        0,                              // Write address offset
        0x11E6AE,                       // Ethanol memory address
        0x9F93E,                        // Speed display memory address
        VARIABLES_4K0907557G__0003,
        sizeof(VARIABLES_4K0907557G__0003) / sizeof(logger_variable_def_t)
    },
    {
        "8W0907559H__0008",
        1,                              // Big endian
        0x09,                           // Write mid byte
        0x40000,                        // Write address offset
        0x6A8550,                       // Ethanol memory address
        0x651F30,                       // Speed display memory address
        VARIABLES_8W0907559H__0008,
        sizeof(VARIABLES_8W0907559H__0008) / sizeof(logger_variable_def_t)
    },
    {
        "4M0906014__0005",
        0,                              // Little endian
        0x80,                           // Write mid byte
        0,                              // Write address offset
        0x11E46A,                       // Ethanol memory address
        0x9F90E,                        // Speed display memory address
        VARIABLES_4M0906014__0005,
        sizeof(VARIABLES_4M0906014__0005) / sizeof(logger_variable_def_t)
    },
    {
        "4M0906014B__0003",
        0,                              // Little endian
        0x80,                           // Write mid byte
        0,                              // Write address offset
        0x11E6C6,                       // Ethanol memory address
        0x91A70,                        // Speed display memory address
        VARIABLES_4M0906014B__0003,
        sizeof(VARIABLES_4M0906014B__0003) / sizeof(logger_variable_def_t)
    },
};

static const uint8_t BOXCODE_COUNT = sizeof(BOXCODE_CONFIGS) / sizeof(boxcode_config_t);
static const boxcode_config_t *current_boxcode = NULL;

bool logger_variables_set_boxcode(const char *boxcode) {
    for (uint8_t i = 0; i < BOXCODE_COUNT; i++) {
        if (strcmp(BOXCODE_CONFIGS[i].boxcode, boxcode) == 0) {
            current_boxcode = &BOXCODE_CONFIGS[i];
            ESP_LOGI(TAG, "Selected boxcode: %s (%d variables)",
                     boxcode, current_boxcode->variable_count);
            return true;
        }
    }
    
    ESP_LOGE(TAG, "Unsupported boxcode: %s", boxcode);
    return false;
}

const char* logger_variables_get_current_boxcode(void) {
    return current_boxcode ? current_boxcode->boxcode : NULL;
}

bool logger_variables_is_boxcode_supported(const char *boxcode) {
    for (uint8_t i = 0; i < BOXCODE_COUNT; i++) {
        if (strcmp(BOXCODE_CONFIGS[i].boxcode, boxcode) == 0) {
            return true;
        }
    }
    return false;
}

const logger_variable_def_t* logger_variables_find_by_name(const char *name) {
    if (current_boxcode == NULL) {
        ESP_LOGW(TAG, "No boxcode selected");
        return NULL;
    }
    
    for (uint8_t i = 0; i < current_boxcode->variable_count; i++) {
        if (strcmp(current_boxcode->variables[i].name, name) == 0) {
            return &current_boxcode->variables[i];
        }
    }
    return NULL;
}

bool logger_variables_add_all_required(void) {
    if (current_boxcode == NULL) {
        ESP_LOGE(TAG, "No boxcode selected");
        return false;
    }
    
    ESP_LOGI(TAG, "Adding all required variables for boxcode: %s", current_boxcode->boxcode);
    uint8_t added = 0;
    
    for (uint8_t i = 0; i < current_boxcode->variable_count; i++) {
        if (current_boxcode->variables[i].is_required) {
            if (logger_manager_add_variable(
                current_boxcode->variables[i].address,
                current_boxcode->variables[i].size,
                current_boxcode->variables[i].scale,
                current_boxcode->variables[i].offset,
                current_boxcode->variables[i].is_signed,
                current_boxcode->is_big_endian != 0,
                current_boxcode->variables[i].name)) {
                added++;
                ESP_LOGI(TAG, "Added required variable: %s @ 0x%08lX",
                         current_boxcode->variables[i].name,
                         current_boxcode->variables[i].address);
            } else {
                ESP_LOGE(TAG, "Failed to add required variable: %s",
                         current_boxcode->variables[i].name);
                return false;
            }
        }
    }
    
    ESP_LOGI(TAG, "Added %d required variables", added);
    return added > 0;
}

bool logger_variables_add_by_name(const char *name) {
    const logger_variable_def_t *var = logger_variables_find_by_name(name);
    if (var == NULL) {
        ESP_LOGE(TAG, "Variable not found: %s", name);
        return false;
    }
    
    bool result = logger_manager_add_variable(
        var->address,
        var->size,
        var->scale,
        var->offset,
        var->is_signed,
        current_boxcode->is_big_endian != 0,
        var->name
    );
    
    if (result) {
        ESP_LOGI(TAG, "Added variable: %s @ 0x%08lX", name, var->address);
    } else {
        ESP_LOGE(TAG, "Failed to add variable: %s", name);
    }
    
    return result;
}

uint8_t logger_variables_get_boxcode_count(void) {
    return BOXCODE_COUNT;
}

const char* logger_variables_get_boxcode_name(uint8_t index) {
    if (index >= BOXCODE_COUNT) {
        return NULL;
    }
    return BOXCODE_CONFIGS[index].boxcode;
}

const boxcode_config_t* logger_variables_get_current_config(void) {
    return current_boxcode;
}

uint8_t logger_variables_get_write_mid_byte(void) {
    return current_boxcode ? current_boxcode->write_mid_byte : 0x80;
}

uint32_t logger_variables_get_write_address_offset(void) {
    return current_boxcode ? current_boxcode->write_address_offset : 0;
}

uint32_t logger_variables_get_ethanol_address(void) {
    return current_boxcode ? current_boxcode->ethanol_memory_address : 0;
}

uint32_t logger_variables_get_speed_display_address(void) {
    return current_boxcode ? current_boxcode->speed_display_memory_address : 0;
}

const logger_variable_def_t* logger_variables_get_by_index(uint8_t index) {
    if (!current_boxcode || index >= current_boxcode->variable_count) {
        return NULL;
    }
    return &current_boxcode->variables[index];
}

uint8_t logger_variables_get_catalog_count(void) {
    return current_boxcode ? current_boxcode->variable_count : 0;
}
