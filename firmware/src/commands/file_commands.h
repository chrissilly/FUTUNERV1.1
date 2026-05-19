#ifndef FILE_COMMANDS_H
#define FILE_COMMANDS_H

#include "command_handler.h"

esp_err_t cmd_fs_info(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_fs_list(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_fs_read(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_fs_write(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_fs_delete(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_fs_mkdir(int fd, const char *params, char *response, size_t response_size);

#endif // FILE_COMMANDS_H

