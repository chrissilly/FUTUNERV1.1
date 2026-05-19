#include "ws_server.h"
#include "commands/command_handler.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <unistd.h>

static const char *TAG = "WS_SERVER";

#define MAX_WS_CLIENTS 4

static httpd_handle_t server = NULL;
static ws_message_handler_t message_handler = NULL;

static int active_clients[MAX_WS_CLIENTS];
static int client_count = 0;

/* M2 fix: mutex for thread-safe client list access */
static SemaphoreHandle_t s_client_mutex = NULL;

/* Embedded control panel HTML (linked by EMBED_FILES in CMakeLists.txt) */
extern const uint8_t futuner_control_panel_html_start[] asm("_binary_futuner_control_panel_html_start");
extern const uint8_t futuner_control_panel_html_end[]   asm("_binary_futuner_control_panel_html_end");

static void register_client(int fd) {
    if (s_client_mutex) xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (active_clients[i] == -1) {
            active_clients[i] = fd;
            client_count++;
            ESP_LOGI(TAG, "Client %d connected (total: %d)", fd, client_count);
            if (s_client_mutex) xSemaphoreGive(s_client_mutex);
            return;
        }
    }
    if (s_client_mutex) xSemaphoreGive(s_client_mutex);
}

static void unregister_client(int fd) {
    /* H3 fix: clear auth state so a new client on the same fd doesn't inherit it */
    command_handler_clear_authentication(fd);
    if (s_client_mutex) xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (active_clients[i] == fd) {
            active_clients[i] = -1;
            client_count--;
            ESP_LOGI(TAG, "Client %d disconnected (total: %d)", fd, client_count);
            if (s_client_mutex) xSemaphoreGive(s_client_mutex);
            return;
        }
    }
    if (s_client_mutex) xSemaphoreGive(s_client_mutex);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake");
        register_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }

    if (ws_pkt.len == 0) {
        ESP_LOGW(TAG, "Received empty frame");
        return ESP_OK;
    }

    if (ws_pkt.len > WS_MAX_PAYLOAD_SIZE) {
        ESP_LOGE(TAG, "Payload too large: %d bytes", ws_pkt.len);
        return ESP_FAIL;
    }

    uint8_t *buf = calloc(1, ws_pkt.len + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for frame");
        return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
        free(buf);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        buf[ws_pkt.len] = '\0';
        ESP_LOGD(TAG, "Received message: %s", buf);

        if (message_handler != NULL) {
            message_handler(httpd_req_to_sockfd(req), (char *)buf, ws_pkt.len);
        }
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "Client requested close");
        unregister_client(httpd_req_to_sockfd(req));
    }

    free(buf);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    size_t html_len = futuner_control_panel_html_end - futuner_control_panel_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)futuner_control_panel_html_start, html_len);
}

static const httpd_uri_t root_uri = {
    .uri        = "/",
    .method     = HTTP_GET,
    .handler    = root_get_handler,
    .user_ctx   = NULL,
    .is_websocket = false
};

static const httpd_uri_t ws_uri = {
    .uri        = "/ws",
    .method     = HTTP_GET,
    .handler    = ws_handler,
    .user_ctx   = NULL,
    .is_websocket = true
};

esp_err_t ws_server_init(void) {
    if (server != NULL) {
        ESP_LOGW(TAG, "WebSocket server already initialized");
        return ESP_OK;
    }

    /* M2 fix: create client list mutex */
    if (!s_client_mutex) {
        s_client_mutex = xSemaphoreCreateMutex();
    }

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        active_clients[i] = -1;
    }
    client_count = 0;

    ESP_LOGI(TAG, "WebSocket server initialized");
    return ESP_OK;
}

static void ws_close_handler(httpd_handle_t hd, int sockfd) {
    unregister_client(sockfd);
    close(sockfd);
}

esp_err_t ws_server_start(void) {
    if (server != NULL) {
        ESP_LOGW(TAG, "WebSocket server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.close_fn = ws_close_handler;

    ESP_LOGI(TAG, "Starting WebSocket server on port %d", config.server_port);

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(server, &root_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register root URI handler: %s", esp_err_to_name(ret));
        httpd_stop(server);
        server = NULL;
        return ret;
    }

    ret = httpd_register_uri_handler(server, &ws_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WS URI handler: %s", esp_err_to_name(ret));
        httpd_stop(server);
        server = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "WebSocket server started successfully (serving control panel at /)");
    return ESP_OK;
}

esp_err_t ws_server_stop(void) {
    if (server == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(server);
    server = NULL;
    client_count = 0;

    ESP_LOGI(TAG, "WebSocket server stopped");
    return ret;
}

bool ws_server_is_running(void) {
    return (server != NULL);
}

uint8_t ws_server_get_client_count(void) {
    return client_count;
}

esp_err_t ws_server_send_text(int fd, const char *message) {
    if (server == NULL || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)message;
    ws_pkt.len = strlen(message);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    return httpd_ws_send_frame_async(server, fd, &ws_pkt);
}

esp_err_t ws_server_broadcast_text(const char *message) {
    if (server == NULL || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (active_clients[i] != -1) {
            esp_err_t send_ret = ws_server_send_text(active_clients[i], message);
            if (send_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send to client %d: %s", 
                         active_clients[i], esp_err_to_name(send_ret));
                ret = send_ret;
            }
        }
    }

    return ret;
}

void ws_server_set_message_handler(ws_message_handler_t handler) {
    message_handler = handler;
}
