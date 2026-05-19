#ifndef WS_SERVER_H
#define WS_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdint.h>
#include <stdbool.h>

#define WS_SERVER_PORT 80
#define WS_MAX_PAYLOAD_SIZE 4096

typedef void (*ws_message_handler_t)(int fd, const char *message, size_t len);

esp_err_t ws_server_init(void);
esp_err_t ws_server_start(void);
esp_err_t ws_server_stop(void);

void ws_server_set_message_handler(ws_message_handler_t handler);

esp_err_t ws_server_send_text(int fd, const char *message);
esp_err_t ws_server_broadcast_text(const char *message);

bool ws_server_is_running(void);
uint8_t ws_server_get_client_count(void);

#endif // WS_SERVER_H

