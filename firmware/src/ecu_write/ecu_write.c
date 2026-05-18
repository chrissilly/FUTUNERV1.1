#include "ecu_write.h"
#include "can/can_manager.h"
#include "state_machine/uds_protocol.h"
#include "can/isotp_shims.h"
#include "isotp_coordinator/isotp_coordinator.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ECU_WRITE";

// Write states
typedef enum {
    WRITE_STATE_IDLE,
    WRITE_STATE_SENDING_CHUNK,
    WRITE_STATE_WAIT_CHUNK_RESPONSE,
    WRITE_STATE_SENDING_FOLLOWUP,
    WRITE_STATE_WAIT_FOLLOWUP_RESPONSE,
    WRITE_STATE_COMPLETE,
    WRITE_STATE_ERROR
} ecu_write_internal_state_t;

// Write operation state
typedef struct {
    uint32_t base_address;       // Original target address
    const uint8_t *data;         // Data buffer (user must keep valid during write!)
    size_t total_size;           // Total data size
    size_t offset;               // Current write offset
    uint8_t mid_byte;            // Mid byte from boxcode
    uint32_t address_offset;     // Address offset from boxcode
    ecu_write_callback_t callback;
    void *user_data;
    uint8_t original_request[256]; // Store original request for retry
    size_t original_size;
    ecu_write_internal_state_t state;
    uint32_t request_time;       // Time when request was sent
    uint32_t timeout_ms;         // Timeout for responses
} ecu_write_state_t;

static ecu_write_state_t write_state = {
    .state = WRITE_STATE_IDLE
};

// Forward declarations
static void send_next_chunk(void);
static void check_timeout(void);
static void handle_response(const uint8_t *response, size_t response_len);

static void cleanup_write_state(bool notify_callback, bool success) {
    if (write_state.state == WRITE_STATE_IDLE) {
        return;
    }
    
    if (notify_callback && write_state.callback) {
        write_state.callback(success, write_state.user_data);
    }
    
    write_state.state = WRITE_STATE_IDLE;
    write_state.data = NULL;
    write_state.callback = NULL;
}

static void send_next_chunk(void) {
    if (write_state.offset >= write_state.total_size) {
        ESP_LOGI(TAG, "Write operation completed successfully");
        write_state.state = WRITE_STATE_COMPLETE;
        isotp_coordinator_release(ISOTP_OWNER_ECU_WRITE);
        cleanup_write_state(true, true);
        return;
    }
    
    // Calculate chunk size
    size_t remaining = write_state.total_size - write_state.offset;
    size_t chunk_size = (remaining > UDS_WRITE_MAX_CHUNK_SIZE) ? 
                        UDS_WRITE_MAX_CHUNK_SIZE : remaining;
    
    uint32_t current_address = write_state.base_address + write_state.offset;
    const uint8_t *chunk_data = write_state.data + write_state.offset;
    
    ESP_LOGD(TAG, "Writing chunk: addr=0x%08lX, size=%u, offset=%u/%u",
             current_address, chunk_size, write_state.offset, write_state.total_size);
    
    // Apply address offset
    current_address -= write_state.address_offset;
    
    // Build request
    uint8_t request[256];
    size_t request_size = 0;
    
    request[request_size++] = UDS_SERVICE_TESTER_PRESENT;
    request[request_size++] = UDS_WRITE_RAM_SUBFUNCTION;
    request[request_size++] = UDS_WRITE_FIXED_BYTE;
    request[request_size++] = write_state.mid_byte;
    request[request_size++] = (current_address >> 16) & 0xFF;
    request[request_size++] = (current_address >> 8) & 0xFF;
    request[request_size++] = current_address & 0xFF;
    
    memcpy(&request[request_size], chunk_data, chunk_size);
    request_size += chunk_size;
    
    // Store original request for potential retry
    memcpy(write_state.original_request, request, request_size);
    write_state.original_size = request_size;
    
    // Send request
    esp_err_t err = can_manager_send_isotp(request, request_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send write request: %s", esp_err_to_name(err));
        write_state.state = WRITE_STATE_ERROR;
        isotp_coordinator_release(ISOTP_OWNER_ECU_WRITE);
        cleanup_write_state(true, false);
        return;
    }
    
    // Transition to waiting for response
    write_state.state = WRITE_STATE_WAIT_CHUNK_RESPONSE;
    write_state.request_time = isotp_user_get_ms();
}

static void check_timeout(void) {
    uint32_t elapsed = isotp_user_get_ms() - write_state.request_time;
    if (elapsed >= write_state.timeout_ms) {
        ESP_LOGE(TAG, "Write operation timeout in state %d", write_state.state);
        write_state.state = WRITE_STATE_ERROR;
        isotp_coordinator_release(ISOTP_OWNER_ECU_WRITE);
        cleanup_write_state(true, false);
    }
}

static void handle_response(const uint8_t *response, size_t response_len) {
    if (response_len < 2) {
        ESP_LOGE(TAG, "Response too short");
        write_state.state = WRITE_STATE_ERROR;
        isotp_coordinator_release(ISOTP_OWNER_ECU_WRITE);
        cleanup_write_state(true, false);
        return;
    }
    
    // Handle potential single-frame offset
    int offset = (response_len > 2 && response[0] <= 0x10) ? 1 : 0;
    
    uint8_t service = response[offset];
    uint8_t subfunction = (response_len > offset + 1) ? response[offset + 1] : 0;
    
    if (write_state.state == WRITE_STATE_WAIT_CHUNK_RESPONSE) {
        // Check for "not ready" response (0x7E 0x05)
        if (service == (UDS_SERVICE_TESTER_PRESENT + 0x40) && subfunction == 0x05) {
            ESP_LOGD(TAG, "ECU not ready, sending follow-up request");
            
            // Send follow-up request: {0x02, 0x3E, 0x37}
            uint8_t followup[UDS_WRITE_FOLLOWUP_REQUEST_SIZE] = {0x02, UDS_SERVICE_TESTER_PRESENT, 0x37};
            
            esp_err_t err = can_manager_send_isotp(followup, sizeof(followup));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send follow-up request: %s", esp_err_to_name(err));
                write_state.state = WRITE_STATE_ERROR;
                isotp_coordinator_release(ISOTP_OWNER_ECU_WRITE);
                cleanup_write_state(true, false);
                return;
            }
            
            write_state.state = WRITE_STATE_WAIT_FOLLOWUP_RESPONSE;
            write_state.request_time = isotp_user_get_ms();
            return;
        }
        
        // Check for success response (0x7E 0x01)
        if (service == (UDS_SERVICE_TESTER_PRESENT + 0x40) && subfunction == 0x01) {
            ESP_LOGD(TAG, "Chunk write succeeded");
            
            // Calculate bytes written (original request size - 7 header bytes)
            size_t bytes_written = write_state.original_size - 7;
            write_state.offset += bytes_written;
            
            // Send next chunk
            write_state.state = WRITE_STATE_SENDING_CHUNK;
            send_next_chunk();
            return;
        }
        
        // Unexpected response
        ESP_LOGE(TAG, "Unexpected write response: 0x%02X 0x%02X", service, subfunction);
        write_state.state = WRITE_STATE_ERROR;
        isotp_coordinator_release(ISOTP_OWNER_ECU_WRITE);
        cleanup_write_state(true, false);
        
    } else if (write_state.state == WRITE_STATE_WAIT_FOLLOWUP_RESPONSE) {
        // Handle follow-up response
        if (response_len < 4) {
            ESP_LOGE(TAG, "Follow-up response too short");
            write_state.state = WRITE_STATE_ERROR;
            isotp_coordinator_release(ISOTP_OWNER_ECU_WRITE);
            cleanup_write_state(true, false);
            return;
        }
        
        uint8_t status = response[offset + 2];
        uint8_t mid_byte_echo = response[offset + 3];
        
        // Validate response: 0x7E 0x01 [0x00 or 0x01] [mid_byte] [address_echo]
        bool is_valid = (service == (UDS_SERVICE_TESTER_PRESENT + 0x40) &&
                         subfunction == 0x01 &&
                         (status == 0x00 || status == 0x01) &&
                         mid_byte_echo == write_state.mid_byte);
        
        // Verify address echo if response is long enough
        if (is_valid && response_len >= (offset + 7)) {
            uint8_t addr_echo[3] = {
                response[offset + 4],
                response[offset + 5],
                response[offset + 6]
            };
            uint8_t addr_sent[3] = {
                write_state.original_request[4],
                write_state.original_request[5],
                write_state.original_request[6]
            };
            
            if (memcmp(addr_echo, addr_sent, 3) != 0) {
                is_valid = false;
                ESP_LOGE(TAG, "Address echo mismatch");
            }
        }
        
        if (is_valid) {
            ESP_LOGD(TAG, "Follow-up validation passed, chunk complete");
            
            // Calculate bytes written
            size_t bytes_written = write_state.original_size - 7;
            write_state.offset += bytes_written;
            
            // Send next chunk
            write_state.state = WRITE_STATE_SENDING_CHUNK;
            send_next_chunk();
        } else {
            ESP_LOGE(TAG, "Follow-up validation failed");
            write_state.state = WRITE_STATE_ERROR;
            isotp_coordinator_release(ISOTP_OWNER_ECU_WRITE);
            cleanup_write_state(true, false);
        }
    }
}

void ecu_write_poll_response(const uint8_t *response, uint16_t response_len) {
    if (write_state.state == WRITE_STATE_IDLE || 
        write_state.state == WRITE_STATE_COMPLETE ||
        write_state.state == WRITE_STATE_ERROR) {
        return;  // Not waiting for response
    }
    
    if (write_state.state == WRITE_STATE_WAIT_CHUNK_RESPONSE ||
        write_state.state == WRITE_STATE_WAIT_FOLLOWUP_RESPONSE) {
        handle_response(response, response_len);
    }
}

void ecu_write_poll(void) {
    if (write_state.state == WRITE_STATE_IDLE ||
        write_state.state == WRITE_STATE_COMPLETE) {
        return;
    }
    
    if (write_state.state == WRITE_STATE_SENDING_CHUNK) {
        // Don't do anything, waiting for send_next_chunk to be called
        return;
    }
    
    // Check for timeout in waiting states
    if (write_state.state == WRITE_STATE_WAIT_CHUNK_RESPONSE ||
        write_state.state == WRITE_STATE_WAIT_FOLLOWUP_RESPONSE) {
        check_timeout();
    }
}

esp_err_t ecu_write_data(uint32_t address, 
                         const uint8_t *data, 
                         size_t size,
                         uint8_t mid_byte,
                         uint32_t address_offset,
                         ecu_write_callback_t callback,
                         void *user_data) {
    if (!data || size == 0) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (write_state.state != WRITE_STATE_IDLE) {
        ESP_LOGE(TAG, "Write operation already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Try to acquire ISO-TP channel ownership
    if (!isotp_coordinator_request(ISOTP_OWNER_ECU_WRITE, 0)) {
        ESP_LOGW(TAG, "ISO-TP channel busy, cannot start write");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting write: addr=0x%08lX, size=%u, mid_byte=0x%02X, offset=0x%08lX",
             address, size, mid_byte, address_offset);
    
    // Initialize write state
    write_state.base_address = address;
    write_state.data = data;  // Note: User must keep data valid until write completes!
    write_state.total_size = size;
    write_state.offset = 0;
    write_state.mid_byte = mid_byte;
    write_state.address_offset = address_offset;
    write_state.callback = callback;
    write_state.user_data = user_data;
    write_state.timeout_ms = 1000;  // 1 second timeout
    write_state.state = WRITE_STATE_SENDING_CHUNK;
    
    // Start first chunk
    send_next_chunk();
    
    return ESP_OK;
}

bool ecu_write_is_busy(void) {
    return (write_state.state != WRITE_STATE_IDLE && 
            write_state.state != WRITE_STATE_COMPLETE);
}

void ecu_write_cancel(void) {
    if (write_state.state != WRITE_STATE_IDLE) {
        ESP_LOGW(TAG, "Write operation cancelled");
        write_state.state = WRITE_STATE_ERROR;
        isotp_coordinator_release(ISOTP_OWNER_ECU_WRITE);
        cleanup_write_state(false, false);
    }
}

