#ifndef WIFI_COMMANDS_H
#define WIFI_COMMANDS_H

#include "command_handler.h"

/*
 * wifi_commands — operator surface for the WiFi mode-intent + creds APIs
 * in wifi_ap.{c,h}. Wired into commands/commands.c by
 * `cmd_wifi_sta_set`, `cmd_wifi_mode`, `cmd_wifi_clear`, `cmd_wifi_status2`.
 *
 * `cmd_wifi_status2` replaces the legacy `cmd_wifi_status` in
 * system_commands.c at the registry level; the legacy handler is left
 * intact (and exported) for one release of overlap. See P-24 in
 * docs/PHASE_2_PREREQUISITES.md.
 */

esp_err_t cmd_wifi_sta_set (int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_wifi_mode    (int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_wifi_clear   (int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_wifi_status2 (int fd, const char *params, char *response, size_t response_size);

#endif /* WIFI_COMMANDS_H */
