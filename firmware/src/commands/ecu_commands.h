#ifndef ECU_COMMANDS_H
#define ECU_COMMANDS_H

#include "command_handler.h"

esp_err_t cmd_pair_ecu(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_remove_pairing(int fd, const char *params, char *response, size_t response_size);

#endif // ECU_COMMANDS_H

