#include "nvs_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NVS_MANAGER";
static const char *NVS_NAMESPACE = "futuner";

static bool nvs_initialized = false;

static const char *KEY_VIN = "vin";
static const char *KEY_SW_VERSION = "sw_ver";
static const char *KEY_HW_VERSION = "hw_ver";
static const char *KEY_BUILD_ID = "build_id";

esp_err_t nvs_manager_init(void) {
    if (nvs_initialized) {
        ESP_LOGW(TAG, "NVS manager already initialized");
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated and needs to be erased");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %s", esp_err_to_name(err));
            return err;
        }
        
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        return err;
    }

    nvs_initialized = true;
    ESP_LOGI(TAG, "NVS manager initialized successfully");
    return ESP_OK;
}

esp_err_t nvs_manager_deinit(void) {
    if (!nvs_initialized) {
        ESP_LOGW(TAG, "NVS manager not initialized");
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize NVS: %s", esp_err_to_name(err));
        return err;
    }

    nvs_initialized = false;
    ESP_LOGI(TAG, "NVS manager deinitialized");
    return ESP_OK;
}

esp_err_t nvs_manager_save_ecu_info(const ecu_info_t *info) {
    if (!nvs_initialized) {
        ESP_LOGE(TAG, "NVS manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (info == NULL) {
        ESP_LOGE(TAG, "Invalid ECU info pointer");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    do {
        if (strlen(info->vin) > 0) {
            err = nvs_set_str(nvs_handle, KEY_VIN, info->vin);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save VIN: %s", esp_err_to_name(err));
                break;
            }
        }

        if (strlen(info->software_version) > 0) {
            err = nvs_set_str(nvs_handle, KEY_SW_VERSION, info->software_version);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save software version: %s", esp_err_to_name(err));
                break;
            }
        }

        if (strlen(info->hardware_version) > 0) {
            err = nvs_set_str(nvs_handle, KEY_HW_VERSION, info->hardware_version);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save hardware version: %s", esp_err_to_name(err));
                break;
            }
        }

        if (strlen(info->build_id) > 0) {
            err = nvs_set_str(nvs_handle, KEY_BUILD_ID, info->build_id);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save build ID: %s", esp_err_to_name(err));
                break;
            }
        }

        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
            break;
        }

        ESP_LOGI(TAG, "ECU info saved successfully");
    } while (0);

    nvs_close(nvs_handle);
    return err;
}

esp_err_t nvs_manager_load_ecu_info(ecu_info_t *info) {
    if (!nvs_initialized) {
        ESP_LOGE(TAG, "NVS manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (info == NULL) {
        ESP_LOGE(TAG, "Invalid ECU info pointer");
        return ESP_ERR_INVALID_ARG;
    }

    ecu_info_init(info);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS (might be empty): %s", esp_err_to_name(err));
        return err;
    }

    size_t len;
    
    len = VIN_LENGTH + 1;
    err = nvs_get_str(nvs_handle, KEY_VIN, info->vin, &len);
    if (err == ESP_OK) {
        info->is_valid = true;
        ESP_LOGI(TAG, "Loaded VIN: %s", info->vin);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load VIN: %s", esp_err_to_name(err));
    }

    len = SOFTWARE_VERSION_LENGTH + 1;
    err = nvs_get_str(nvs_handle, KEY_SW_VERSION, info->software_version, &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded software version: %s", info->software_version);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load software version: %s", esp_err_to_name(err));
    }

    len = HARDWARE_VERSION_LENGTH + 1;
    err = nvs_get_str(nvs_handle, KEY_HW_VERSION, info->hardware_version, &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded hardware version: %s", info->hardware_version);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load hardware version: %s", esp_err_to_name(err));
    }

    len = BUILD_ID_LENGTH + 1;
    err = nvs_get_str(nvs_handle, KEY_BUILD_ID, info->build_id, &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded build ID: %s", info->build_id);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load build ID: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);

    if (info->is_valid) {
        ESP_LOGI(TAG, "ECU info loaded successfully");
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "No valid ECU info found in NVS");
        return ESP_ERR_NOT_FOUND;
    }
}

esp_err_t nvs_manager_erase_ecu_info(void) {
    if (!nvs_initialized) {
        ESP_LOGE(TAG, "NVS manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    nvs_erase_key(nvs_handle, KEY_VIN);
    nvs_erase_key(nvs_handle, KEY_SW_VERSION);
    nvs_erase_key(nvs_handle, KEY_HW_VERSION);
    nvs_erase_key(nvs_handle, KEY_BUILD_ID);

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ECU info erased successfully");
    } else {
        ESP_LOGE(TAG, "Failed to erase ECU info: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t nvs_manager_save_string(const char *key, const char *value) {
    if (!nvs_initialized) {
        ESP_LOGE(TAG, "NVS manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (key == NULL || value == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t nvs_manager_load_string(const char *key, char *value, size_t max_len) {
    if (!nvs_initialized) {
        ESP_LOGE(TAG, "NVS manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (key == NULL || value == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = max_len;
    err = nvs_get_str(nvs_handle, key, value, &len);
    nvs_close(nvs_handle);

    return err;
}

esp_err_t nvs_manager_save_uint32(const char *key, uint32_t value) {
    if (!nvs_initialized) {
        ESP_LOGE(TAG, "NVS manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (key == NULL) {
        ESP_LOGE(TAG, "Invalid key");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u32(nvs_handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t nvs_manager_load_uint32(const char *key, uint32_t *value) {
    if (!nvs_initialized) {
        ESP_LOGE(TAG, "NVS manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (key == NULL || value == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_get_u32(nvs_handle, key, value);
    nvs_close(nvs_handle);

    return err;
}

bool nvs_manager_is_initialized(void) {
    return nvs_initialized;
}

esp_err_t nvs_manager_clear_ecu_info(void) {
    if (!nvs_initialized) {
        ESP_LOGE(TAG, "NVS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for clear: %s", esp_err_to_name(err));
        return err;
    }

    nvs_erase_key(handle, KEY_VIN);
    nvs_erase_key(handle, KEY_SW_VERSION);
    nvs_erase_key(handle, KEY_HW_VERSION);
    nvs_erase_key(handle, KEY_BUILD_ID);

    err = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "ECU info cleared from NVS");
    return ESP_OK;
}

