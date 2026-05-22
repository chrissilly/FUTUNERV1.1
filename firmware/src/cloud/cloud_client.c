#include "cloud_client.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "CLOUD_CLIENT";

esp_http_client_handle_t cloud_client_https_init(const char                *url,
                                                  esp_http_client_method_t   method,
                                                  int                        timeout_ms)
{
    /* Canonical TLS config: full Mozilla CA bundle attached via
     * esp_crt_bundle_attach. CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL
     * is already y in sdkconfig (200 CA certs). Any cloud-touching
     * HTTPS client in the dongle MUST flow through this factory; the
     * P-49 audit gate greps for `.crt_bundle_attach` outside this
     * file and treats hits as regressions. */
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = method,
        .timeout_ms        = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t handle = esp_http_client_init(&cfg);
    if (handle == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed (url=%s, method=%d)",
                 url != NULL ? url : "(null)", (int)method);
    }
    return handle;
}
