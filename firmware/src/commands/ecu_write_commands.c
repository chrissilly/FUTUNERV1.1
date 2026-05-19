#include "ecu_write_commands.h"
#include "ecu_write/ecu_write.h"
#include "logger/logger_variables.h"
#include "state_machine/connection_manager.h"
#include "websocket/ws_server.h"
#include "error/error_tracker.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ECU_WRITE_CMD";

// Store client FD for async callback
static int pending_write_client_fd = -1;

static void write_completion_callback(bool success, void *user_data) {
    // Free the data buffer that was allocated
    if (user_data != NULL) {
        free(user_data);
    }
    
    if (pending_write_client_fd < 0) {
        return;  // No client waiting
    }
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", success ? "success" : "error");
    cJSON_AddStringToObject(response, "command", "write_ecu");
    cJSON_AddStringToObject(response, "message", 
                            success ? "Write completed successfully" : "Write failed");
    
    char *json_str = cJSON_PrintUnformatted(response);
    if (json_str) {
        ws_server_send_text(pending_write_client_fd, json_str);
        free(json_str);
    }
    cJSON_Delete(response);
    
    if (success) {
        ESP_LOGI(TAG, "ECU write completed successfully");
    } else {
        ESP_LOGE(TAG, "ECU write failed");
        error_tracker_log(ERROR_CATEGORY_CONNECTION, ERROR_SEVERITY_ERROR, "ECU write operation failed");
    }
    
    pending_write_client_fd = -1;
}

esp_err_t cmd_write_ecu(int fd, const char *params, char *response, size_t response_size) {
    cJSON *root = cJSON_CreateObject();
    
    // Check if connected and patched
    if (!connection_manager_is_connected()) {
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "Not connected to ECU");
        
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            strncpy(response, json_str, response_size - 1);
            response[response_size - 1] = '\0';
            free(json_str);
        }
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    if (!connection_manager_is_patched()) {
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "ECU not patched - write not available");
        
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            strncpy(response, json_str, response_size - 1);
            response[response_size - 1] = '\0';
            free(json_str);
        }
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    // Check if write already in progress
    if (ecu_write_is_busy()) {
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "ECU write already in progress");
        
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            strncpy(response, json_str, response_size - 1);
            response[response_size - 1] = '\0';
            free(json_str);
        }
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    // Parse parameters
    cJSON *params_json = cJSON_Parse(params);
    if (!params_json) {
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "Invalid JSON parameters");
        
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            strncpy(response, json_str, response_size - 1);
            response[response_size - 1] = '\0';
            free(json_str);
        }
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    // Get address parameter
    cJSON *address_param = cJSON_GetObjectItem(params_json, "address");
    if (!address_param || !cJSON_IsNumber(address_param)) {
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "Missing or invalid 'address' parameter");
        
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            strncpy(response, json_str, response_size - 1);
            response[response_size - 1] = '\0';
            free(json_str);
        }
        cJSON_Delete(params_json);
        cJSON_Delete(root);
        return ESP_OK;
    }
    /* H2 fix: use valuedouble to avoid truncation of addresses > 0x7FFFFFFF */
    uint32_t address = (uint32_t)address_param->valuedouble;
    
    // Get data parameter (hex string or base64)
    cJSON *data_param = cJSON_GetObjectItem(params_json, "data");
    if (!data_param || !cJSON_IsString(data_param)) {
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "Missing or invalid 'data' parameter");
        
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            strncpy(response, json_str, response_size - 1);
            response[response_size - 1] = '\0';
            free(json_str);
        }
        cJSON_Delete(params_json);
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    // Check format (hex or base64)
    cJSON *format_param = cJSON_GetObjectItem(params_json, "format");
    const char *format = (format_param && cJSON_IsString(format_param)) ? format_param->valuestring : "hex";
    
    // Decode data
    uint8_t *data_buffer = NULL;
    size_t data_size = 0;
    
    if (strcmp(format, "base64") == 0) {
        // Base64 decode
        const char *base64_data = data_param->valuestring;
        size_t base64_len = strlen(base64_data);
        size_t decoded_len = 0;
        
        // Calculate required buffer size
        mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *)base64_data, base64_len);
        
        data_buffer = malloc(decoded_len);
        if (!data_buffer) {
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddStringToObject(root, "message", "Memory allocation failed");
            
            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str) {
                strncpy(response, json_str, response_size - 1);
                response[response_size - 1] = '\0';
                free(json_str);
            }
            cJSON_Delete(params_json);
            cJSON_Delete(root);
            return ESP_OK;
        }
        
        if (mbedtls_base64_decode(data_buffer, decoded_len, &data_size, 
                                   (const unsigned char *)base64_data, base64_len) != 0) {
            free(data_buffer);
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddStringToObject(root, "message", "Invalid base64 data");
            
            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str) {
                strncpy(response, json_str, response_size - 1);
                response[response_size - 1] = '\0';
                free(json_str);
            }
            cJSON_Delete(params_json);
            cJSON_Delete(root);
            return ESP_OK;
        }
    } else {
        // Hex decode
        const char *hex_data = data_param->valuestring;
        size_t hex_len = strlen(hex_data);
        
        if (hex_len % 2 != 0) {
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddStringToObject(root, "message", "Invalid hex string (odd length)");
            
            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str) {
                strncpy(response, json_str, response_size - 1);
                response[response_size - 1] = '\0';
                free(json_str);
            }
            cJSON_Delete(params_json);
            cJSON_Delete(root);
            return ESP_OK;
        }
        
        data_size = hex_len / 2;
        data_buffer = malloc(data_size);
        if (!data_buffer) {
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddStringToObject(root, "message", "Memory allocation failed");
            
            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str) {
                strncpy(response, json_str, response_size - 1);
                response[response_size - 1] = '\0';
                free(json_str);
            }
            cJSON_Delete(params_json);
            cJSON_Delete(root);
            return ESP_OK;
        }
        
        for (size_t i = 0; i < data_size; i++) {
            sscanf(&hex_data[i * 2], "%2hhx", &data_buffer[i]);
        }
    }
    
    // Get boxcode-specific parameters
    uint8_t mid_byte = logger_variables_get_write_mid_byte();
    uint32_t address_offset = logger_variables_get_write_address_offset();
    
    /* H4 fix: reject concurrent writes — only one at a time */
    if (pending_write_client_fd >= 0) {
        free(data_buffer);
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "Write already in progress");
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            strncpy(response, json_str, response_size - 1);
            response[response_size - 1] = '\0';
            free(json_str);
        }
        cJSON_Delete(params_json);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    // Store client FD for async callback
    pending_write_client_fd = fd;
    
    // Start write operation (data_buffer will be freed in callback)
    esp_err_t err = ecu_write_data(address, data_buffer, data_size, mid_byte, 
                                    address_offset, write_completion_callback, data_buffer);
    
    if (err == ESP_OK) {
        cJSON_AddStringToObject(root, "status", "success");
        cJSON_AddStringToObject(root, "message", "Write operation started");
        cJSON_AddNumberToObject(root, "address", address);
        cJSON_AddNumberToObject(root, "size", data_size);
        ESP_LOGI(TAG, "ECU write started: addr=0x%08lX, size=%u", address, data_size);
    } else {
        pending_write_client_fd = -1;  // Reset
        free(data_buffer);
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "Failed to start write operation (channel busy)");
        ESP_LOGE(TAG, "Failed to start ECU write");
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        strncpy(response, json_str, response_size - 1);
        response[response_size - 1] = '\0';
        free(json_str);
    }
    
    cJSON_Delete(params_json);
    cJSON_Delete(root);
    
    // Note: data_buffer will remain valid until write completes (callback frees it)
    return ESP_OK;
}

