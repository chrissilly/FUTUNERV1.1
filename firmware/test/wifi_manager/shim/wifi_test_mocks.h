/* wifi_test_mocks.h — observable side of the host_shim.c mocks. The
 * test file (test_wifi_manager.c) reads the spy counters and calls the
 * mock-state setters directly. */
#ifndef WIFI_TEST_MOCKS_H
#define WIFI_TEST_MOCKS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* esp_wifi_* spy counters + captured-arg buffers. */
extern int  g_esp_wifi_disconnect_count;
extern int  g_esp_wifi_connect_count;
extern int  g_esp_wifi_set_config_count;
extern char g_last_set_config_ssid[33];
extern char g_last_set_config_password[65];

void wifi_test_reset_spies(void);

/* In-memory NVS mock controls. */
void        nvs_mock_clear(void);
const char *nvs_mock_get(const char *key);
void        nvs_mock_set(const char *key, const char *value);

/* feature_manager mock controls. id is feature_id_t cast to int to avoid
 * a circular include. */
void wifi_test_set_feature_active(int id, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_TEST_MOCKS_H */
