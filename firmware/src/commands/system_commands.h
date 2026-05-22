#ifndef SYSTEM_COMMANDS_H
#define SYSTEM_COMMANDS_H

#include "command_handler.h"

esp_err_t cmd_get_status(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_list_commands(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_get_errors(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_clear_errors(int fd, const char *params, char *response, size_t response_size);

esp_err_t cmd_wifi_connect(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_wifi_disconnect(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_wifi_status(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_logger_start(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_logger_stop(int fd, const char *params, char *response, size_t response_size);

/* P-59: soft restart via esp_restart() after the WS ACK is sent. The
 * actual restart is deferred SYSTEM_CMD_REBOOT_ACK_DELAY_MS so the
 * httpd has time to flush the response to the client. SECURED. */
esp_err_t cmd_reboot(int fd, const char *params, char *response, size_t response_size);

#endif // SYSTEM_COMMANDS_H

