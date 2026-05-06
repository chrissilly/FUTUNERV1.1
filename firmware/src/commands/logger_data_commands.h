#ifndef LOGGER_DATA_COMMANDS_H
#define LOGGER_DATA_COMMANDS_H

#include <stddef.h>
#include "esp_err.h"

esp_err_t cmd_get_logger_data(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_get_single_variable(int fd, const char *params, char *response, size_t response_size);

#endif // LOGGER_DATA_COMMANDS_H

