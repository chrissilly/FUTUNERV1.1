#ifndef VIN_PAIR_COMMANDS_H
#define VIN_PAIR_COMMANDS_H

#include <stddef.h>
#include "esp_err.h"

/*
 * vin_pair_commands — WS / serial command handlers for VIN pairing
 * and license maintenance.
 *
 * Three commands:
 *   vin_pair_now    UNSECURED (first sync after boot can come from
 *                   the AP captive page; SECURED gating happens at
 *                   the cloud layer via Bearer token)
 *   set_auth_token  SECURED   (admin pre-enrollment install)
 *   license_status  UNSECURED (read-only status snapshot)
 */

esp_err_t cmd_vin_pair_now   (int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_set_auth_token (int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_license_status (int fd, const char *params, char *response, size_t response_size);

#endif /* VIN_PAIR_COMMANDS_H */
