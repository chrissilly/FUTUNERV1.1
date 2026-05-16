/*
 * mdg1_aes_mbedtls.c — production AES-128-CBC iface adapter that wraps
 * mbedtls for the mdg1_payload module.
 *
 * Firmware-only. Host builds use tiny_aes (see firmware/test/mdg1_payload/).
 *
 * Registered at orchestrator init via mdg1_payload_set_aes_iface().
 */

#ifndef MDG1_FLASH_ORCHESTRATOR_HOST_BUILD

#include "mdg1_payload.h"
#include <string.h>
#include "mbedtls/aes.h"
#include "esp_log.h"

static const char *TAG_AES = "MDG1_AES";

/* One persistent mbedtls context per call site. We keep the key
 * schedule across calls but re-key when the caller's key pointer differs
 * (cheap: AES key schedule is a few microseconds). */
typedef struct {
    mbedtls_aes_context enc_ctx;
    mbedtls_aes_context dec_ctx;
    uint8_t cached_enc_key[16];
    uint8_t cached_dec_key[16];
    bool    enc_keyed;
    bool    dec_keyed;
} mbed_ctx_t;

static mbed_ctx_t g_mbed_ctx = { 0 };

static esp_err_t mbed_encrypt(void *user_ctx,
                              const uint8_t *key,
                              uint8_t *iv,
                              const uint8_t *in,
                              uint8_t *out,
                              size_t len)
{
    (void)user_ctx;
    if (!g_mbed_ctx.enc_keyed || memcmp(g_mbed_ctx.cached_enc_key, key, 16) != 0) {
        mbedtls_aes_init(&g_mbed_ctx.enc_ctx);
        int r = mbedtls_aes_setkey_enc(&g_mbed_ctx.enc_ctx, key, 128);
        if (r != 0) {
            ESP_LOGE(TAG_AES, "mbedtls_aes_setkey_enc failed: %d", r);
            return ESP_FAIL;
        }
        memcpy(g_mbed_ctx.cached_enc_key, key, 16);
        g_mbed_ctx.enc_keyed = true;
    }
    int r = mbedtls_aes_crypt_cbc(&g_mbed_ctx.enc_ctx, MBEDTLS_AES_ENCRYPT,
                                  len, iv, in, out);
    return (r == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t mbed_decrypt(void *user_ctx,
                              const uint8_t *key,
                              uint8_t *iv,
                              const uint8_t *in,
                              uint8_t *out,
                              size_t len)
{
    (void)user_ctx;
    if (!g_mbed_ctx.dec_keyed || memcmp(g_mbed_ctx.cached_dec_key, key, 16) != 0) {
        mbedtls_aes_init(&g_mbed_ctx.dec_ctx);
        int r = mbedtls_aes_setkey_dec(&g_mbed_ctx.dec_ctx, key, 128);
        if (r != 0) {
            ESP_LOGE(TAG_AES, "mbedtls_aes_setkey_dec failed: %d", r);
            return ESP_FAIL;
        }
        memcpy(g_mbed_ctx.cached_dec_key, key, 16);
        g_mbed_ctx.dec_keyed = true;
    }
    int r = mbedtls_aes_crypt_cbc(&g_mbed_ctx.dec_ctx, MBEDTLS_AES_DECRYPT,
                                  len, iv, in, out);
    return (r == 0) ? ESP_OK : ESP_FAIL;
}

static const mdg1_aes_iface_t MBED_IFACE = {
    .encrypt_cbc = mbed_encrypt,
    .decrypt_cbc = mbed_decrypt,
    .user_ctx    = NULL,
};

void mdg1_aes_mbedtls_register(void)
{
    mdg1_payload_set_aes_iface(&MBED_IFACE);
}

#else  /* HOST build excludes this TU */
typedef int mdg1_aes_mbedtls_host_build_excluded_t;
#endif
