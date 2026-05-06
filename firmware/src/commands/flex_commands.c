#include "flex_commands.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "flex_cmds";

static esp_err_t stub_response(const char *cmd_name, char *response, size_t response_size)
{
    snprintf(response, response_size,
             "{\"command\":\"%s\",\"success\":false,\"message\":\"Not yet implemented\"}", cmd_name);
    return ESP_OK;
}

esp_err_t cmd_flex_load_scal(int fd, const char *params, char *response, size_t response_size)
{
    return stub_response("flex_load_scal", response, response_size);
}

esp_err_t cmd_flex_unload_scal(int fd, const char *params, char *response, size_t response_size)
{
    return stub_response("flex_unload_scal", response, response_size);
}

esp_err_t cmd_flex_status(int fd, const char *params, char *response, size_t response_size)
{
    return stub_response("flex_status", response, response_size);
}

esp_err_t cmd_flex_enable(int fd, const char *params, char *response, size_t response_size)
{
    return stub_response("flex_enable", response, response_size);
}

esp_err_t cmd_flex_disable(int fd, const char *params, char *response, size_t response_size)
{
    return stub_response("flex_disable", response, response_size);
}

esp_err_t cmd_flex_set_override(int fd, const char *params, char *response, size_t response_size)
{
    return stub_response("flex_set_override", response, response_size);
}
