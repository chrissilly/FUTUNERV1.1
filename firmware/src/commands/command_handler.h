#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define CMD_MAX_RESPONSE_SIZE 8192

typedef enum {
    CMD_SECURITY_UNSECURED = 0,
    /* CMD_SECURITY_SECURED removed in P-75 — the compile-time-
     * default password gate was UX friction without real security
     * (the default lived in a header, extractable from any shipped
     * binary in 30 seconds, no rotation surface, no per-dongle
     * key). Phase 2 + Phase 3 destructive operations need a real
     * auth model (per-dongle key, server-mirrored, second-factor
     * confirmation) — that is P-76. Do NOT reintroduce
     * CMD_SECURITY_SECURED without an explicit auth-model design
     * RFC.
     *
     * Compile-time guard: any handler that tries to use a removed
     * enum value fails to compile, which is the point. */
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

#endif // COMMAND_HANDLER_H
