#ifndef FLASH_COMMANDS_H
#define FLASH_COMMANDS_H

#include "command_handler.h"

/* Flash ECU command */
esp_err_t cmd_flash_ecu(int fd, const char *params, char *response, size_t response_size);

#endif /* FLASH_COMMANDS_H */