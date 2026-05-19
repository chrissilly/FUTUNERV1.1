#include "connection_manager.h"
#include "connection_config.h"
#include "uds_protocol.h"
#include "can/can_manager.h"
#include "nvs/nvs_manager.h"
#include "nvs/ecu_info.h"
#include "logger/logger_manager.h"
#include "logger/logger_config.h"
#include "logger/logger_variables.h"
#include "logger/logger_profile.h"
#include "error/error_tracker.h"
#include "isotp_coordinator/isotp_coordinator.h"
#include "ecu_write/ecu_write.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "CONN_MGR";

static connection_state_t current_state = CONN_STATE_DISCONNECTED;
static connection_status_t current_status = CONN_STATUS_DISCONNECTED;
static uint32_t state_entry_time = 0;
static uint32_t last_keepalive_time = 0;
static bool is_patched = false;
static bool is_paired = false;
/* Logger off by default — avoids contention with live tuning, flashing, DTC reads.
 * Set true via connection_manager_logger_start(). */
static bool logger_polling_enabled = false;

static ecu_info_t current_ecu_info;
static ecu_info_t stored_ecu_info;

static uint32_t log_buffer_address = 0;
static uint8_t patch_version = 0;
static uint16_t patch_buffer_size = 0;

static uint8_t rx_buffer[256];
static uint16_t rx_size;
static uds_response_t current_response;

static uint32_t last_logger_poll_time = 0;

// ===== Helper Functions =====

static inline uint32_t get_time_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static inline bool check_timeout(uint32_t timeout_ms) {
    return (get_time_ms() - state_entry_time) >= timeout_ms;
}

static void change_state(connection_state_t new_state) {
    if (new_state != current_state) {
        ESP_LOGI(TAG, "State: %s -> %s", 
                 connection_manager_get_state_name(current_state),
                 connection_manager_get_state_name(new_state));
        current_state = new_state;
        state_entry_time = get_time_ms();
        
        switch (new_state) {
            case CONN_STATE_DISCONNECTED:
                current_status = CONN_STATUS_DISCONNECTED;
                break;
            case CONN_STATE_DISCOVERING:
                current_status = CONN_STATUS_CONNECTING;
                break;
            case CONN_STATE_CONNECTED:
                last_keepalive_time = get_time_ms();
                current_status = is_patched ? CONN_STATUS_CONNECTED_PATCHED : CONN_STATUS_CONNECTED_UNPATCHED;
                ESP_LOGI(TAG, "Connected to %s ECU", is_patched ? "patched" : "unpatched");
                break;
            case CONN_STATE_ERROR:
                current_status = CONN_STATUS_ERROR;
                break;
            default:
                break;
        }
    }
}

static esp_err_t send_uds_request(const uds_request_t *req) {
    if (!req) return ESP_ERR_INVALID_ARG;
    
    uint8_t buffer[256];
    buffer[0] = req->service;
    if (req->length > 0) {
        memcpy(&buffer[1], req->data, req->length);
    }
    
    return can_manager_send_isotp(buffer, 1 + req->length);
}

static bool try_receive_uds_response(uds_response_t *resp) {
    esp_err_t err = can_manager_receive_isotp(rx_buffer, sizeof(rx_buffer), &rx_size);
    if (err == ESP_OK) {
        return uds_parse_response(rx_buffer, rx_size, resp);
    }
    return false;
}


// ===== State Handlers =====

static void handle_discovering(void) {
    ESP_LOGI(TAG, "Discovering vehicle...");
    
    uds_request_t req;
    uds_build_tester_present(&req, UDS_SUBFUNCTION_TESTER_PRESENT_NORMAL, 0);
    send_uds_request(&req);
    
    change_state(CONN_STATE_WAIT_DISCOVER_RESPONSE);
}

static void handle_wait_discover_response(void) {
    if (try_receive_uds_response(&current_response)) {
        if (uds_is_positive_response(&current_response, UDS_SERVICE_TESTER_PRESENT)) {
            ESP_LOGI(TAG, "Vehicle discovered");
            change_state(CONN_STATE_REQUEST_VIN);
        } else {
            ESP_LOGW(TAG, "Invalid discover response");
            change_state(CONN_STATE_DISCONNECTED);
        }
    } else if (check_timeout(CONNECTION_DISCOVERY_RETRY_DELAY_MS)) {
        ESP_LOGW(TAG, "Discovery timeout, retrying...");
        change_state(CONN_STATE_DISCOVERING);
    }
}

static void handle_request_vin(void) {
    ESP_LOGI(TAG, "Requesting VIN");
    
    uds_request_t req;
    uds_build_read_data_by_id(&req, UDS_VIN_RESOURCE_ID);
    send_uds_request(&req);
    
    change_state(CONN_STATE_WAIT_VIN_RESPONSE);
}

static void handle_wait_vin_response(void) {
    if (try_receive_uds_response(&current_response)) {
        uint8_t vin_data[32];
        uint16_t vin_len;
        
        if (uds_extract_data(&current_response, UDS_VIN_RESOURCE_ID, vin_data, &vin_len, sizeof(vin_data))) {
            if (vin_len == 17) {
                memcpy(current_ecu_info.vin, vin_data, 17);
                current_ecu_info.vin[17] = '\0';
                ESP_LOGI(TAG, "VIN received: %s", current_ecu_info.vin);
                change_state(CONN_STATE_CHECK_PAIRING);
            } else {
                ESP_LOGE(TAG, "Invalid VIN length: %d", vin_len);
                change_state(CONN_STATE_ERROR);
            }
        } else if (uds_is_negative_response(&current_response)) {
            ESP_LOGE(TAG, "Negative response for VIN: %s", uds_get_nrc_name(current_response.nrc));
            change_state(CONN_STATE_ERROR);
        } else {
            ESP_LOGE(TAG, "Unexpected VIN response");
            change_state(CONN_STATE_ERROR);
        }
    } else if (check_timeout(CONNECTION_REQUEST_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "VIN request timeout");
        change_state(CONN_STATE_ERROR);
    }
}

static void handle_check_pairing(void) {
    ESP_LOGI(TAG, "Checking if paired with VIN: %s", current_ecu_info.vin);
    
    if (nvs_manager_load_ecu_info(&stored_ecu_info) == ESP_OK) {
        if (strcmp(stored_ecu_info.vin, current_ecu_info.vin) == 0) {
            ESP_LOGI(TAG, "Already paired with this vehicle");
            memcpy(&current_ecu_info, &stored_ecu_info, sizeof(ecu_info_t));
            is_paired = true;
            change_state(CONN_STATE_CHECK_PATCH_STATUS);
            return;
        }
    }
    
    ESP_LOGI(TAG, "Not paired with this vehicle, reading vehicle info");
    ESP_LOGW(TAG, "User must explicitly pair to save this vehicle");
    is_paired = false;
    change_state(CONN_STATE_REQUEST_SERIAL);
}

static bool handle_standard_did_response(uint16_t did, const char *name, char *dest, size_t dest_size, connection_state_t next_state) {
    if (!try_receive_uds_response(&current_response)) {
        if (check_timeout(CONNECTION_REQUEST_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "%s request timeout", name);
            change_state(CONN_STATE_ERROR);
        }
        return false;
    }
    
    uint8_t data[64];
    uint16_t data_len;
    
    if (uds_extract_data(&current_response, did, data, &data_len, sizeof(data))) {
        if (data_len > 0 && data_len < dest_size) {
            memcpy(dest, data, data_len);
            dest[data_len] = '\0';
            ESP_LOGI(TAG, "%s: %s", name, dest);
            change_state(next_state);
            return true;
        }
        ESP_LOGE(TAG, "Invalid %s length: %d", name, data_len);
    } else if (uds_is_negative_response(&current_response)) {
        ESP_LOGE(TAG, "Negative response for %s: %s", name, uds_get_nrc_name(current_response.nrc));
    } else {
        ESP_LOGE(TAG, "Unexpected %s response", name);
    }
    
    change_state(CONN_STATE_ERROR);
    return false;
}

static void handle_request_serial(void) {
    ESP_LOGI(TAG, "Requesting serial number");
    uds_request_t req;
    uds_build_read_data_by_id(&req, UDS_SERIAL_NUMBER_DID);
    send_uds_request(&req);
    change_state(CONN_STATE_WAIT_SERIAL_RESPONSE);
}

static void handle_wait_serial_response(void) {
    handle_standard_did_response(UDS_SERIAL_NUMBER_DID, "Serial number", 
                                  current_ecu_info.hardware_version, 
                                  sizeof(current_ecu_info.hardware_version),
                                  CONN_STATE_REQUEST_SOFTWARE_VERSION);
}

static void handle_request_software_version(void) {
    ESP_LOGI(TAG, "Requesting software version");
    uds_request_t req;
    uds_build_read_data_by_id(&req, UDS_SOFTWARE_VERSION_DID);
    send_uds_request(&req);
    change_state(CONN_STATE_WAIT_SOFTWARE_VERSION_RESPONSE);
}

static void handle_wait_software_version_response(void) {
    handle_standard_did_response(UDS_SOFTWARE_VERSION_DID, "Software version",
                                  current_ecu_info.software_version,
                                  sizeof(current_ecu_info.software_version),
                                  CONN_STATE_REQUEST_BUILD_ID);
}

static void handle_request_build_id(void) {
    ESP_LOGI(TAG, "Requesting build ID");
    uds_request_t req;
    uds_build_read_data_by_id(&req, UDS_BUILD_ID_DID);
    send_uds_request(&req);
    change_state(CONN_STATE_WAIT_BUILD_ID_RESPONSE);
}

static void handle_wait_build_id_response(void) {
    if (handle_standard_did_response(UDS_BUILD_ID_DID, "Build ID",
                                      current_ecu_info.build_id,
                                      sizeof(current_ecu_info.build_id),
                                      CONN_STATE_REQUEST_EXTENDED_SESSION)) {
        current_ecu_info.is_valid = true;
        /* Generate boxcode from hardware_version (serial) + software_version.
         * Trim trailing whitespace from both fields — ECU may pad with spaces. */
        char hw[32], sw[16];
        strncpy(hw, current_ecu_info.hardware_version, sizeof(hw) - 1);
        hw[sizeof(hw) - 1] = '\0';
        strncpy(sw, current_ecu_info.software_version, sizeof(sw) - 1);
        sw[sizeof(sw) - 1] = '\0';
        for (int i = strlen(hw) - 1; i >= 0 && hw[i] == ' '; i--) hw[i] = '\0';
        for (int i = strlen(sw) - 1; i >= 0 && sw[i] == ' '; i--) sw[i] = '\0';
        snprintf(current_ecu_info.boxcode, sizeof(current_ecu_info.boxcode),
                 "%s__%s", hw, sw);
        ESP_LOGI(TAG, "Vehicle info read successfully (not paired yet)");
        ESP_LOGI(TAG, "Boxcode: %s", current_ecu_info.boxcode);
    }
}

/* ===== Diagnostic Session & Security Access ===== */

static uint32_t security_seed = 0;

static void handle_request_extended_session(void) {
    ESP_LOGI(TAG, "Requesting extended diagnostic session");
    uds_request_t req;
    uds_build_diagnostic_session(&req, UDS_SESSION_EXTENDED);
    send_uds_request(&req);
    change_state(CONN_STATE_WAIT_EXTENDED_SESSION_RESPONSE);
}

static void handle_wait_extended_session_response(void) {
    if (try_receive_uds_response(&current_response)) {
        if (uds_is_positive_response(&current_response, UDS_SERVICE_DIAGNOSTIC_SESSION_CONTROL)) {
            ESP_LOGI(TAG, "Extended diagnostic session active");
            change_state(CONN_STATE_CHECK_PATCH_STATUS);
        } else if (uds_is_negative_response(&current_response)) {
            ESP_LOGE(TAG, "Extended diagnostic session failed: %s", uds_get_nrc_name(current_response.nrc));
            /* Some ECUs work without extended session — try patch check anyway */
            ESP_LOGW(TAG, "Continuing without extended session");
            change_state(CONN_STATE_CHECK_PATCH_STATUS);
        }
    } else if (check_timeout(CONNECTION_REQUEST_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Extended session timeout");
        change_state(CONN_STATE_ERROR);
    }
}

static void handle_check_patch_status(void) {
    ESP_LOGI(TAG, "Checking ECU patch status");
    
    uds_request_t req;
    uds_build_tester_present(&req, UDS_SUBFUNCTION_LIVE_MODE_ENABLE, 0x01);
    send_uds_request(&req);
    
    change_state(CONN_STATE_WAIT_PATCH_RESPONSE);
}

static void handle_wait_patch_response(void) {
    if (try_receive_uds_response(&current_response)) {
        if (uds_is_positive_response(&current_response, UDS_SERVICE_TESTER_PRESENT)) {
            ESP_LOGI(TAG, "ECU is patched - LIVE mode enabled");
            is_patched = true;
            change_state(CONN_STATE_REQUEST_PATCH_INFO);
        } else if (uds_is_negative_response(&current_response)) {
            ESP_LOGW(TAG, "ECU is not patched (NRC: %s)", uds_get_nrc_name(current_response.nrc));
            is_patched = false;
            change_state(CONN_STATE_CONNECTED);
        } else {
            ESP_LOGE(TAG, "Unexpected patch response - connection failed");
            change_state(CONN_STATE_ERROR);
        }
    } else if (check_timeout(CONNECTION_REQUEST_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Patch check timeout - connection failed");
        change_state(CONN_STATE_ERROR);
    }
}

static void handle_request_patch_info(void) {
    ESP_LOGI(TAG, "Requesting patch information");
    
    uint8_t buffer[4];
    buffer[0] = UDS_SERVICE_TESTER_PRESENT;
    buffer[1] = UDS_SUBFUNCTION_PATCH_INFO;
    buffer[2] = UDS_PATCH_INFO_PARAM;

    can_manager_send_isotp(buffer, 3);
    
    change_state(CONN_STATE_WAIT_PATCH_INFO_RESPONSE);
}

static void handle_wait_patch_info_response(void) {
    if (try_receive_uds_response(&current_response)) {
        if (uds_is_positive_response(&current_response, UDS_SERVICE_TESTER_PRESENT)) {
            // Expected format: 0x7e, 0x01, 0x80, 0x03, 0xff, 0x00, 0x8a, 0x50, 0x01, 0xd2, 0x93
            // [service][version][subfunction][buffer_size_high][buffer_size_low][padding][padding][addr0][addr1][addr2][addr3]
            
            if (current_response.length < 10) {
                ESP_LOGE(TAG, "Invalid patch info response length: %d", current_response.length);
                change_state(CONN_STATE_ERROR);
                return;
            }
            
            // Check version (data[0] is version since we already have service byte stripped)
            patch_version = current_response.data[0];
            if (patch_version != UDS_PATCH_VERSION_V2) {
                ESP_LOGE(TAG, "Unsupported patch version: 0x%02X (expected V2: 0x%02X)", 
                         patch_version, UDS_PATCH_VERSION_V2);
                change_state(CONN_STATE_ERROR);
                return;
            }
            
            // Check buffer size (data[2] and data[3])
            patch_buffer_size = (current_response.data[2] << 8) | current_response.data[3];
            if (patch_buffer_size != UDS_PATCH_BUFFER_SIZE) {
                ESP_LOGE(TAG, "Invalid buffer size: %d (expected %d)", 
                         patch_buffer_size, UDS_PATCH_BUFFER_SIZE);
                change_state(CONN_STATE_ERROR);
                return;
            }
            
            // Extract log buffer address (4 bytes starting at data[6])
            log_buffer_address = (current_response.data[6] << 24) | 
                                 (current_response.data[7] << 16) | 
                                 (current_response.data[8] << 8) | 
                                 current_response.data[9];
            
            ESP_LOGI(TAG, "Patch info - Version: V%d, Buffer Size: %d, Log Buffer Address: 0x%08lX", 
                     (patch_version + 1), patch_buffer_size, log_buffer_address);
            
            logger_manager_init(log_buffer_address, patch_buffer_size);
            
            const char *boxcode = ecu_info_get_boxcode(&current_ecu_info);
            if (logger_variables_is_boxcode_supported(boxcode)) {
                ESP_LOGI(TAG, "Boxcode %s is supported", boxcode);
                logger_variables_set_boxcode(boxcode);
            } else {
                ESP_LOGW(TAG, "Boxcode %s is NOT supported, logger will not be available", boxcode);
            }
            
            change_state(CONN_STATE_CHECK_LOGGER_CONFIG);
        } else if (uds_is_negative_response(&current_response)) {
            ESP_LOGE(TAG, "Negative response for patch info: %s", 
                     uds_get_nrc_name(current_response.nrc));
            change_state(CONN_STATE_ERROR);
        } else {
            ESP_LOGE(TAG, "Unexpected patch info response");
            change_state(CONN_STATE_ERROR);
        }
    } else if (check_timeout(CONNECTION_REQUEST_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Patch info request timeout");
        change_state(CONN_STATE_ERROR);
    }
}

static void handle_check_logger_config(void) {
    if (!logger_manager_is_configured()) {
        const char *boxcode = logger_variables_get_current_boxcode();
        ESP_LOGI(TAG, "Logger not configured, applying profile for %s",
                 boxcode ? boxcode : "unknown");

        /* Use the profile system: loads saved profile or falls back to all vars */
        if (!logger_profile_apply(boxcode)) {
            ESP_LOGE(TAG, "Failed to apply logger profile");
            error_tracker_log(ERROR_CATEGORY_LOGGER, ERROR_SEVERITY_ERROR,
                            "Failed to apply logger profile");
            change_state(CONN_STATE_ERROR);
            return;
        }

        ESP_LOGI(TAG, "Logger profile applied (%d variables), configuring ECU",
                 logger_manager_get_variable_count());
        change_state(CONN_STATE_CONFIGURE_LOGGER);
    } else if (logger_manager_needs_reconfigure()) {
        ESP_LOGI(TAG, "Logger needs reconfiguration");
        change_state(CONN_STATE_CONFIGURE_LOGGER);
    } else {
        ESP_LOGI(TAG, "Logger already configured");
        change_state(CONN_STATE_CONNECTED);
    }
}

static void handle_configure_logger(void) {
    ESP_LOGI(TAG, "Configuring logger with %d variables", 
             logger_manager_get_variable_count());
    
    logger_config_t *config = logger_manager_get_config();
    uint8_t config_msg[512];
    uint16_t config_len;
    
    if (!logger_config_build_configuration(config, log_buffer_address, patch_buffer_size)) {
        ESP_LOGE(TAG, "Failed to build logger configuration");
        change_state(CONN_STATE_ERROR);
        return;
    }
    
    if (!logger_config_build_config_message(config, config_msg, &config_len, sizeof(config_msg))) {
        ESP_LOGE(TAG, "Failed to build logger config message");
        change_state(CONN_STATE_ERROR);
        return;
    }
    
    esp_err_t ret = can_manager_send_isotp(config_msg, config_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send logger config: %s", esp_err_to_name(ret));
        error_tracker_log(ERROR_CATEGORY_LOGGER, ERROR_SEVERITY_ERROR,
                        "Failed to send logger configuration");
        change_state(CONN_STATE_ERROR);
        return;
    }
    
    state_entry_time = get_time_ms();
    change_state(CONN_STATE_WAIT_LOGGER_CONFIG_RESPONSE);
}

static void handle_wait_logger_config_response(void) {
    if (try_receive_uds_response(&current_response)) {
        if (uds_is_positive_response(&current_response, UDS_SERVICE_TESTER_PRESENT)) {
            ESP_LOGI(TAG, "Logger configured successfully");
            logger_config_t *config = logger_manager_get_config();
            config->is_configured = true;
            config->needs_reconfigure = false;
            change_state(CONN_STATE_CONNECTED);
        } else if (uds_is_negative_response(&current_response)) {
            ESP_LOGE(TAG, "Logger configuration failed: %s", 
                     uds_get_nrc_name(current_response.nrc));
            change_state(CONN_STATE_ERROR);
        } else {
            ESP_LOGE(TAG, "Unexpected logger config response");
            change_state(CONN_STATE_ERROR);
        }
    } else if (check_timeout(CONNECTION_REQUEST_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Logger configuration timeout");
        change_state(CONN_STATE_ERROR);
    }
}

static void handle_polling_logger(void) {
    // Check if ISO-TP channel is free (no ECU write in progress)
    if (!isotp_coordinator_request(ISOTP_OWNER_LOGGER, 0)) {
        // Channel is busy (probably ECU write), skip this poll cycle
        ESP_LOGD(TAG, "ISO-TP channel busy, skipping logger poll");
        last_logger_poll_time = get_time_ms();  // Update time so we don't spam
        change_state(CONN_STATE_CONNECTED);
        return;
    }
    
    uint8_t poll_msg[6];
    uint16_t poll_len;
    
    logger_config_t *config = logger_manager_get_config();
    if (!logger_config_build_poll_message(config, poll_msg, &poll_len)) {
        ESP_LOGE(TAG, "Failed to build logger poll message");
        isotp_coordinator_release(ISOTP_OWNER_LOGGER);
        change_state(CONN_STATE_ERROR);
        return;
    }
    
    esp_err_t ret = can_manager_send_isotp(poll_msg, poll_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send logger poll: %s", esp_err_to_name(ret));
        error_tracker_log(ERROR_CATEGORY_LOGGER, ERROR_SEVERITY_ERROR,
                        "Failed to send logger poll request");
        isotp_coordinator_release(ISOTP_OWNER_LOGGER);
        change_state(CONN_STATE_ERROR);
        return;
    }
    
    last_logger_poll_time = get_time_ms();
    state_entry_time = last_logger_poll_time;
    change_state(CONN_STATE_WAIT_LOGGER_POLL_RESPONSE);
}

static void handle_wait_logger_poll_response(void) {
    esp_err_t ret = can_manager_receive_isotp(rx_buffer, sizeof(rx_buffer), &rx_size);
    
    if (ret == ESP_OK && rx_size > 0) {
        // Release logger ownership before processing response
        isotp_coordinator_release(ISOTP_OWNER_LOGGER);
        
        if (!logger_manager_handle_poll_response(rx_buffer, rx_size)) {
            ESP_LOGW(TAG, "Failed to parse logger response");
        }
        
        float nmot = logger_manager_get_variable_value_by_name("nmot_w");
        uint32_t next_poll_delay;
        
        if (nmot > 0.0f) {
            next_poll_delay = 0;
            ESP_LOGD(TAG, "Engine running (nmot=%.1f RPM), polling immediately", nmot);
        } else {
            next_poll_delay = 1000;
            ESP_LOGD(TAG, "Engine stopped, polling at 1Hz");
        }
        
        if (next_poll_delay == 0) {
            change_state(CONN_STATE_POLLING_LOGGER);
        } else {
            last_logger_poll_time = get_time_ms();
            change_state(CONN_STATE_CONNECTED);
        }
    } else if (check_timeout(CONNECTION_REQUEST_TIMEOUT_MS)) {
        // Release ownership on timeout
        isotp_coordinator_release(ISOTP_OWNER_LOGGER);
        ESP_LOGE(TAG, "Logger poll timeout, ECU disconnected");
        change_state(CONN_STATE_DISCONNECTED);
    }
}

static void handle_connected(void) {
    // Route ISO-TP responses to ECU write system when a write is active
    if (ecu_write_is_busy()) {
        esp_err_t ret = can_manager_receive_isotp(rx_buffer, sizeof(rx_buffer), &rx_size);
        if (ret == ESP_OK && rx_size > 0) {
            ecu_write_poll_response(rx_buffer, rx_size);
        }
        return;  // Skip logger/keepalive while write is in progress
    }

    // Check if logger configuration changed
    if (is_patched && logger_manager_needs_reconfigure()) {
        ESP_LOGI(TAG, "Logger configuration changed, reconfiguring");
        change_state(CONN_STATE_CHECK_LOGGER_CONFIG);
        return;
    }

    uint32_t now = get_time_ms();

    if (is_patched && logger_manager_is_configured() && logger_polling_enabled) {
        float nmot = logger_manager_get_variable_value_by_name("nmot_w");
        uint32_t poll_interval = (nmot > 0.0f) ? 0 : 1000;

        if ((now - last_logger_poll_time) >= poll_interval) {
            change_state(CONN_STATE_POLLING_LOGGER);
        }
    } else {
        if ((now - last_keepalive_time) >= KEEPALIVE_INTERVAL_MS) {
            ESP_LOGD(TAG, "Sending keep-alive");
            uds_request_t req;
            uds_build_tester_present(&req, UDS_SUBFUNCTION_TESTER_PRESENT_NORMAL, 0);
            send_uds_request(&req);
            last_keepalive_time = now;
            state_entry_time = now;
        }
        
        if (try_receive_uds_response(&current_response)) {
            if (uds_is_positive_response(&current_response, UDS_SERVICE_TESTER_PRESENT)) {
                ESP_LOGD(TAG, "Keep-alive acknowledged");
            } else {
                ESP_LOGE(TAG, "Unexpected keep-alive response, disconnecting");
                change_state(CONN_STATE_DISCONNECTED);
            }
        } else if (check_timeout(CONNECTION_REQUEST_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "Keep-alive timeout, ECU disconnected");
            change_state(CONN_STATE_DISCONNECTED);
        }
    }
}

// ===== Public API =====

esp_err_t connection_manager_init(void) {
    // Initialize ISO-TP coordinator first
    esp_err_t ret = isotp_coordinator_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ISO-TP coordinator");
        return ret;
    }
    
    current_state = CONN_STATE_DISCONNECTED;
    current_status = CONN_STATUS_DISCONNECTED;
    state_entry_time = 0;
    last_keepalive_time = 0;
    last_logger_poll_time = 0;
    is_patched = false;
    is_paired = false;
    
    log_buffer_address = 0;
    patch_version = 0;
    patch_buffer_size = 0;
    
    ecu_info_init(&current_ecu_info);
    ecu_info_init(&stored_ecu_info);
    
    ESP_LOGI(TAG, "Connection manager initialized");
    return ESP_OK;
}

void connection_manager_start_connection(void) {
    if (current_state != CONN_STATE_DISCONNECTED) {
        ESP_LOGW(TAG, "Already connecting or connected");
        return;
    }
    
    ESP_LOGI(TAG, "Starting connection sequence");
    ecu_info_init(&current_ecu_info);
    is_patched = false;
    change_state(CONN_STATE_DISCOVERING);
}

void connection_manager_disconnect(void) {
    ESP_LOGI(TAG, "Disconnecting");
    change_state(CONN_STATE_DISCONNECTED);
}

esp_err_t connection_manager_pair_vehicle(void) {
    // Can only pair when connected
    if (!connection_manager_is_connected()) {
        ESP_LOGE(TAG, "Cannot pair - not connected to vehicle");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if already paired
    if (is_paired) {
        ESP_LOGW(TAG, "Already paired with this vehicle");
        return ESP_OK;
    }
    
    // Validate ECU info is complete
    if (!current_ecu_info.is_valid || strlen(current_ecu_info.vin) == 0) {
        ESP_LOGE(TAG, "Cannot pair - ECU info incomplete");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if trying to pair with different vehicle
    if (nvs_manager_load_ecu_info(&stored_ecu_info) == ESP_OK) {
        if (strcmp(stored_ecu_info.vin, current_ecu_info.vin) != 0) {
            ESP_LOGW(TAG, "WARNING: Replacing existing pairing!");
            ESP_LOGW(TAG, "Old VIN: %s", stored_ecu_info.vin);
            ESP_LOGW(TAG, "New VIN: %s", current_ecu_info.vin);
        }
    }
    
    // Save pairing
    ESP_LOGI(TAG, "Pairing with vehicle VIN: %s", current_ecu_info.vin);
    esp_err_t err = nvs_manager_save_ecu_info(&current_ecu_info);
    
    if (err == ESP_OK) {
        is_paired = true;
        ESP_LOGI(TAG, "Vehicle paired successfully");
        ESP_LOGI(TAG, "VIN: %s", current_ecu_info.vin);
        ESP_LOGI(TAG, "Hardware: %s", current_ecu_info.hardware_version);
        ESP_LOGI(TAG, "Software: %s", current_ecu_info.software_version);
        ESP_LOGI(TAG, "Build ID: %s", current_ecu_info.build_id);
        
        // Disconnect to force clean reconnection with paired state
        ESP_LOGI(TAG, "Disconnecting to apply paired state - please reconnect");
        change_state(CONN_STATE_DISCONNECTED);
    } else {
        ESP_LOGE(TAG, "Failed to save pairing: %s", esp_err_to_name(err));
    }
    
    return err;
}

bool connection_manager_is_paired(void) {
    return is_paired;
}

void connection_manager_update(void) {
    switch (current_state) {
        case CONN_STATE_DISCONNECTED:
            if (check_timeout(2000)) {
                ecu_info_init(&current_ecu_info);
                is_patched = false;
                change_state(CONN_STATE_DISCOVERING);
            }
            break;
        case CONN_STATE_DISCOVERING:
            handle_discovering();
            break;
        case CONN_STATE_WAIT_DISCOVER_RESPONSE:
            handle_wait_discover_response();
            break;
        case CONN_STATE_REQUEST_VIN:
            handle_request_vin();
            break;
        case CONN_STATE_WAIT_VIN_RESPONSE:
            handle_wait_vin_response();
            break;
        case CONN_STATE_CHECK_PAIRING:
            handle_check_pairing();
            break;
        case CONN_STATE_REQUEST_SERIAL:
            handle_request_serial();
            break;
        case CONN_STATE_WAIT_SERIAL_RESPONSE:
            handle_wait_serial_response();
            break;
        case CONN_STATE_REQUEST_SOFTWARE_VERSION:
            handle_request_software_version();
            break;
        case CONN_STATE_WAIT_SOFTWARE_VERSION_RESPONSE:
            handle_wait_software_version_response();
            break;
        case CONN_STATE_REQUEST_BUILD_ID:
            handle_request_build_id();
            break;
        case CONN_STATE_WAIT_BUILD_ID_RESPONSE:
            handle_wait_build_id_response();
            break;
        case CONN_STATE_REQUEST_EXTENDED_SESSION:
            handle_request_extended_session();
            break;
        case CONN_STATE_WAIT_EXTENDED_SESSION_RESPONSE:
            handle_wait_extended_session_response();
            break;
        case CONN_STATE_REQUEST_PROGRAMMING_SESSION:
        case CONN_STATE_WAIT_PROGRAMMING_SESSION_RESPONSE:
        case CONN_STATE_REQUEST_SECURITY_SEED:
        case CONN_STATE_WAIT_SECURITY_SEED_RESPONSE:
        case CONN_STATE_SEND_SECURITY_KEY:
        case CONN_STATE_WAIT_SECURITY_KEY_RESPONSE:
            /* TODO: Implement security access when needed for ECU writes */
            break;
        case CONN_STATE_CHECK_PATCH_STATUS:
            handle_check_patch_status();
            break;
        case CONN_STATE_WAIT_PATCH_RESPONSE:
            handle_wait_patch_response();
            break;
        case CONN_STATE_REQUEST_PATCH_INFO:
            handle_request_patch_info();
            break;
        case CONN_STATE_WAIT_PATCH_INFO_RESPONSE:
            handle_wait_patch_info_response();
            break;
        case CONN_STATE_CHECK_LOGGER_CONFIG:
            handle_check_logger_config();
            break;
        case CONN_STATE_CONFIGURE_LOGGER:
            handle_configure_logger();
            break;
        case CONN_STATE_WAIT_LOGGER_CONFIG_RESPONSE:
            handle_wait_logger_config_response();
            break;
        case CONN_STATE_CONNECTED:
            handle_connected();
            break;
        case CONN_STATE_POLLING_LOGGER:
            handle_polling_logger();
            break;
        case CONN_STATE_WAIT_LOGGER_POLL_RESPONSE:
            handle_wait_logger_poll_response();
            break;
        case CONN_STATE_ERROR:
            if (check_timeout(5000)) {
                ESP_LOGW(TAG, "Recovering from ERROR state");
                is_patched = false;
                is_paired = false;
                memset(&current_ecu_info, 0, sizeof(current_ecu_info));
                change_state(CONN_STATE_DISCONNECTED);
            }
            break;
    }
}

connection_status_t connection_manager_get_status(void) {
    return current_status;
}

connection_state_t connection_manager_get_state(void) {
    return current_state;
}

bool connection_manager_is_connected(void) {
    return (current_status == CONN_STATUS_CONNECTED_UNPATCHED || 
            current_status == CONN_STATUS_CONNECTED_PATCHED);
}

bool connection_manager_is_patched(void) {
    return is_patched;
}

bool connection_manager_is_logger_configured(void) {
    return is_patched && logger_manager_is_configured();
}

bool connection_manager_has_logger_data(void) {
    return is_patched && logger_manager_has_data();
}

uint32_t connection_manager_get_log_buffer_address(void) {
    return log_buffer_address;
}

uint8_t connection_manager_get_patch_version(void) {
    return patch_version;
}

uint16_t connection_manager_get_patch_buffer_size(void) {
    return patch_buffer_size;
}

const char* connection_manager_get_boxcode(void) {
    return ecu_info_get_boxcode(&current_ecu_info);
}

const char* connection_manager_get_vin(void) {
    /* current_ecu_info.vin is a fixed-size NUL-terminated char buffer
     * populated by handle_wait_vin_response(). When the VIN read has
     * not yet completed, the buffer is zeroed at boot, so this
     * returns an empty string. */
    return current_ecu_info.vin;
}

bool connection_manager_logger_add_variable(uint32_t address, 
                                            uint8_t size,
                                            float scale,
                                            float offset,
                                            const char *name) {
    if (!is_patched) {
        ESP_LOGW(TAG, "Cannot add logger variables - ECU not patched");
        return false;
    }
    return logger_manager_add_variable(address, size, scale, offset, name);
}

void connection_manager_logger_clear_variables(void) {
    logger_manager_clear_variables();
}

uint8_t connection_manager_logger_get_variable_count(void) {
    return logger_manager_get_variable_count();
}

bool connection_manager_logger_add_variable_by_name(const char *name) {
    if (!is_patched) {
        ESP_LOGW(TAG, "Cannot add logger variables - ECU not patched");
        return false;
    }
    return logger_variables_add_by_name(name);
}

bool connection_manager_logger_add_all_required(void) {
    if (!is_patched) {
        ESP_LOGW(TAG, "Cannot add logger variables - ECU not patched");
        return false;
    }
    return logger_variables_add_all_required();
}

float connection_manager_logger_get_value_by_name(const char *name) {
    return logger_manager_get_variable_value_by_name(name);
}

void connection_manager_logger_start(void) {
    logger_polling_enabled = true;
    last_logger_poll_time = 0;
    ESP_LOGI(TAG, "Logger polling enabled");
}

void connection_manager_logger_stop(void) {
    logger_polling_enabled = false;
    ESP_LOGI(TAG, "Logger polling disabled");
}

bool connection_manager_logger_is_running(void) {
    return logger_polling_enabled;
}

const char* connection_manager_get_state_name(connection_state_t state) {
    switch (state) {
        case CONN_STATE_DISCONNECTED: return "DISCONNECTED";
        case CONN_STATE_DISCOVERING: return "DISCOVERING";
        case CONN_STATE_WAIT_DISCOVER_RESPONSE: return "WAIT_DISCOVER_RESPONSE";
        case CONN_STATE_REQUEST_VIN: return "REQUEST_VIN";
        case CONN_STATE_WAIT_VIN_RESPONSE: return "WAIT_VIN_RESPONSE";
        case CONN_STATE_CHECK_PAIRING: return "CHECK_PAIRING";
        case CONN_STATE_REQUEST_SERIAL: return "REQUEST_SERIAL";
        case CONN_STATE_WAIT_SERIAL_RESPONSE: return "WAIT_SERIAL_RESPONSE";
        case CONN_STATE_REQUEST_SOFTWARE_VERSION: return "REQUEST_SOFTWARE_VERSION";
        case CONN_STATE_WAIT_SOFTWARE_VERSION_RESPONSE: return "WAIT_SOFTWARE_VERSION_RESPONSE";
        case CONN_STATE_REQUEST_BUILD_ID: return "REQUEST_BUILD_ID";
        case CONN_STATE_WAIT_BUILD_ID_RESPONSE: return "WAIT_BUILD_ID_RESPONSE";
        case CONN_STATE_REQUEST_EXTENDED_SESSION: return "REQUEST_EXTENDED_SESSION";
        case CONN_STATE_WAIT_EXTENDED_SESSION_RESPONSE: return "WAIT_EXTENDED_SESSION_RESPONSE";
        case CONN_STATE_REQUEST_PROGRAMMING_SESSION: return "REQUEST_PROGRAMMING_SESSION";
        case CONN_STATE_WAIT_PROGRAMMING_SESSION_RESPONSE: return "WAIT_PROGRAMMING_SESSION_RESPONSE";
        case CONN_STATE_REQUEST_SECURITY_SEED: return "REQUEST_SECURITY_SEED";
        case CONN_STATE_WAIT_SECURITY_SEED_RESPONSE: return "WAIT_SECURITY_SEED_RESPONSE";
        case CONN_STATE_SEND_SECURITY_KEY: return "SEND_SECURITY_KEY";
        case CONN_STATE_WAIT_SECURITY_KEY_RESPONSE: return "WAIT_SECURITY_KEY_RESPONSE";
        case CONN_STATE_CHECK_PATCH_STATUS: return "CHECK_PATCH_STATUS";
        case CONN_STATE_WAIT_PATCH_RESPONSE: return "WAIT_PATCH_RESPONSE";
        case CONN_STATE_REQUEST_PATCH_INFO: return "REQUEST_PATCH_INFO";
        case CONN_STATE_WAIT_PATCH_INFO_RESPONSE: return "WAIT_PATCH_INFO_RESPONSE";
        case CONN_STATE_CHECK_LOGGER_CONFIG: return "CHECK_LOGGER_CONFIG";
        case CONN_STATE_CONFIGURE_LOGGER: return "CONFIGURE_LOGGER";
        case CONN_STATE_WAIT_LOGGER_CONFIG_RESPONSE: return "WAIT_LOGGER_CONFIG_RESPONSE";
        case CONN_STATE_CONNECTED: return "CONNECTED";
        case CONN_STATE_POLLING_LOGGER: return "POLLING_LOGGER";
        case CONN_STATE_WAIT_LOGGER_POLL_RESPONSE: return "WAIT_LOGGER_POLL_RESPONSE";
        case CONN_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

