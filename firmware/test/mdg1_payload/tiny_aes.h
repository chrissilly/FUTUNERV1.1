/*
 * tiny_aes.h — minimal AES-128-CBC for the host-side mdg1_payload tests.
 *
 * Subset of the public-domain tiny-AES-c codec (kokke, Unlicense),
 * trimmed to AES-128 + CBC only. Host-side test infrastructure ONLY —
 * do NOT include in firmware builds (firmware uses mbedtls).
 */

#ifndef TINY_AES_H
#define TINY_AES_H

#include <stddef.h>
#include <stdint.h>

#define TINY_AES_KEY_BYTES   16
#define TINY_AES_BLOCK_BYTES 16

/*
 * AES-128-CBC encrypt/decrypt `len` bytes from `in` into `out`.
 * `len` MUST be a positive multiple of 16. `in` and `out` MAY alias.
 * `iv` is mutated in place to leave the last cipher block in the
 * buffer (matches mbedtls_aes_crypt_cbc semantics).
 *
 * Returns 0 on success, non-zero on bad args.
 */
int tiny_aes_cbc_encrypt(const uint8_t *key,
                         uint8_t       *iv,
                         const uint8_t *in,
                         uint8_t       *out,
                         size_t         len);

int tiny_aes_cbc_decrypt(const uint8_t *key,
                         uint8_t       *iv,
                         const uint8_t *in,
                         uint8_t       *out,
                         size_t         len);

#endif /* TINY_AES_H */
