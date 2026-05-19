#ifndef ECU_WRITE_COMMANDS_H
#define ECU_WRITE_COMMANDS_H

#include <stddef.h>
#include "esp_err.h"

esp_err_t cmd_write_ecu(int fd, const char *params, char *response, size_t response_size);

#endif // ECU_WRITE_COMMANDS_H

