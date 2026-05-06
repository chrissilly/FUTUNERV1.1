#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

/* OTA update context */
typedef struct {
    /* Callback for progress reporting */
    void (*progress_cb)(uint32_t percent, const char *msg);
    
    /* OTA handle */
    esp_ota_handle_t ota_handle;
    
    /* The partition we are updating to */
    const esp_partition_t *update_partition;
    
    /* Total size of the firmware (for progress) */
    uint32_t total_size;
    
    /* Bytes written so far */
    uint32_t bytes_written;
    
    /* Flag to indicate if we have begun the OTA update */
    bool begun;
} ota_update_ctx_t;

/* Initialize OTA update context */
void ota_update_init(ota_update_ctx_t *ctx,
                     void (*progress_cb)(uint32_t percent, const char *msg));

/* Begin OTA update - must be called before receiving any data */
esp_err_t ota_update_begin(ota_update_ctx_t *ctx);

/* Write a chunk of data to the OTA update */
/* data: pointer to the data chunk */
/* len: length of the data chunk */
esp_err_t ota_update_write(ota_update_ctx_t *ctx, const uint8_t *data, uint32_t len);

/* End the OTA update and set the boot partition */
esp_err_t ota_update_end(ota_update_ctx_t *ctx);

/* Abort the OTA update */
void ota_update_abort(ota_update_ctx_t *ctx);

#endif /* OTA_UPDATE_H */