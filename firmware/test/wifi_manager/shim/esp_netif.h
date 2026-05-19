#ifndef HOST_SHIM_ESP_NETIF_H
#define HOST_SHIM_ESP_NETIF_H

#include "esp_err.h"
#include <stdint.h>

typedef struct esp_netif_obj esp_netif_t;

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t gw;
    esp_ip4_addr_t netmask;
} esp_netif_ip_info_t;

typedef struct {
    esp_netif_ip_info_t ip_info;
} ip_event_got_ip_t;

esp_err_t    esp_netif_init               (void);
esp_netif_t *esp_netif_create_default_wifi_ap (void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_err_t    esp_netif_str_to_ip4          (const char *src, esp_ip4_addr_t *dst);
esp_err_t    esp_netif_dhcps_stop          (esp_netif_t *netif);
esp_err_t    esp_netif_dhcps_start         (esp_netif_t *netif);
esp_err_t    esp_netif_set_ip_info         (esp_netif_t *netif, const esp_netif_ip_info_t *info);

#endif /* HOST_SHIM_ESP_NETIF_H */
