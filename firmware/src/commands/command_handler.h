#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define CMD_PASSWORD_DEFAULT "futuner_admin_2024"
#define CMD_PASSWORD_NVS_KEY "cmd_password"
#define CMD_MAX_RESPONSE_SIZE 8192

typedef enum {
    CMD_SECURITY_UNSECURED,
    CMD_SECURITY_SECURED
} command_security_t;

typedef esp_err_t (*command_execute_fn_t)(int fd, const char *params, char *response, size_t response_size);

typedef struct {
    const char *name;
    const char *description;
    command_security_t security;
    command_execute_fn_t execute;
} command_def_t;

esp_err_t command_handler_init(void);
void command_handler_process_message(int fd, const char *message, size_t len);

bool command_handler_is_client_authenticated(int fd);
void command_handler_clear_authentication(int fd);

#endif // COMMAND_HANDLER_H

