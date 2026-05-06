#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    CONN_STATE_DISCONNECTED,
    CONN_STATE_DISCOVERING,
    CONN_STATE_WAIT_DISCOVER_RESPONSE,
    CONN_STATE_REQUEST_VIN,
    CONN_STATE_WAIT_VIN_RESPONSE,
    CONN_STATE_CHECK_PAIRING,
    CONN_STATE_REQUEST_SERIAL,
    CONN_STATE_WAIT_SERIAL_RESPONSE,
    CONN_STATE_REQUEST_SOFTWARE_VERSION,
    CONN_STATE_WAIT_SOFTWARE_VERSION_RESPONSE,
    CONN_STATE_REQUEST_BUILD_ID,
    CONN_STATE_WAIT_BUILD_ID_RESPONSE,
    CONN_STATE_REQUEST_EXTENDED_SESSION,
    CONN_STATE_WAIT_EXTENDED_SESSION_RESPONSE,
    CONN_STATE_REQUEST_PROGRAMMING_SESSION,
    CONN_STATE_WAIT_PROGRAMMING_SESSION_RESPONSE,
    CONN_STATE_REQUEST_SECURITY_SEED,
    CONN_STATE_WAIT_SECURITY_SEED_RESPONSE,
    CONN_STATE_SEND_SECURITY_KEY,
    CONN_STATE_WAIT_SECURITY_KEY_RESPONSE,
    CONN_STATE_CHECK_PATCH_STATUS,
    CONN_STATE_WAIT_PATCH_RESPONSE,
    CONN_STATE_REQUEST_PATCH_INFO,
    CONN_STATE_WAIT_PATCH_INFO_RESPONSE,
    CONN_STATE_CHECK_LOGGER_CONFIG,
    CONN_STATE_CONFIGURE_LOGGER,
    CONN_STATE_WAIT_LOGGER_CONFIG_RESPONSE,
    CONN_STATE_CONNECTED,
    CONN_STATE_POLLING_LOGGER,
    CONN_STATE_WAIT_LOGGER_POLL_RESPONSE,
    CONN_STATE_ERROR
} connection_state_t;

typedef enum {
    CONN_STATUS_DISCONNECTED,
    CONN_STATUS_CONNECTING,
    CONN_STATUS_CONNECTED_UNPATCHED,
    CONN_STATUS_CONNECTED_PATCHED,
    CONN_STATUS_ERROR
} connection_status_t;

esp_err_t connection_manager_init(void);

void connection_manager_start_connection(void);
void connection_manager_disconnect(void);
void connection_manager_update(void);

esp_err_t connection_manager_pair_vehicle(void);

connection_status_t connection_manager_get_status(void);
connection_state_t connection_manager_get_state(void);
bool connection_manager_is_connected(void);
bool connection_manager_is_patched(void);
bool connection_manager_is_paired(void);
bool connection_manager_is_logger_configured(void);
bool connection_manager_has_logger_data(void);

uint32_t connection_manager_get_log_buffer_address(void);
uint8_t connection_manager_get_patch_version(void);
uint16_t connection_manager_get_patch_buffer_size(void);
const char* connection_manager_get_boxcode(void);

/* Returns the current ECU VIN as a NUL-terminated string. Returns
 * an empty string if the VIN read has not yet completed. The
 * pointer is owned by connection_manager and is valid until the
 * next discovery cycle; callers needing to retain the value must
 * copy it. NOT normalized (case / whitespace) — the comparison
 * site is responsible for ISO-3779 normalization (uppercase + trim). */
const char* connection_manager_get_vin(void);

bool connection_manager_logger_add_variable(uint32_t address, 
                                            uint8_t size,
                                            float scale,
                                            float offset,
                                            const char *name);
bool connection_manager_logger_add_variable_by_name(const char *name);
bool connection_manager_logger_add_all_required(void);
void connection_manager_logger_clear_variables(void);
uint8_t connection_manager_logger_get_variable_count(void);
float connection_manager_logger_get_value_by_name(const char *name);

const char* connection_manager_get_state_name(connection_state_t state);

/* Logger gating — off by default to avoid contention with live tuning,
 * flashing, and DTC reads. Caller must explicitly start. */
void connection_manager_logger_start(void);
void connection_manager_logger_stop(void);
bool connection_manager_logger_is_running(void);

#endif // CONNECTION_MANAGER_H

