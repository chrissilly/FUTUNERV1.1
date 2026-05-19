#ifndef HOST_SHIM_ESP_EVENT_H
#define HOST_SHIM_ESP_EVENT_H

#include "esp_err.h"
#include <stdint.h>

typedef const char *esp_event_base_t;

#define ESP_EVENT_ANY_ID (-1)

typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base,
                                    int32_t event_id, void *event_data);

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base,
                                              int32_t event_id,
                                              esp_event_handler_t handler,
                                              void *arg,
                                              void *instance);

#endif /* HOST_SHIM_ESP_EVENT_H */
