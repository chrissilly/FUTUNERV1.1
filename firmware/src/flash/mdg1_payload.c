/*
 * mdg1_payload.c — implementation. See mdg1_payload.h for the API contract
 * and the JSON schema extension notes. Algorithm validated end-to-end
 * against the live RS7 capture; see
 * hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md.
 */

#include "mdg1_payload.h"
#include "mdg1_payload_config.h"
#include "lzrb.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Module-private state                                               */
/* ------------------------------------------------------------------ */

static const mdg1_aes_iface_t *g_aes_iface = NULL;

void mdg1_payload_set_aes_iface(const mdg1_aes_iface_t *iface)
{
    g_aes_iface = iface;
}

const mdg1_aes_iface_t *mdg1_payload_get_aes_iface(void)
{
    return g_aes_iface;
}

/* ------------------------------------------------------------------ */
/* PKCS#7 helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Compute the PKCS#7 pad-byte count for a given pre-pad length.
 * Always returns 1..MDG1_PAYLOAD_AES_BLOCK_BYTES (never 0 — a full
 * block of padding is appended when the input is already block-aligned).
 */
static size_t pkcs7_pad_count(size_t pre_pad_len)
{
    size_t rem = pre_pad_len % MDG1_PAYLOAD_AES_BLOCK_BYTES;
    return MDG1_PAYLOAD_AES_BLOCK_BYTES - rem;
}

/*
 * Validate PKCS#7 padding at the tail of `buf` and return the pad count
 * on success or 0 on any malformedness. Caller must subtract the
 * returned count to get the unpadded length.
 *
 * Constant-time is not a goal here — pad validation in this codepath
 * runs after AES decrypt, and a bogus pad indicates a wrong key or
 * corrupted ciphertext, which the caller will report up the stack via
 * ESP_ERR_INVALID_CRC. There is no padding-oracle attack surface
 * because the bootloader never round-trips error info back to a
 * network attacker.
 */
static size_t pkcs7_validate_and_count(const uint8_t *buf, size_t buf_len)
{
    if (buf_len == 0 || (buf_len % MDG1_PAYLOAD_AES_BLOCK_BYTES) != 0) {
        return 0;
    }
    uint8_t last = buf[buf_len - 1];
    if (last < MDG1_PAYLOAD_PKCS7_MIN_PAD || last > MDG1_PAYLOAD_PKCS7_MAX_PAD) {
        return 0;
    }
    if ((size_t)last > buf_len) {
        return 0;
    }
    for (size_t i = buf_len - last; i < buf_len; i++) {
        if (buf[i] != last) {
            return 0;
        }
    }
    return last;
}

/* ------------------------------------------------------------------ */
/* Pack                                                                */
/* ------------------------------------------------------------------ */

esp_err_t mdg1_payload_pack(const uint8_t *plaintext_in,
                            size_t         plaintext_len,
                            const uint8_t *key,
                            const uint8_t *iv,
                            uint8_t       *out_buf,
                            size_t         out_cap,
                            size_t        *out_len_written)
{
    if (out_len_written) {
        *out_len_written = 0;
    }
    /* NULL-arg check. plaintext_in MAY be NULL when plaintext_len == 0
     * (the empty-input edge case in test_pack_unpack_roundtrip_empty);
     * everything else must be non-NULL. */
    if (!key || !iv || !out_buf || !out_len_written) {
        return ESP_ERR_INVALID_ARG;
    }
    if (plaintext_len > 0 && !plaintext_in) {
        return ESP_ERR_INVALID_ARG;
    }
    if (plaintext_len > MDG1_PAYLOAD_MAX_PLAINTEXT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!g_aes_iface || !g_aes_iface->encrypt_cbc) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Worst-case LZRB output: input + input/HEADROOM_DIVISOR +
     * HEADROOM_FIXED_BYTES (see mdg1_payload_config.h for the rationale).
     * Then we append at most MDG1_PAYLOAD_AES_BLOCK_BYTES of padding.
     * The out_cap check below catches under-sized buffers. */
    size_t cmp_cap = plaintext_len
                   + (plaintext_len / MDG1_PAYLOAD_LZRB_HEADROOM_DIVISOR)
                   + MDG1_PAYLOAD_LZRB_HEADROOM_FIXED_BYTES;
    if (cmp_cap > out_cap) {
        cmp_cap = out_cap;
    }

    /* Step 1: LZRB compress directly into out_buf. */
    size_t cmp_len = 0;
    lzrb_status_t s = lzrb_compress(plaintext_in, plaintext_len,
                                    out_buf, cmp_cap, &cmp_len);
    if (s != LZRB_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Step 2: PKCS#7 pad — appended in plaintext-pre-encrypt order
     * (i.e. on the compressed bytes, BEFORE AES). This is the Bosch
     * convention (RL_MDG1.cpp lzrb_comp() lines 324-329). */
    size_t pad = pkcs7_pad_count(cmp_len);
    size_t padded_len = cmp_len + pad;
    if (padded_len > out_cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < pad; i++) {
        out_buf[cmp_len + i] = (uint8_t)pad;
    }

    /* Step 3: AES-CBC encrypt in place. Working copy of IV so the
     * caller's IV is not clobbered (matches API contract). */
    uint8_t iv_work[MDG1_PAYLOAD_AES_BLOCK_BYTES];
    memcpy(iv_work, iv, MDG1_PAYLOAD_AES_BLOCK_BYTES);
    esp_err_t err = g_aes_iface->encrypt_cbc(g_aes_iface->user_ctx,
                                             key, iv_work,
                                             out_buf, out_buf, padded_len);
    /* Zeroize the working IV — defensive. */
    memset(iv_work, 0, sizeof(iv_work));
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_len_written = padded_len;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Unpack                                                              */
/* ------------------------------------------------------------------ */

esp_err_t mdg1_payload_unpack(const uint8_t *ciphertext_in,
                              size_t         ciphertext_len,
                              const uint8_t *key,
                              const uint8_t *iv,
                              uint8_t       *out_buf,
                              size_t         out_cap,
                              size_t         expected_plaintext_len,
                              size_t        *out_len_written)
{
    if (out_len_written) {
        *out_len_written = 0;
    }
    if (!ciphertext_in || !key || !iv || !out_buf || !out_len_written) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ciphertext_len == 0 || (ciphertext_len % MDG1_PAYLOAD_AES_BLOCK_BYTES) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ciphertext_len > MDG1_PAYLOAD_MAX_CIPHERTEXT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (expected_plaintext_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (expected_plaintext_len > out_cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!g_aes_iface || !g_aes_iface->decrypt_cbc) {
        return ESP_ERR_INVALID_STATE;
    }

    /* We need a scratch buffer to hold the AES-decrypted (still LZRB-
     * compressed, still PKCS#7-padded) bytes before LZRB-decompressing
     * into out_buf. Allocate on the stack only for tiny inputs;
     * otherwise we'd blow the ESP32-S3 task stack. The largest
     * ciphertext we care about is ~1 MB (ASW1/2 sections compressed).
     * Use a heap allocation; firmware has plenty of heap on S3.
     *
     * Avoid stdlib in firmware where reasonable, but malloc is already
     * used elsewhere in this build, so this is fine. */
    uint8_t *scratch = NULL;
    /* For small payloads (typical TransferData chunks <= 4093 B
     * post-LZRB) we can re-use out_buf as the scratch since
     * ciphertext_len <= out_cap is checked. But out_cap is sized for
     * EXPANDED plaintext, not compressed, so out_cap < ciphertext_len
     * is possible when compression ratio is poor.
     *
     * Simplest correct path: always heap-allocate scratch. */
    extern void *malloc(size_t);
    extern void  free(void *);

    scratch = (uint8_t *)malloc(ciphertext_len);
    if (!scratch) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Step 1: AES-CBC decrypt into scratch. Working IV copy. */
    uint8_t iv_work[MDG1_PAYLOAD_AES_BLOCK_BYTES];
    memcpy(iv_work, iv, MDG1_PAYLOAD_AES_BLOCK_BYTES);
    esp_err_t err = g_aes_iface->decrypt_cbc(g_aes_iface->user_ctx,
                                             key, iv_work,
                                             ciphertext_in, scratch,
                                             ciphertext_len);
    memset(iv_work, 0, sizeof(iv_work));
    if (err != ESP_OK) {
        free(scratch);
        return ESP_ERR_INVALID_STATE;
    }

    /* Step 2: PKCS#7 validate + strip. */
    size_t pad = pkcs7_validate_and_count(scratch, ciphertext_len);
    if (pad == 0) {
        free(scratch);
        return ESP_ERR_INVALID_CRC;
    }
    size_t lzrb_len = ciphertext_len - pad;

    /* Step 3: LZRB decompress into caller's buffer. */
    size_t produced = 0;
    lzrb_status_t s = lzrb_decompress(scratch, lzrb_len,
                                      out_buf, out_cap,
                                      expected_plaintext_len, &produced);
    /* Zeroize the LZRB scratch — it held decrypted plaintext (compressed
     * form, but still proprietary). Defensive cleanup. */
    memset(scratch, 0, ciphertext_len);
    free(scratch);

    if (s != LZRB_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (produced != expected_plaintext_len) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_len_written = produced;
    return ESP_OK;
}
