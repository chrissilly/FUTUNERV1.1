#include "bdef_file.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "BDEF";

// Header structure
typedef struct {
    uint32_t signature;
    uint32_t version;
    uint32_t segment_count;
    uint32_t inverse_segment_count;
    uint32_t pre_cal_data_size;
    uint32_t post_cal_data_size;
    uint32_t cal_start;
    uint32_t cal_end;
} bdef_header_t;

// BDEF file handle
struct bdef_file_s {
    FILE *fp;
    uint32_t file_size;
    bdef_header_t header;
    
    // Computed offsets
    uint32_t segments_index_offset;
    uint32_t inverse_segments_index_offset;
    uint32_t segments_data_offset;
    uint32_t inverse_segments_data_offset;
    uint32_t pre_cal_data_offset;
    uint32_t post_cal_data_offset;
};

// Memory allocation helpers
static void *allocate_memory(size_t size) {
    // Try PSRAM first if available
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) {
        return ptr;
    }
    
    // Fall back to internal RAM
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

// File reading helpers
static esp_err_t read_uint32_le(FILE *fp, uint32_t *value) {
    uint8_t bytes[4];
    if (fread(bytes, 1, 4, fp) != 4) {
        return ESP_FAIL;
    }
    *value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
    return ESP_OK;
}

static esp_err_t read_header(FILE *fp, bdef_header_t *header) {
    if (fseek(fp, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to beginning");
        return ESP_FAIL;
    }
    
    // Read signature (4 bytes)
    uint8_t sig_bytes[4];
    if (fread(sig_bytes, 1, 4, fp) != 4) {
        ESP_LOGE(TAG, "Failed to read signature");
        return ESP_FAIL;
    }
    
    // Validate signature
    if (sig_bytes[0] != 'B' || sig_bytes[1] != 'D' || 
        sig_bytes[2] != 'E' || sig_bytes[3] != 'F') {
        ESP_LOGE(TAG, "Invalid signature: %c%c%c%c", 
                 sig_bytes[0], sig_bytes[1], sig_bytes[2], sig_bytes[3]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    header->signature = BDEF_SIGNATURE;
    
    // Read remaining fields
    if (read_uint32_le(fp, &header->version) != ESP_OK ||
        read_uint32_le(fp, &header->segment_count) != ESP_OK ||
        read_uint32_le(fp, &header->inverse_segment_count) != ESP_OK ||
        read_uint32_le(fp, &header->pre_cal_data_size) != ESP_OK ||
        read_uint32_le(fp, &header->post_cal_data_size) != ESP_OK ||
        read_uint32_le(fp, &header->cal_start) != ESP_OK ||
        read_uint32_le(fp, &header->cal_end) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read header fields");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

static esp_err_t validate_header(const bdef_header_t *header, uint32_t file_size) {
    if (header->version != BDEF_VERSION) {
        ESP_LOGE(TAG, "Unsupported version: %lu (expected %d)", 
                 header->version, BDEF_VERSION);
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    if (file_size < BDEF_HEADER_SIZE) {
        ESP_LOGE(TAG, "File too small: %lu bytes (minimum %d)", 
                 file_size, BDEF_HEADER_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    
    return ESP_OK;
}

static esp_err_t compute_offsets(bdef_file_t *file) {
    // Fixed layout after header
    file->segments_index_offset = BDEF_HEADER_SIZE;
    file->inverse_segments_index_offset = file->segments_index_offset + 
        BDEF_TABLE_COUNT_SIZE + file->header.segment_count * BDEF_SEGMENT_ENTRY_SIZE;
    
    // Scan segment tables to find data region boundaries
    uint32_t max_segments_end = file->inverse_segments_index_offset;
    
    // Scan normal segments
    if (file->header.segment_count > 0) {
        uint32_t table_start = file->segments_index_offset + BDEF_TABLE_COUNT_SIZE;
        if (fseek(file->fp, table_start, SEEK_SET) != 0) {
            ESP_LOGE(TAG, "Failed to seek to segments table");
            return ESP_FAIL;
        }
        
        for (uint32_t i = 0; i < file->header.segment_count; i++) {
            uint32_t address, size, data_offset;
            if (read_uint32_le(file->fp, &address) != ESP_OK ||
                read_uint32_le(file->fp, &size) != ESP_OK ||
                read_uint32_le(file->fp, &data_offset) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read segment entry %lu", i);
                return ESP_FAIL;
            }
            
            if (size > BDEF_MAX_SEGMENT_SIZE) {
                ESP_LOGE(TAG, "Segment %lu size %lu exceeds maximum %d", i, size, BDEF_MAX_SEGMENT_SIZE);
                return ESP_ERR_INVALID_SIZE;
            }
            
            if (data_offset + size > file->file_size) {
                ESP_LOGE(TAG, "Segment %lu extends beyond file", i);
                return ESP_ERR_INVALID_SIZE;
            }
            
            uint32_t end = data_offset + size;
            if (end > max_segments_end) {
                max_segments_end = end;
            }
        }
    }
    
    file->segments_data_offset = file->inverse_segments_index_offset + 
        BDEF_TABLE_COUNT_SIZE + file->header.inverse_segment_count * BDEF_SEGMENT_ENTRY_SIZE;
    
    // Scan inverse segments
    uint32_t max_inverse_end = file->segments_data_offset;
    
    if (file->header.inverse_segment_count > 0) {
        uint32_t table_start = file->inverse_segments_index_offset + BDEF_TABLE_COUNT_SIZE;
        if (fseek(file->fp, table_start, SEEK_SET) != 0) {
            ESP_LOGE(TAG, "Failed to seek to inverse segments table");
            return ESP_FAIL;
        }
        
        for (uint32_t i = 0; i < file->header.inverse_segment_count; i++) {
            uint32_t address, size, data_offset;
            if (read_uint32_le(file->fp, &address) != ESP_OK ||
                read_uint32_le(file->fp, &size) != ESP_OK ||
                read_uint32_le(file->fp, &data_offset) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read inverse segment entry %lu", i);
                return ESP_FAIL;
            }
            
            if (size > BDEF_MAX_SEGMENT_SIZE) {
                ESP_LOGE(TAG, "Inverse segment %lu size %lu exceeds maximum %d", i, size, BDEF_MAX_SEGMENT_SIZE);
                return ESP_ERR_INVALID_SIZE;
            }
            
            if (data_offset + size > file->file_size) {
                ESP_LOGE(TAG, "Inverse segment %lu extends beyond file", i);
                return ESP_ERR_INVALID_SIZE;
            }
            
            uint32_t end = data_offset + size;
            if (end > max_inverse_end) {
                max_inverse_end = end;
            }
        }
    }
    
    file->inverse_segments_data_offset = max_segments_end;
    file->pre_cal_data_offset = max_inverse_end;
    file->post_cal_data_offset = file->pre_cal_data_offset + file->header.pre_cal_data_size;
    
    // Validate total file size
    uint32_t expected_size = file->post_cal_data_offset + file->header.post_cal_data_size;
    if (expected_size != file->file_size) {
        ESP_LOGE(TAG, "File size mismatch: expected %lu, actual %lu", 
                 expected_size, file->file_size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    ESP_LOGI(TAG, "BDEF offsets computed: segments=%lu, inverse=%lu, pre_cal=%lu, post_cal=%lu",
             file->segments_data_offset, file->inverse_segments_data_offset,
             file->pre_cal_data_offset, file->post_cal_data_offset);
    
    return ESP_OK;
}

bdef_file_t *bdef_file_open(const char *path) {
    if (!path) {
        ESP_LOGE(TAG, "NULL path provided");
        return NULL;
    }
    
    // Allocate file handle
    bdef_file_t *file = calloc(1, sizeof(bdef_file_t));
    if (!file) {
        ESP_LOGE(TAG, "Failed to allocate file handle");
        return NULL;
    }
    
    // Open file
    file->fp = fopen(path, "rb");
    if (!file->fp) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        free(file);
        return NULL;
    }
    
    // Get file size
    if (fseek(file->fp, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "Failed to seek to end");
        fclose(file->fp);
        free(file);
        return NULL;
    }
    file->file_size = ftell(file->fp);
    
    // Read and validate header
    if (read_header(file->fp, &file->header) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read header");
        fclose(file->fp);
        free(file);
        return NULL;
    }
    
    if (validate_header(&file->header, file->file_size) != ESP_OK) {
        ESP_LOGE(TAG, "Header validation failed");
        fclose(file->fp);
        free(file);
        return NULL;
    }
    
    // Compute section offsets
    if (compute_offsets(file) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to compute offsets");
        fclose(file->fp);
        free(file);
        return NULL;
    }
    
    ESP_LOGI(TAG, "Opened BDEF: version=%lu, size=%lu, segments=%lu, inverse=%lu",
             file->header.version, file->file_size, 
             file->header.segment_count, file->header.inverse_segment_count);
    
    return file;
}

void bdef_file_close(bdef_file_t *file) {
    if (!file) {
        return;
    }
    
    if (file->fp) {
        fclose(file->fp);
    }
    
    free(file);
}

uint32_t bdef_file_get_version(const bdef_file_t *file) {
    return file ? file->header.version : 0;
}

uint32_t bdef_file_get_total_size(const bdef_file_t *file) {
    return file ? file->file_size : 0;
}

uint32_t bdef_file_get_segment_count(const bdef_file_t *file) {
    return file ? file->header.segment_count : 0;
}

uint32_t bdef_file_get_inverse_segment_count(const bdef_file_t *file) {
    return file ? file->header.inverse_segment_count : 0;
}

uint32_t bdef_file_get_cal_start(const bdef_file_t *file) {
    return file ? file->header.cal_start : 0;
}

uint32_t bdef_file_get_cal_end(const bdef_file_t *file) {
    return file ? file->header.cal_end : 0;
}

static esp_err_t read_segment_internal(bdef_file_t *file, uint32_t index, 
                                       bool inverse, bdef_segment_t *segment) {
    if (!file || !segment) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint32_t count = inverse ? file->header.inverse_segment_count : file->header.segment_count;
    if (index >= count) {
        ESP_LOGE(TAG, "Invalid %ssegment index: %lu", inverse ? "inverse " : "", index);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Seek to segment entry
    uint32_t base_offset = inverse ? file->inverse_segments_index_offset : file->segments_index_offset;
    uint32_t entry_offset = base_offset + BDEF_TABLE_COUNT_SIZE + (index * BDEF_SEGMENT_ENTRY_SIZE);
    
    if (fseek(file->fp, entry_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to segment entry");
        return ESP_FAIL;
    }
    
    // Read segment entry
    if (read_uint32_le(file->fp, &segment->address) != ESP_OK ||
        read_uint32_le(file->fp, &segment->size) != ESP_OK ||
        read_uint32_le(file->fp, &segment->data_offset) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read segment entry");
        return ESP_FAIL;
    }
    
    // Validate
    if (segment->size > BDEF_MAX_SEGMENT_SIZE) {
        ESP_LOGE(TAG, "Segment size too large: %lu", segment->size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    if (segment->data_offset + segment->size > file->file_size) {
        ESP_LOGE(TAG, "Segment extends beyond file");
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Allocate and read data
    segment->data = allocate_memory(segment->size);
    if (!segment->data) {
        ESP_LOGE(TAG, "Failed to allocate segment data");
        return ESP_ERR_NO_MEM;
    }
    
    if (fseek(file->fp, segment->data_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to segment data");
        free(segment->data);
        segment->data = NULL;
        return ESP_FAIL;
    }
    
    if (fread(segment->data, 1, segment->size, file->fp) != segment->size) {
        ESP_LOGE(TAG, "Failed to read segment data");
        free(segment->data);
        segment->data = NULL;
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t bdef_file_read_segment(bdef_file_t *file, uint32_t index, bdef_segment_t *segment) {
    return read_segment_internal(file, index, false, segment);
}

esp_err_t bdef_file_read_inverse_segment(bdef_file_t *file, uint32_t index, bdef_segment_t *segment) {
    return read_segment_internal(file, index, true, segment);
}

esp_err_t bdef_file_read_pre_calibration(bdef_file_t *file, bdef_calibration_data_t *data) {
    if (!file || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    data->offset = file->pre_cal_data_offset;
    data->size = file->header.pre_cal_data_size;
    
    if (data->size == 0) {
        data->data = NULL;
        return ESP_OK;
    }
    
    data->data = allocate_memory(data->size);
    if (!data->data) {
        ESP_LOGE(TAG, "Failed to allocate pre-calibration data");
        return ESP_ERR_NO_MEM;
    }
    
    if (fseek(file->fp, data->offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to pre-calibration data");
        free(data->data);
        data->data = NULL;
        return ESP_FAIL;
    }
    
    if (fread(data->data, 1, data->size, file->fp) != data->size) {
        ESP_LOGE(TAG, "Failed to read pre-calibration data");
        free(data->data);
        data->data = NULL;
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t bdef_file_read_post_calibration(bdef_file_t *file, bdef_calibration_data_t *data) {
    if (!file || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    data->offset = file->post_cal_data_offset;
    data->size = file->header.post_cal_data_size;
    
    if (data->size == 0) {
        data->data = NULL;
        return ESP_OK;
    }
    
    data->data = allocate_memory(data->size);
    if (!data->data) {
        ESP_LOGE(TAG, "Failed to allocate post-calibration data");
        return ESP_ERR_NO_MEM;
    }
    
    if (fseek(file->fp, data->offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to post-calibration data");
        free(data->data);
        data->data = NULL;
        return ESP_FAIL;
    }
    
    if (fread(data->data, 1, data->size, file->fp) != data->size) {
        ESP_LOGE(TAG, "Failed to read post-calibration data");
        free(data->data);
        data->data = NULL;
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

void bdef_segment_free(bdef_segment_t *segment) {
    if (segment && segment->data) {
        free(segment->data);
        segment->data = NULL;
    }
}

void bdef_calibration_data_free(bdef_calibration_data_t *data) {
    if (data && data->data) {
        free(data->data);
        data->data = NULL;
    }
}

