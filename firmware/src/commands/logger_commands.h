#ifndef LOGGER_COMMANDS_H
#define LOGGER_COMMANDS_H

#include "command_handler.h"

esp_err_t cmd_configure_logger(int fd, const char *params, char *response, size_t response_size);

#endif // LOGGER_COMMANDS_H

