#ifndef HOST_SHIM_WS_SERVER_H
#define HOST_SHIM_WS_SERVER_H

#include "esp_err.h"
#include <stdbool.h>

bool      ws_server_is_running(void);
esp_err_t ws_server_start(void);
esp_err_t ws_server_stop(void);

#endif /* HOST_SHIM_WS_SERVER_H */
