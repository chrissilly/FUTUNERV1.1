#ifndef FLEX_COMMANDS_H
#define FLEX_COMMANDS_H

#include "command_handler.h"

esp_err_t cmd_flex_load_scal(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_flex_unload_scal(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_flex_status(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_flex_enable(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_flex_disable(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_flex_set_override(int fd, const char *params, char *response, size_t response_size);

#endif // FLEX_COMMANDS_H
