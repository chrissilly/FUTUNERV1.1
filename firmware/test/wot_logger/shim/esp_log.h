/* Host shim for esp_log.h — silent by default. */
#ifndef HOST_SHIM_ESP_LOG_H
#define HOST_SHIM_ESP_LOG_H

#include <stdio.h>

#ifndef HOST_TEST_VERBOSE_LOGS
#define HOST_TEST_VERBOSE_LOGS 0
#endif

#if HOST_TEST_VERBOSE_LOGS
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#else
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#endif

#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

#endif
