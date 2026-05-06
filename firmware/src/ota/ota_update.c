#include "ota_update.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "OTA_UPDATE";

void ota_update_init(ota_update_ctx_t *ctx,
                     void (*progress_cb)(uint32_t percent, const char *msg))
{
    memset(ctx, 0, sizeof(ota_update_ctx_t));
    ctx->progress_cb = progress_cb;
}

esp_err_t ota_update_begin(ota_update_ctx_t *ctx)
{
    esp_err_t err;

    if (ctx->begun) {
        ESP_LOGW(TAG, "OTA update already begun");
        return ESP_ERR_INVALID_STATE;
    }

    // Get the partition to update
    ctx->update_partition = esp_ota_get_next_update_partition(NULL);
    if (ctx->update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Writing to partition '%s' at offset 0x%x",
             ctx->update_partition->label, ctx->update_partition->address);

    // Begin the OTA update
    err = esp_ota_begin(ctx->update_partition, OTA_SIZE_UNKNOWN, &ctx->ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    ctx->begun = true;
    ESP_LOGI(TAG, "OTA update begun");

    return ESP_OK;
}

esp_err_t ota_update_write(ota_update_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    esp_err_t err;

    if (!ctx->begun) {
        ESP_LOGE(TAG, "OTA update not begun");
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_write(ctx->ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return err;
    }

    ctx->bytes_written += len;
    if (ctx->total_size > 0) {
        uint32_t percent = (ctx->bytes_written * 100) / ctx->total_size;
        if (ctx->progress_cb) {
            ctx->progress_cb(percent, "Writing OTA update");
        }
    }

    return ESP_OK;
}

esp_err_t ota_update_end(ota_update_ctx_t *ctx)
{
    esp_err_t err;

    if (!ctx->begun) {
        ESP_LOGW(TAG, "OTA update not begun");
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_end(ctx->ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(ctx->update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA update prepared successfully, reboot to boot from new partition");

    ctx->begun = false;
    return ESP_OK;
}

void ota_update_abort(ota_update_ctx_t *ctx)
{
    if (ctx->begun) {
        esp_ota_abort(ctx->ota_handle);
        ctx->begun = false;
        ESP_LOGI(TAG, "OTA update aborted");
    }
}