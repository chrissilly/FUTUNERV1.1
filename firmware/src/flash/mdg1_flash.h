#ifndef MDG1_FLASH_H
#define MDG1_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include "uds_protocol.h"
#include "mdg1_crc.h"
#include "esp_err.h"
#include "mbedtls/aes.h"

/* Flash progress callback type */
typedef void (*flash_progress_cb_t)(uint32_t progress_percent, const char *msg);

/* Flash context */
typedef struct {
    /* UDS communication - to be implemented by caller */
    int (*uds_send)(const uds_request_t *req, uds_response_t *resp, uint32_t timeout_ms);
    
    /* Progress reporting */
    flash_progress_cb_t progress_cb;
    
    /* Security access. security_level is the UDS 0x27 sub-function used
     * to request the seed (the send-key sub-function is +1). MDG1 uses
     * 0x11 / 0x12 per the MagicMotorsport reference capture (see
     * hw_reference/MM_Flash_Capture_Analysis.md §2.2). Defaults to 0x11
     * in mdg1_flash_init(); override before execute() for other families. */
    uint8_t security_level;
    uint8_t security_seed[4];
    uint8_t security_key[4];

    /* Per-variant SA2 bytecode (VW80126 SA2-060331-V10). Set via
     * mdg1_flash_set_sa2_script() before calling mdg1_flash_execute() —
     * the VM runs over the seed to produce the key for UDS 0x27 <level+1>. */
    const uint8_t *sa2_script;
    size_t         sa2_script_len;

    /* Encryption */
    mbedtls_aes_context aes_ctx;
    uint8_t aes_key[16];  /* AES-128 key */
    uint8_t aes_iv[16];   /* Initialization vector */
    
    /* Flash state */
    uint32_t current_address;
    uint32_t total_size;
    uint32_t bytes_transferred;
    
    /* Firmware buffer (for checksum fixing) */
    uint8_t *firmware_buf;
    size_t firmware_buf_size;
} mdg1_flash_ctx_t;

/* Initialize flash context */
void mdg1_flash_init(mdg1_flash_ctx_t *ctx,
                     int (*uds_send)(const uds_request_t *req, uds_response_t *resp, uint32_t timeout_ms),
                     flash_progress_cb_t progress_cb);

/* Set AES key and IV for encryption/decryption */
void mdg1_flash_set_aes_key(mdg1_flash_ctx_t *ctx, const uint8_t *key, const uint8_t *iv);

/* Set the per-variant SA2 bytecode used to derive the security-access key
 * from the seed. The buffer must remain valid for the lifetime of ctx. */
void mdg1_flash_set_sa2_script(mdg1_flash_ctx_t *ctx, const uint8_t *script, size_t len);

/* Set firmware buffer for checksum fixing */
void mdg1_flash_set_firmware_buffer(mdg1_flash_ctx_t *ctx, uint8_t *buf, size_t size);

/* Execute full flash sequence */
esp_err_t mdg1_flash_execute(mdg1_flash_ctx_t *ctx);

/* Individual step functions (for advanced usage).
 *
 * mdg1_flash_request_download: MDG1 addresses sections by 1-byte logical
 * block ID (0x02 ASW1, 0x03 ASW2, 0x04 ASW3, 0x05 CBOOT, 0x06 CAL — see
 * hw_reference/MM_Flash_Capture_Analysis.md §2.5), not by a physical
 * 32-bit address. plaintext_size is the uncompressed section size and
 * goes on the wire as a 3-byte big-endian value (ALFID 0x31). The
 * dataFormatIdentifier sent is 0x2A (LZRB compression + Bosch AES). */
esp_err_t mdg1_flash_request_download(mdg1_flash_ctx_t *ctx, uint8_t block_id, uint32_t plaintext_size);
esp_err_t mdg1_flash_transfer_data(mdg1_flash_ctx_t *ctx, uint8_t *data, uint16_t size, uint8_t *seq_num);
esp_err_t mdg1_flash_request_transfer_exit(mdg1_flash_ctx_t *ctx);
esp_err_t mdg1_flash_ecu_reset(mdg1_flash_ctx_t *ctx);
esp_err_t mdg1_flash_security_access_seed(mdg1_flash_ctx_t *ctx);
esp_err_t mdg1_flash_security_access_key(mdg1_flash_ctx_t *ctx);

/* Cleanup */
void mdg1_flash_cleanup(mdg1_flash_ctx_t *ctx);

#endif /* MDG1_FLASH_H */