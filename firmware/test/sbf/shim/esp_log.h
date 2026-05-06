/* Host shim for esp_log.h — silent by default. */
#ifndef HOST_SHIM_ESP_LOG_H
#define HOST_SHIM_ESP_LOG_H
#include <stdio.h>
#ifndef HOST_TEST_VERBOSE_LOGS
#define HOST_TEST_VERBOSE_LOGS 0
#endif
#if HOST_TEST_VERBOSE_LOGS
#define ESP_LOGI(t,f,...) fprintf(stderr,"I (%s) " f "\n",t,##__VA_ARGS__)
#define ESP_LOGW(t,f,...) fprintf(stderr,"W (%s) " f "\n",t,##__VA_ARGS__)
#define ESP_LOGE(t,f,...) fprintf(stderr,"E (%s) " f "\n",t,##__VA_ARGS__)
#else
#define ESP_LOGI(t,f,...) ((void)0)
#define ESP_LOGW(t,f,...) ((void)0)
#define ESP_LOGE(t,f,...) ((void)0)
#endif
#define ESP_LOGD(t,f,...) ((void)0)
#define ESP_LOGV(t,f,...) ((void)0)
#endif
