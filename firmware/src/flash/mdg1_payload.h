#ifndef MDG1_PAYLOAD_H
#define MDG1_PAYLOAD_H

/*
 * mdg1_payload.{c,h} — wire-payload pack/unpack for the MDG1 flash path.
 *
 *   pack    : plaintext → LZRB-compress → AES-CBC-encrypt(PKCS#7-padded)
 *   unpack  : ciphertext → AES-CBC-decrypt → PKCS#7-strip → LZRB-decompress
 *
 * This module is the cryptographic core of Phase 2 flash. It is gated
 * by `FUTUNER_PHASE2_ENABLED` in `firmware/src/config/futuner_config.h`
 * (default 0 in customer firmware; the orchestrator that consumes this
 * module is gated by the same flag).
 *
 * AES is abstracted through `mdg1_aes_iface_t` so production firmware
 * can inject an mbedtls-backed implementation while host tests inject
 * a self-contained AES-128-CBC (tiny-AES, public domain). The default
 * implementation pointer is NULL; the caller MUST register an iface
 * via `mdg1_payload_set_aes_iface()` before calling pack/unpack, or
 * both return ESP_ERR_INVALID_STATE.
 *
 * Key material never lives in this module — the caller passes (key, iv)
 * by pointer per-call. The standard Bosch fixed IV
 * (MDG1_BOSCH_FIXED_IV_INIT in mdg1_payload_config.h) is exposed as a
 * convenience; callers are free to pass any 16-byte IV they sourced
 * elsewhere (per-variant override pathway, etc.).
 *
 * -------------------------------------------------------------------
 * Companion JSON schema extension (aes_keys_per_boxcode.json)
 * -------------------------------------------------------------------
 * The per-boxcode key-location authority is `secrets/aes_keys_per_boxcode.json`.
 * Per-boxcode entries that have a sourceable AES key carry an `aes_key`
 * block alongside the legacy `aes_encryption_key` field:
 *
 *   "aes_key": {
 *       "source": "bin_offset",
 *       "bin_path": "<absolute or project-relative path to an ECU dump>",
 *       "offset":   6291968,         // 0x600200 (or 0x18200 for older plain variants)
 *       "length_bytes": 16,
 *       "sha256_first8_fingerprint": "7fa117fa"
 *   }
 *
 * The runtime loader (NOT in this prompt — written in a follow-up) reads
 * `length_bytes` bytes from `bin_path` at `offset`, verifies
 * sha256(bytes)[:8] == sha256_first8_fingerprint, caches the key in
 * RAM, and never writes it back to flash. This module accepts the key
 * by pointer — the loading mechanism is the caller's responsibility.
 *
 * Reference: hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * Host builds (tests) don't link against ESP-IDF. The Makefile defines
 * MDG1_PAYLOAD_HOST_BUILD; we shim esp_err_t + the constants we use.
 * Values match ESP-IDF (esp_err.h) so firmware and host return the
 * same numbers.
 */
#ifdef MDG1_PAYLOAD_HOST_BUILD
typedef int esp_err_t;
#  ifndef ESP_OK
#    define ESP_OK                   0
#  endif
#  ifndef ESP_FAIL
#    define ESP_FAIL                -1
#  endif
#  ifndef ESP_ERR_INVALID_ARG
#    define ESP_ERR_INVALID_ARG     0x102
#  endif
#  ifndef ESP_ERR_INVALID_STATE
#    define ESP_ERR_INVALID_STATE   0x103
#  endif
#  ifndef ESP_ERR_INVALID_SIZE
#    define ESP_ERR_INVALID_SIZE    0x104
#  endif
#  ifndef ESP_ERR_INVALID_CRC
#    define ESP_ERR_INVALID_CRC     0x109
#  endif
#else
#  include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AES-128-CBC iface. Production injects an mbedtls wrapper; host tests
 * inject tiny-AES. The function-pointer interface mirrors the sbf /
 * wot_uploader inject-at-boot pattern used elsewhere in the codebase.
 *
 * Contract:
 *   - `len` MUST be a positive multiple of MDG1_PAYLOAD_AES_BLOCK_BYTES (16).
 *   - `iv` is a 16-byte buffer that the implementation MAY mutate
 *     (matches mbedtls semantics: trailing cipher block is left in IV).
 *     Callers that need to preserve their IV must pass a copy.
 *   - `in` and `out` MAY alias (in-place crypto is OK).
 *   - Return ESP_OK on success, any non-zero esp_err_t on failure.
 *   - `user_ctx` is opaque and forwarded to every invocation.
 */
typedef struct mdg1_aes_iface_s {
    esp_err_t (*encrypt_cbc)(void          *user_ctx,
                             const uint8_t *key,
                             uint8_t       *iv,
                             const uint8_t *in,
                             uint8_t       *out,
                             size_t         len);
    esp_err_t (*decrypt_cbc)(void          *user_ctx,
                             const uint8_t *key,
                             uint8_t       *iv,
                             const uint8_t *in,
                             uint8_t       *out,
                             size_t         len);
    void *user_ctx;
} mdg1_aes_iface_t;

/*
 * Register an AES iface globally. Pass NULL to clear (subsequent
 * pack/unpack calls return ESP_ERR_INVALID_STATE). The pointer is
 * captured by reference — the iface struct must outlive any pack/unpack
 * call that uses it. There is NO synchronization; the iface is expected
 * to be set once at boot before any flash feature starts.
 */
void mdg1_payload_set_aes_iface(const mdg1_aes_iface_t *iface);

/*
 * Return the currently-registered iface, or NULL if unset. Used by the
 * test harness to verify injection took effect.
 */
const mdg1_aes_iface_t *mdg1_payload_get_aes_iface(void);

/*
 * Pack: plaintext → LZRB-compress → PKCS#7-pad → AES-CBC-encrypt.
 *
 * Inputs:
 *   plaintext_in     : caller-owned buffer of plaintext_len bytes
 *   plaintext_len    : 0..MDG1_PAYLOAD_MAX_PLAINTEXT_BYTES
 *   key              : 16-byte AES-128 key (caller-owned)
 *   iv               : 16-byte initialization vector (caller-owned; NOT modified
 *                      by this function — implementation makes a working copy)
 *   out_buf          : caller-owned output buffer
 *   out_cap          : capacity of out_buf in bytes
 * Output:
 *   out_len_written  : (out param) number of bytes actually written to out_buf
 *
 * Returns:
 *   ESP_OK                  on success
 *   ESP_ERR_INVALID_ARG     on a NULL pointer that must be non-NULL
 *   ESP_ERR_INVALID_SIZE    plaintext_len exceeds MDG1_PAYLOAD_MAX_PLAINTEXT_BYTES,
 *                           or out_cap is too small for the produced ciphertext
 *   ESP_ERR_INVALID_STATE   LZRB compression failure OR AES iface unset/failure
 *
 * The output ciphertext_len is exactly:
 *   ceil((lzrb_compressed_len + 1) / 16) * 16
 * because PKCS#7 appends at least 1 byte and at most 16 (always at least
 * 1 — a full block of padding goes on when the compressed length is
 * already block-aligned). Caller should size out_cap >= plaintext_len + 32
 * for typical (compressible) inputs; for incompressible inputs LZRB can
 * inflate by ~13%, so out_cap >= plaintext_len * 9 / 8 + 32 is a safe
 * upper bound.
 */
esp_err_t mdg1_payload_pack(const uint8_t *plaintext_in,
                            size_t         plaintext_len,
                            const uint8_t *key,
                            const uint8_t *iv,
                            uint8_t       *out_buf,
                            size_t         out_cap,
                            size_t        *out_len_written);

/*
 * Unpack: ciphertext → AES-CBC-decrypt → PKCS#7-strip → LZRB-decompress.
 *
 * Inputs:
 *   ciphertext_in            : caller-owned buffer of ciphertext_len bytes
 *   ciphertext_len           : positive multiple of 16,
 *                              <= MDG1_PAYLOAD_MAX_CIPHERTEXT_BYTES
 *   key                      : 16-byte AES-128 key
 *   iv                       : 16-byte IV (NOT modified — working copy made)
 *   out_buf                  : caller-owned output buffer
 *   out_cap                  : capacity of out_buf in bytes
 *   expected_plaintext_len   : exact decompressed length expected
 *                              (RequestDownload's plaintext_size on the wire)
 * Output:
 *   out_len_written          : == expected_plaintext_len on success
 *
 * Returns:
 *   ESP_OK                   on success
 *   ESP_ERR_INVALID_ARG      NULL where non-NULL required, or
 *                            ciphertext_len % 16 != 0, or expected_plaintext_len == 0
 *   ESP_ERR_INVALID_SIZE     ciphertext_len exceeds MDG1_PAYLOAD_MAX_CIPHERTEXT_BYTES,
 *                            or out_cap < expected_plaintext_len
 *   ESP_ERR_INVALID_CRC      PKCS#7 pad validation failed (last byte 0 or > 16, or
 *                            trailing bytes don't all match the count)
 *   ESP_ERR_INVALID_STATE    LZRB decompression failure OR AES iface unset/failure
 */
esp_err_t mdg1_payload_unpack(const uint8_t *ciphertext_in,
                              size_t         ciphertext_len,
                              const uint8_t *key,
                              const uint8_t *iv,
                              uint8_t       *out_buf,
                              size_t         out_cap,
                              size_t         expected_plaintext_len,
                              size_t        *out_len_written);

#ifdef __cplusplus
}
#endif

#endif /* MDG1_PAYLOAD_H */
