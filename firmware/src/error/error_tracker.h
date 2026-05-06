#ifndef ERROR_TRACKER_H
#define ERROR_TRACKER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define ERROR_LOG_SIZE 50
#define ERROR_MESSAGE_MAX_LEN 128

typedef enum {
    ERROR_CATEGORY_SYSTEM,
    ERROR_CATEGORY_WIFI,
    ERROR_CATEGORY_CAN,
    ERROR_CATEGORY_CONNECTION,
    ERROR_CATEGORY_LOGGER,
    ERROR_CATEGORY_NVS,
    ERROR_CATEGORY_WEBSOCKET,
    ERROR_CATEGORY_COMMAND
} error_category_t;

typedef enum {
    ERROR_SEVERITY_INFO,
    ERROR_SEVERITY_WARNING,
    ERROR_SEVERITY_ERROR,
    ERROR_SEVERITY_CRITICAL
} error_severity_t;

typedef struct {
    uint32_t timestamp_ms;
    error_category_t category;
    error_severity_t severity;
    char message[ERROR_MESSAGE_MAX_LEN];
    bool active;
} error_entry_t;

esp_err_t error_tracker_init(void);

void error_tracker_log(error_category_t category, 
                       error_severity_t severity, 
                       const char *format, ...);

uint16_t error_tracker_get_count(void);
uint16_t error_tracker_get_error_count(void);
uint16_t error_tracker_get_warning_count(void);

const error_entry_t* error_tracker_get_entry(uint16_t index);
void error_tracker_clear(void);

const char* error_tracker_category_to_string(error_category_t category);
const char* error_tracker_severity_to_string(error_severity_t severity);

void error_tracker_broadcast_error(error_category_t category, 
                                   error_severity_t severity, 
                                   const char *message);

#endif // ERROR_TRACKER_H

