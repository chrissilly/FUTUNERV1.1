#include "ecu_info.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ECU_INFO";

void ecu_info_init(ecu_info_t *info) {
    if (info == NULL) {
        return;
    }

    memset(info, 0, sizeof(ecu_info_t));
    info->is_valid = false;
}

bool ecu_info_is_valid(const ecu_info_t *info) {
    if (info == NULL) {
        return false;
    }

    return info->is_valid && (strlen(info->vin) > 0);
}

void ecu_info_set_vin(ecu_info_t *info, const char *vin) {
    if (info == NULL || vin == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for VIN");
        return;
    }

    strncpy(info->vin, vin, VIN_LENGTH);
    info->vin[VIN_LENGTH] = '\0';
    
    if (strlen(vin) > 0) {
        info->is_valid = true;
    }
    
    ESP_LOGI(TAG, "VIN set to: %s", info->vin);
}

void ecu_info_set_software_version(ecu_info_t *info, const char *sw_version) {
    if (info == NULL || sw_version == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for software version");
        return;
    }

    strncpy(info->software_version, sw_version, SOFTWARE_VERSION_LENGTH);
    info->software_version[SOFTWARE_VERSION_LENGTH] = '\0';
    
    ESP_LOGI(TAG, "Software version set to: %s", info->software_version);
}

void ecu_info_set_hardware_version(ecu_info_t *info, const char *hw_version) {
    if (info == NULL || hw_version == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for hardware version");
        return;
    }

    strncpy(info->hardware_version, hw_version, HARDWARE_VERSION_LENGTH);
    info->hardware_version[HARDWARE_VERSION_LENGTH] = '\0';
    
    ESP_LOGI(TAG, "Hardware version set to: %s", info->hardware_version);
}

void ecu_info_set_build_id(ecu_info_t *info, const char *build_id) {
    if (info == NULL || build_id == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for build ID");
        return;
    }

    strncpy(info->build_id, build_id, BUILD_ID_LENGTH);
    info->build_id[BUILD_ID_LENGTH] = '\0';
    
    snprintf(info->boxcode, BOXCODE_LENGTH, "%s__%s",
             info->hardware_version, info->software_version);
    
    ESP_LOGI(TAG, "Build ID set to: %s", info->build_id);
    ESP_LOGI(TAG, "Boxcode generated: %s", info->boxcode);
}

const char* ecu_info_get_vin(const ecu_info_t *info) {
    if (info == NULL) {
        return "";
    }
    return info->vin;
}

const char* ecu_info_get_software_version(const ecu_info_t *info) {
    if (info == NULL) {
        return "";
    }
    return info->software_version;
}

const char* ecu_info_get_hardware_version(const ecu_info_t *info) {
    if (info == NULL) {
        return "";
    }
    return info->hardware_version;
}

const char* ecu_info_get_build_id(const ecu_info_t *info) {
    if (info == NULL) {
        return "";
    }
    return info->build_id;
}

void ecu_info_print(const ecu_info_t *info) {
    if (info == NULL) {
        ESP_LOGE(TAG, "Cannot print NULL ECU info");
        return;
    }

    ESP_LOGI(TAG, "===== ECU Information =====");
    ESP_LOGI(TAG, "VIN:              %s", info->vin[0] ? info->vin : "(not set)");
    ESP_LOGI(TAG, "Software Version: %s", info->software_version[0] ? info->software_version : "(not set)");
    ESP_LOGI(TAG, "Hardware Version: %s", info->hardware_version[0] ? info->hardware_version : "(not set)");
    ESP_LOGI(TAG, "Build ID:         %s", info->build_id[0] ? info->build_id : "(not set)");
    ESP_LOGI(TAG, "Boxcode:          %s", info->boxcode[0] ? info->boxcode : "(not set)");
    ESP_LOGI(TAG, "Valid:            %s", info->is_valid ? "Yes" : "No");
    ESP_LOGI(TAG, "===========================");
}

const char* ecu_info_get_boxcode(const ecu_info_t *info) {
    if (info == NULL) {
        return "";
    }
    return info->boxcode;
}

