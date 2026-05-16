#ifndef MDG1_VARIANT_MANIFEST_H
#define MDG1_VARIANT_MANIFEST_H

/*
 * mdg1_variant_manifest.{c,h} — variant-manifest loader for the MDG1
 * flash orchestrator.
 *
 * The manifest authority on disk is
 *     secrets/mdg1_variant_manifest.json  (per-variant SA2 + flash sections)
 *     secrets/aes_keys_per_boxcode.json   (per-boxcode AES key offset metadata)
 *
 * At orchestrator init, the loader:
 *   1. Reads the two JSON blobs from filesystem (host) or SPIFFS (firmware).
 *   2. Looks up the active boxcode in boxcode_index → variant family.
 *   3. Extracts the variant's SA2 script + 5 flash sections + bin source.
 *   4. Extracts the AES key location from aes_keys_per_boxcode.json.
 *   5. Opens the ECU bin at the recorded path/offset, reads the 16 key
 *      bytes, computes SHA-256 and compares first 8 hex chars to the
 *      stored fingerprint. Bails ESP_ERR_INVALID_CRC on mismatch.
 *   6. Caches the resulting variant_t in RAM. The key bytes live ONLY
 *      in this RAM cache — never logged, never persisted.
 *
 * Per spec Q6: load once at init. Per Hard Rule 5: key bytes never
 * leave the cache; the cache zeros itself on free.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
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
#  ifndef ESP_ERR_NOT_FOUND
#    define ESP_ERR_NOT_FOUND       0x105
#  endif
#  ifndef ESP_ERR_NOT_SUPPORTED
#    define ESP_ERR_NOT_SUPPORTED   0x106
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

#define MDG1_VARIANT_BOXCODE_MAX        32u
#define MDG1_VARIANT_NAME_MAX           48u
#define MDG1_VARIANT_SA2_MAX_BYTES      64u
#define MDG1_VARIANT_BIN_PATH_MAX       256u
#define MDG1_VARIANT_SECTION_NAME_MAX   16u
#define MDG1_VARIANT_MAX_SECTIONS       5u

/*
 * One flash section. Mirrors the JSON entry in
 * variants["<variant>"].flash_sections_by_boxcode["<boxcode>"][N].
 */
typedef struct {
    uint8_t  block_id;          /* MDG1 1-byte logical block ID (0x02..0x06) */
    uint32_t plaintext_size;    /* uncompressed size in bytes */
    uint32_t ecu_address;       /* TriCore physical address (informational) */
    uint32_t expected_crc32;    /* zlib.crc32 of the plaintext slice */
    uint32_t file_offset;       /* offset in the source bin where plaintext begins */
    uint32_t file_length;       /* bytes of plaintext to slice from the source bin */
    char     name[MDG1_VARIANT_SECTION_NAME_MAX]; /* "ASW1", "CAL", … */
} mdg1_variant_section_t;

/*
 * Variant descriptor. Populated by mdg1_variant_manifest_load().
 * The 16 AES key bytes live in `aes_key`; this struct is the only
 * place they exist in RAM during a flash run.
 */
typedef struct {
    char    boxcode[MDG1_VARIANT_BOXCODE_MAX];          /* "4K0907557G__0003" */
    char    variant_name[MDG1_VARIANT_NAME_MAX];        /* "MG1 CS002IFX RS"  */
    uint8_t sa2_script[MDG1_VARIANT_SA2_MAX_BYTES];
    size_t  sa2_script_len;

    mdg1_variant_section_t  sections[MDG1_VARIANT_MAX_SECTIONS];
    size_t                  section_count;

    char    plaintext_bin_path[MDG1_VARIANT_BIN_PATH_MAX];
    uint8_t aes_key[16];        /* validated against stored sha256[:8] fingerprint */
    uint8_t aes_iv[16];         /* fixed Bosch IV from mdg1_payload_config.h */
} mdg1_variant_t;

/*
 * Load the variant for `boxcode` from the two manifest JSON files.
 *
 * Parameters:
 *   manifest_json_path   — path to mdg1_variant_manifest.json
 *   keys_json_path       — path to aes_keys_per_boxcode.json
 *   boxcode              — e.g. "4K0907557G__0003" (with double-underscore
 *                          to match aes_keys_per_boxcode.json; the loader
 *                          internally converts to space-form when querying
 *                          mdg1_variant_manifest.json's boxcode_index)
 *   out                  — populated on ESP_OK
 *
 * Returns:
 *   ESP_OK                    on success
 *   ESP_ERR_INVALID_ARG       NULL pointer
 *   ESP_ERR_NOT_FOUND         boxcode missing from one of the manifests
 *   ESP_ERR_INVALID_STATE     JSON malformed or file I/O failed
 *   ESP_ERR_INVALID_CRC       AES key SHA-256 fingerprint mismatch — bail
 *   ESP_ERR_INVALID_SIZE      section count exceeds MDG1_VARIANT_MAX_SECTIONS
 *                             or SA2 script exceeds MDG1_VARIANT_SA2_MAX_BYTES
 */
esp_err_t mdg1_variant_manifest_load(const char *manifest_json_path,
                                     const char *keys_json_path,
                                     const char *boxcode,
                                     mdg1_variant_t *out);

/*
 * Zeroize the cached key bytes in `v`. Call when the orchestrator is
 * done with the variant (end of flash, error abort, etc.). Idempotent.
 */
void mdg1_variant_manifest_clear(mdg1_variant_t *v);

#ifdef __cplusplus
}
#endif

#endif /* MDG1_VARIANT_MANIFEST_H */
