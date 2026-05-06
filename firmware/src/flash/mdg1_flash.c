#include "mdg1_flash.h"
#include "esp_log.h"
#include <string.h>
#include <mbedtls/aes.h>

static const char *TAG = "MDG1_FLASH";

void mdg1_flash_init(mdg1_flash_ctx_t *ctx,
                     int (*uds_send)(const uds_request_t *req, uds_response_t *resp, uint32_t timeout_ms),
                     flash_progress_cb_t progress_cb)
{
    memset(ctx, 0, sizeof(mdg1_flash_ctx_t));
    ctx->uds_send = uds_send;
    ctx->progress_cb = progress_cb;
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

esp_err_t mdg1_flash_request_download(mdg1_flash_ctx_t *ctx, uint32_t address, uint32_t size)
{
    uds_request_t req;
    uds_response_t resp;
    uint8_t data[12];
    int ret;

    // Build RequestDownload (0x34) request
    // Format: 0x34, address format byte, size format byte, address (4 bytes), size (4 bytes)
    // Address format: 0x44 (4-byte address, little-endian? We'll use big-endian as per UDS)
    // Size format: 0x44 (4-byte size)
    data[0] = 0x34; // RequestDownload service
    data[1] = 0x44; // Address format: 4-byte, size format: 4-byte (both in one byte: high nibble=address format, low nibble=size format)
    data[2] = (address >> 24) & 0xFF;
    data[3] = (address >> 16) & 0xFF;
    data[4] = (address >> 8) & 0xFF;
    data[5] = address & 0xFF;
    data[6] = (size >> 24) & 0xFF;
    data[7] = (size >> 16) & 0xFF;
    data[8] = (size >> 8) & 0xFF;
    data[9] = size & 0xFF;

    req.service = data[0];
    req.length = 10; // service + 9 data bytes
    memcpy(req.data, data, req.length);

    // Send request and wait for response
    ret = ctx->uds_send(&req, &resp, 1000); // 1 second timeout
    if (ret != 0) {
        ESP_LOGE(TAG, "UDS send failed for RequestDownload: %d", ret);
        return ESP_FAIL;
    }

    // Check response
    if (!uds_is_positive_response(&resp, 0x34)) {
        ESP_LOGE(TAG, "RequestDownload failed: NRC 0x%02X", resp.nrc);
        return ESP_FAIL;
    }

    // Extract max block length from response (if needed)
    // For simplicity, we assume the ECU accepts our block size.

    report_progress(ctx, 5, "RequestDownload sent");
    return ESP_OK;
}

esp_err_t mdg1_flash_transfer_data(mdg1_flash_ctx_t *ctx, uint8_t *data, uint16_t size, uint8_t *seq_num)
{
    uds_request_t req;
    uds_response_t resp;
    int ret;
    uint8_t *payload;
    uint16_t payload_len;

    // We'll encrypt the data if AES key is set
    uint8_t encrypted_data[256]; // Max UDS payload is 255, but we'll use 256 for alignment
    uint8_t *to_send = data;
    uint16_t to_send_len = size;

    if (ctx->aes_key[0] != 0) { // Simple check if key is set
        // We need to encrypt the data in CBC mode.
        // We'll use mbedtls for AES-128-CBC.
        // Note: We assume the data size is a multiple of 16 bytes (AES block size).
        // If not, we need to pad. For simplicity, we'll require the caller to provide block-aligned data.
        // In a real implementation, we would handle padding.
        if (size % 16 != 0) {
            ESP_LOGE(TAG, "Data size not multiple of 16 for AES encryption");
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(encrypted_data, data, size);
        mbedtls_aes_crypt_cbc(&ctx->aes_ctx, MBEDTLS_AES_ENCRYPT, size, ctx->aes_iv, encrypted_data, encrypted_data);
        to_send = encrypted_data;
        to_send_len = size;
    }

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

    // Build SecurityAccess (0x27) request for seed
    // Format: 0x27, subfunction = 0x01 (request seed)
    req.service = 0x27;
    req.length = 2;
    req.data[0] = 0x01; // Request seed
    req.data[1] = 0x00; // Security level (we'll use 0 for now, but note: we need to know the correct level)

    // Send request and wait for response
    ret = ctx->uds_send(&req, &resp, 1000); // 1 second timeout
    if (ret != 0) {
        ESP_LOGE(TAG, "UDS send failed for SecurityAccess seed: %d", ret);
        return ESP_FAIL;
    }

    // Check response
    if (!uds_is_positive_response(&resp, 0x27)) {
        ESP_LOGE(TAG, "SecurityAccess seed failed: NRC 0x%02X", resp.nrc);
        return ESP_FAIL;
    }

    // Extract seed from response (typically 4 bytes, but depends on ECU)
    // We'll assume the seed is in the first 4 bytes of the response data.
    if (resp.length >= 2 + 4) { // service byte + seed length (we assume 4)
        memcpy(ctx->security_seed, &resp.data[1], 4); // Skip the service byte (0x67) and then the seed
        // Actually, the response format is: 0x67, seed data...
        // So the seed starts at resp.data[0] (which is 0x67) and then the seed data.
        // We want the seed data, so we skip the first byte (the service byte with positive response offset).
        // But note: the response service is 0x27 + 0x40 = 0x67.
        // So the data after the service byte is the seed.
        // We'll copy the next 4 bytes.
        if (resp.length >= 1 + 4) {
            memcpy(ctx->security_seed, &resp.data[1], 4);
        } else {
            // If we don't have 4 bytes, copy what we have and zero pad.
            memcpy(ctx->security_seed, &resp.data[1], resp.length - 1);
            memset(&ctx->security_seed[resp.length - 1], 0, 4 - (resp.length - 1));
        }
    } else {
        // Not enough data, zero out
        memset(ctx->security_seed, 0, 4);
    }

    report_progress(ctx, 20, "Security access seed received");
    return ESP_OK;
}

esp_err_t mdg1_flash_security_access_key(mdg1_flash_ctx_t *ctx)
{
    uds_request_t req;
    uds_response_t resp;
    int ret;
    uint32_t key;

    // Calculate key from seed (this is a placeholder - real algorithm is ECU-specific)
    // For now, we'll just use a simple transformation: key = seed + 0x12345678
    key = (ctx->security_seed[0] << 24) |
          (ctx->security_seed[1] << 16) |
          (ctx->security_seed[2] << 8) |
          ctx->security_seed[3];
    key += 0x12345678;

    // Build SecurityAccess (0x27) request for key
    // Format: 0x27, subfunction = 0x02 (send key), key (4 bytes)
    req.service = 0x27;
    req.length = 1 + 1 + 4; // service + subfunction + key
    req.data[0] = 0x02; // Send key
    req.data[1] = (key >> 24) & 0xFF;
    req.data[2] = (key >> 16) & 0xFF;
    req.data[3] = (key >> 8) & 0xFF;
    req.data[4] = key & 0xFF;

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
    esp_err_t err;
    uint8_t seq_num = 0x00; // Start with sequence number 0

    // Step 1: Security Access - Request Seed
    err = mdg1_flash_security_access_seed(ctx);
    if (err != ESP_OK) return err;

    // Step 2: Security Access - Send Key
    err = mdg1_flash_security_access_key(ctx);
    if (err != ESP_OK) return err;

    // Step 3: RequestDownload
    // We need to know the address and size. For now, we'll use placeholders.
    // In a real implementation, these would be provided by the caller.
    uint32_t address = 0x08000000; // Example flash start address
    uint32_t size = ctx->firmware_buf_size; // Use the firmware buffer size

    err = mdg1_flash_request_download(ctx, address, size);
    if (err != ESP_OK) return err;

    // Step 4: TransferData - Loop through the firmware buffer in chunks
    // We'll use a chunk size of 1024 bytes (or less if near the end)
    const uint32_t chunk_size = 1024;
    uint32_t offset = 0;

    while (offset < size) {
        uint16_t chunk_len = (size - offset < chunk_size) ? (size - offset) : chunk_size;
        err = mdg1_flash_transfer_data(ctx, &ctx->firmware_buf[offset], chunk_len, &seq_num);
        if (err != ESP_OK) return err;
        offset += chunk_len;
    }

    // Step 5: RequestTransferExit
    err = mdg1_flash_request_transfer_exit(ctx);
    if (err != ESP_OK) return err;

    // Step 6: ECU Reset
    err = mdg1_flash_ecu_reset(ctx);
    if (err != ESP_OK) return err;

    report_progress(ctx, 100, "Flash complete");
    return ESP_OK;
}

void mdg1_flash_cleanup(mdg1_flash_ctx_t *ctx)
{
    mbedtls_aes_free(&ctx->aes_ctx);
    memset(ctx, 0, sizeof(mdg1_flash_ctx_t));
}