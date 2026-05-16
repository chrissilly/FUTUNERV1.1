/* Phase 2 MDG1 flash writer.
 *
 * Wire-protocol reference: hw_reference/MM_Flash_Capture_Analysis.md.
 * The byte-level UDS choreography (SecurityAccess level 0x11, fingerprint
 * WriteDataByIdentifier 0xF15A before erase, EraseMemory by block ID,
 * RequestDownload with ALFID 0x31 + dataFormat 0x2A, TransferData with
 * counter reset per section, CheckMemory using plain zlib.crc32 over
 * plaintext, final CheckProgrammingDependencies 0xFF01) is sourced from
 * two MagicMotorsport captures of the dev car (4K0907557G_0003). */
#include "mdg1_flash.h"
#include "sa2_vm.h"
#include "esp_log.h"
#include <string.h>
#include <mbedtls/aes.h>

static const char *TAG = "MDG1_FLASH";

/* MDG1 SecurityAccess seed sub-function — confirmed via MM capture
 * (MM_Flash_Capture_Analysis.md §2.2). Send-key sub-function is +1. */
#define MDG1_SECURITY_LEVEL_DEFAULT 0x11

void mdg1_flash_init(mdg1_flash_ctx_t *ctx,
                     int (*uds_send)(const uds_request_t *req, uds_response_t *resp, uint32_t timeout_ms),
                     flash_progress_cb_t progress_cb)
{
    memset(ctx, 0, sizeof(mdg1_flash_ctx_t));
    ctx->uds_send = uds_send;
    ctx->progress_cb = progress_cb;
    ctx->security_level = MDG1_SECURITY_LEVEL_DEFAULT;
    mbedtls_aes_init(&ctx->aes_ctx);
}

void mdg1_flash_set_aes_key(mdg1_flash_ctx_t *ctx, const uint8_t *key, const uint8_t *iv)
{
    if (key) {
        memcpy(ctx->aes_key, key, 16);
    }
    if (iv) {
        memcpy(ctx->aes_iv, iv, 16);
    }
    mbedtls_aes_setkey_enc(&ctx->aes_ctx, ctx->aes_key, 128);
}

void mdg1_flash_set_sa2_script(mdg1_flash_ctx_t *ctx, const uint8_t *script, size_t len)
{
    ctx->sa2_script = script;
    ctx->sa2_script_len = len;
}

void mdg1_flash_set_firmware_buffer(mdg1_flash_ctx_t *ctx, uint8_t *buf, size_t size)
{
    ctx->firmware_buf = buf;
    ctx->firmware_buf_size = size;
}

static void report_progress(mdg1_flash_ctx_t *ctx, uint32_t percent, const char *msg)
{
    if (ctx->progress_cb) {
        ctx->progress_cb(percent, msg);
    }
    ESP_LOGI(TAG, "Flash progress: %lu%% - %s", (unsigned long)percent, msg);
}

static esp_err_t wait_for_response(mdg1_flash_ctx_t *ctx, uds_response_t *resp, uint32_t timeout_ms)
{
    // In a real implementation, this would wait for a response from the UDS layer.
    // For now, we assume the uds_send function handles timeout and response.
    // We'll just call uds_send and check the result.
    uds_request_t req = {0};
    // We don't have a request to send here, so we assume the caller will handle the response.
    // This function is a placeholder for the actual waiting mechanism.
    // Since we don't have the actual UDS driver, we return success and let the caller handle it.
    // In practice, this function would block until a response is received or timeout.
    // For the purpose of this code, we assume the uds_send function is synchronous and returns the response.
    // So we don't need to do anything here.
    return ESP_OK;
}

esp_err_t mdg1_flash_request_download(mdg1_flash_ctx_t *ctx, uint8_t block_id, uint32_t plaintext_size)
{
    uds_request_t req;
    uds_response_t resp;
    int ret;

    /* MDG1 RequestDownload (MM_Flash_Capture_Analysis.md §2.4.2):
     *   34 2A 31 <BID> <size_3B>
     * 0x2A = compression LZRB (2) + encryption Bosch-AES (A).
     * 0x31 = memorySize 3 bytes, memoryAddress 1 byte.
     * Plaintext size must fit in 3 bytes (max 16 MiB) — all five MDG1
     * sections do (largest is ASW1/ASW2 at 2 MiB). */
    if (plaintext_size > 0xFFFFFFu) {
        ESP_LOGE(TAG, "RequestDownload plaintext size 0x%lX exceeds 3-byte ALFID",
                 (unsigned long)plaintext_size);
        return ESP_ERR_INVALID_ARG;
    }

    req.service = 0x34;
    req.data[0] = 0x2A;
    req.data[1] = 0x31;
    req.data[2] = block_id;
    req.data[3] = (plaintext_size >> 16) & 0xFF;
    req.data[4] = (plaintext_size >> 8) & 0xFF;
    req.data[5] = plaintext_size & 0xFF;
    req.length = 6;

    ret = ctx->uds_send(&req, &resp, 1000);
    if (ret != 0) {
        ESP_LOGE(TAG, "UDS send failed for RequestDownload (BID 0x%02X): %d", block_id, ret);
        return ESP_FAIL;
    }

    if (!uds_is_positive_response(&resp, 0x34)) {
        ESP_LOGE(TAG, "RequestDownload (BID 0x%02X) failed: NRC 0x%02X", block_id, resp.nrc);
        return ESP_FAIL;
    }

    /* TODO: parse the maxNumberOfBlockLength out of the 74 <LFID> <maxLen…>
     * response and clamp per-block TransferData payloads accordingly.
     * MM observed 0x0FFF on all five MDG1 sections. */

    report_progress(ctx, 5, "RequestDownload sent");
    return ESP_OK;
}

esp_err_t mdg1_flash_transfer_data(mdg1_flash_ctx_t *ctx, uint8_t *data, uint16_t size, uint8_t *seq_num)
{
    uds_request_t req;
    uds_response_t resp;
    int ret;
    uint16_t payload_len;

    /* CONTRACT CHANGE (2026-05-12, orchestrator-prompt landing):
     *
     * This function now emits a single TransferData PCI payload as-is.
     * The caller is responsible for having ALREADY PACKED the section
     * plaintext via mdg1_payload_pack() (LZRB + AES-128-CBC + PKCS#7)
     * and for splitting the resulting ciphertext into ≤ 4093-byte
     * chunks per the ECU's maxNumberOfBlockLength.
     *
     * The previous in-flight `mbedtls_aes_crypt_cbc` on each chunk was
     * incorrect — it encrypted each chunk in isolation (no LZRB, no
     * cross-chunk CBC chaining), which would have produced ciphertext
     * the ECU couldn't decrypt. mdg1_payload_pack handles the full
     * pipeline correctly.
     *
     * The ctx->aes_key / aes_iv / aes_ctx fields are retained for
     * backward compat with callers that initialize via
     * mdg1_flash_set_aes_key(), but transfer_data() no longer reads
     * them — encryption is the orchestrator's responsibility. */
    uint8_t *to_send = data;
    uint16_t to_send_len = size;

    // Build TransferData (0x36) request
    // Format: 0x36, sequence number, data...
    payload_len = 2 + to_send_len; // service + seq_num + data
    if (payload_len > 255) {
        ESP_LOGE(TAG, "TransferData payload too large: %d", payload_len);
        return ESP_ERR_INVALID_SIZE;
    }

    req.service = 0x36;
    req.data[0] = *seq_num;
    memcpy(&req.data[1], to_send, to_send_len);
    req.length = payload_len;

    // Send request and wait for response
    ret = ctx->uds_send(&req, &resp, 1000); // 1 second timeout
    if (ret != 0) {
        ESP_LOGE(TAG, "UDS send failed for TransferData: %d", ret);
        return ESP_FAIL;
    }

    // Check response
    if (!uds_is_positive_response(&resp, 0x36)) {
        ESP_LOGE(TAG, "TransferData failed: NRC 0x%02X", resp.nrc);
        return ESP_FAIL;
    }

    // Increment sequence number (rollover at 0xFF)
    (*seq_num)++;

    // Update progress
    ctx->bytes_transferred += size;
    if (ctx->total_size > 0) {
        uint32_t percent = (ctx->bytes_transferred * 100) / ctx->total_size;
        report_progress(ctx, percent, "Transferring data");
    }

    return ESP_OK;
}

esp_err_t mdg1_flash_request_transfer_exit(mdg1_flash_ctx_t *ctx)
{
    uds_request_t req;
    uds_response_t resp;
    int ret;

    // Build RequestTransferExit (0x37) request
    // Format: 0x37, no additional data (or optional parameters, we'll send none)
    req.service = 0x37;
    req.length = 1; // only service byte
    req.data[0] = 0x00; // No parameter (optional)

    // Send request and wait for response
    ret = ctx->uds_send(&req, &resp, 1000); // 1 second timeout
    if (ret != 0) {
        ESP_LOGE(TAG, "UDS send failed for RequestTransferExit: %d", ret);
        return ESP_FAIL;
    }

    // Check response
    if (!uds_is_positive_response(&resp, 0x37)) {
        ESP_LOGE(TAG, "RequestTransferExit failed: NRC 0x%02X", resp.nrc);
        return ESP_FAIL;
    }

    report_progress(ctx, 80, "Transfer exit sent");
    return ESP_OK;
}

esp_err_t mdg1_flash_ecu_reset(mdg1_flash_ctx_t *ctx)
{
    uds_request_t req;
    uds_response_t resp;
    int ret;

    // Build ECUReset (0x11) request
    // Format: 0x11, reset type (0x01 = hard reset)
    req.service = 0x11;
    req.length = 2;
    req.data[0] = 0x01; // Hard reset
    req.data[1] = 0x00; // Reserved

    // Send request and wait for response
    ret = ctx->uds_send(&req, &resp, 1000); // 1 second timeout
    if (ret != 0) {
        ESP_LOGE(TAG, "UDS send failed for ECUReset: %d", ret);
        return ESP_FAIL;
    }

    // Check response
    if (!uds_is_positive_response(&resp, 0x11)) {
        ESP_LOGE(TAG, "ECUReset failed: NRC 0x%02X", resp.nrc);
        return ESP_FAIL;
    }

    report_progress(ctx, 90, "ECU reset sent");
    return ESP_OK;
}

esp_err_t mdg1_flash_security_access_seed(mdg1_flash_ctx_t *ctx)
{
    uds_request_t req;
    uds_response_t resp;
    int ret;

    /* SecurityAccess request seed: 27 <level>. MDG1 default is 0x11
     * (MM_Flash_Capture_Analysis.md §2.2); other ECU families override
     * ctx->security_level before calling. */
    req.service = 0x27;
    req.data[0] = ctx->security_level;
    req.length = 1;

    ret = ctx->uds_send(&req, &resp, 1000);
    if (ret != 0) {
        ESP_LOGE(TAG, "UDS send failed for SecurityAccess seed: %d", ret);
        return ESP_FAIL;
    }

    if (!uds_is_positive_response(&resp, 0x27)) {
        ESP_LOGE(TAG, "SecurityAccess seed failed: NRC 0x%02X", resp.nrc);
        return ESP_FAIL;
    }

    /* Positive response layout: 67 <level> <seed_4B>. uds_parse_response
     * places everything after the service byte into resp.data, so the
     * sub-function echo is at resp.data[0] and the 4-byte seed starts at
     * resp.data[1]. */
    if (resp.length < 1 + 4) {
        ESP_LOGE(TAG, "SecurityAccess seed response too short: %u bytes", resp.length);
        return ESP_FAIL;
    }
    memcpy(ctx->security_seed, &resp.data[1], 4);

    report_progress(ctx, 20, "Security access seed received");
    return ESP_OK;
}

esp_err_t mdg1_flash_security_access_key(mdg1_flash_ctx_t *ctx)
{
    uds_request_t req;
    uds_response_t resp;
    int ret;
    uint32_t key;

    // Compute key from seed by running the per-variant SA2 bytecode through
    // the VM (VW80126 SA2-060331-V10). The script must be set via
    // mdg1_flash_set_sa2_script() before this function is called.
    if (!ctx->sa2_script || ctx->sa2_script_len == 0) {
        ESP_LOGE(TAG, "SA2 script not set — call mdg1_flash_set_sa2_script() first");
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t seed = ((uint32_t)ctx->security_seed[0] << 24) |
                    ((uint32_t)ctx->security_seed[1] << 16) |
                    ((uint32_t)ctx->security_seed[2] << 8)  |
                    ((uint32_t)ctx->security_seed[3]);
    sa2_status_t sa2_st = sa2_run(seed, ctx->sa2_script, ctx->sa2_script_len, &key);
    if (sa2_st != SA2_OK) {
        ESP_LOGE(TAG, "SA2 VM failed: %s", sa2_status_str(sa2_st));
        return ESP_FAIL;
    }
    memcpy(ctx->security_key, (uint8_t[]){
        (uint8_t)(key >> 24), (uint8_t)(key >> 16),
        (uint8_t)(key >> 8),  (uint8_t)(key)
    }, 4);

    /* SecurityAccess send key: 27 <level+1> <key_4B>. */
    req.service = 0x27;
    req.data[0] = ctx->security_level + 1;
    req.data[1] = (key >> 24) & 0xFF;
    req.data[2] = (key >> 16) & 0xFF;
    req.data[3] = (key >> 8) & 0xFF;
    req.data[4] = key & 0xFF;
    req.length = 5;

    // Send request and wait for response
    ret = ctx->uds_send(&req, &resp, 1000); // 1 second timeout
    if (ret != 0) {
        ESP_LOGE(TAG, "UDS send failed for SecurityAccess key: %d", ret);
        return ESP_FAIL;
    }

    // Check response
    if (!uds_is_positive_response(&resp, 0x27)) {
        ESP_LOGE(TAG, "SecurityAccess key failed: NRC 0x%02X", resp.nrc);
        return ESP_FAIL;
    }

    report_progress(ctx, 30, "Security access key sent");
    return ESP_OK;
}

esp_err_t mdg1_flash_execute(mdg1_flash_ctx_t *ctx)
{
    /* NOT IMPLEMENTED for MDG1's real per-section model.
     *
     * MDG1 full-flash is five sections (block IDs 0x02, 0x03, 0x04, 0x05,
     * 0x06), each with its own Erase / RequestDownload / TransferData /
     * TransferExit / CheckMemory cycle, ending with a single
     * CheckProgrammingDependencies (0xFF01) commit before ECUReset
     * (MM_Flash_Capture_Analysis.md §2.4–§2.6). Per-section TransferData
     * payloads are LZRB-compressed then AES encrypted (dataFormat 0x2A);
     * the CheckMemory CRC is plain zlib.crc32 over plaintext. None of
     * that scaffolding exists in this module yet, and the per-section
     * plan + cipher integration is gated on the Phase 2 transport stack
     * landing (PHASE_2_PREREQUISITES.md P-08).
     *
     * Until then this entry point returns ESP_ERR_NOT_SUPPORTED so it
     * cannot be accidentally invoked from feature_manager. Use the
     * individual step functions for bench experiments. */
    (void)ctx;
    ESP_LOGE(TAG, "mdg1_flash_execute: per-section orchestration not yet implemented "
                  "(see hw_reference/MM_Flash_Capture_Analysis.md and P-08)");
    return ESP_ERR_NOT_SUPPORTED;
}

void mdg1_flash_cleanup(mdg1_flash_ctx_t *ctx)
{
    mbedtls_aes_free(&ctx->aes_ctx);
    memset(ctx, 0, sizeof(mdg1_flash_ctx_t));
}