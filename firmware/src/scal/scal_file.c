#include "scal_file.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "SCAL";

// Header structure
typedef struct {
    uint32_t signature;
    uint32_t version;
    uint32_t total_size;
    uint32_t calibration_region_size;
    uint32_t flex_map_index_offset;
    uint32_t flex_map_index_size;
    uint32_t custom_table_offset;
    uint32_t custom_table_size;
    uint32_t gasoline_region_offset;
    uint32_t ethanol_region_offset;
    uint64_t reserved;
} scal_header_t;

// SCAL file handle
struct scal_file_s {
    FILE *fp;
    scal_header_t header;
    uint32_t flex_map_count;
};

// Memory allocation helpers
static void *allocate_memory(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) {
        return ptr;
    }
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

static esp_err_t read_uint64_le(FILE *fp, uint64_t *value) {
    uint8_t bytes[8];
    if (fread(bytes, 1, 8, fp) != 8) {
        return ESP_FAIL;
    }
    *value = (uint64_t)bytes[0] | 
             ((uint64_t)bytes[1] << 8) | 
             ((uint64_t)bytes[2] << 16) | 
             ((uint64_t)bytes[3] << 24) |
             ((uint64_t)bytes[4] << 32) | 
             ((uint64_t)bytes[5] << 40) | 
             ((uint64_t)bytes[6] << 48) | 
             ((uint64_t)bytes[7] << 56);
    return ESP_OK;
}

static esp_err_t read_uint16_le(FILE *fp, uint16_t *value) {
    uint8_t bytes[2];
    if (fread(bytes, 1, 2, fp) != 2) {
        return ESP_FAIL;
    }
    *value = bytes[0] | (bytes[1] << 8);
    return ESP_OK;
}

static esp_err_t read_header(FILE *fp, scal_header_t *header) {
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
    if (sig_bytes[0] != 'S' || sig_bytes[1] != 'C' || 
        sig_bytes[2] != 'A' || sig_bytes[3] != 'L') {
        ESP_LOGE(TAG, "Invalid signature: %c%c%c%c", 
                 sig_bytes[0], sig_bytes[1], sig_bytes[2], sig_bytes[3]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    header->signature = SCAL_SIGNATURE;
    
    // Read remaining fields
    if (read_uint32_le(fp, &header->version) != ESP_OK ||
        read_uint32_le(fp, &header->total_size) != ESP_OK ||
        read_uint32_le(fp, &header->calibration_region_size) != ESP_OK ||
        read_uint32_le(fp, &header->flex_map_index_offset) != ESP_OK ||
        read_uint32_le(fp, &header->flex_map_index_size) != ESP_OK ||
        read_uint32_le(fp, &header->custom_table_offset) != ESP_OK ||
        read_uint32_le(fp, &header->custom_table_size) != ESP_OK ||
        read_uint32_le(fp, &header->gasoline_region_offset) != ESP_OK ||
        read_uint32_le(fp, &header->ethanol_region_offset) != ESP_OK ||
        read_uint64_le(fp, &header->reserved) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read header fields");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

static esp_err_t validate_header(const scal_header_t *header) {
    if (header->version != SCAL_VERSION) {
        ESP_LOGE(TAG, "Unsupported version: 0x%08lX (expected 0x%08X)", 
                 header->version, SCAL_VERSION);
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    uint32_t min_size = SCAL_HEADER_SIZE + SCAL_FLEX_INDEX_SIZE + 
                       SCAL_CUSTOM_TABLE_SIZE + (2 * header->calibration_region_size);
    if (header->total_size < min_size) {
        ESP_LOGE(TAG, "File too small: %lu bytes (minimum %lu)", 
                 header->total_size, min_size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    if (header->flex_map_index_offset != SCAL_FLEX_INDEX_OFFSET) {
        ESP_LOGE(TAG, "Invalid flex map index offset: 0x%08lX (expected 0x%08X)", 
                 header->flex_map_index_offset, SCAL_FLEX_INDEX_OFFSET);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    if (header->custom_table_offset != SCAL_CUSTOM_TABLE_OFFSET) {
        ESP_LOGE(TAG, "Invalid custom table offset: 0x%08lX (expected 0x%08X)", 
                 header->custom_table_offset, SCAL_CUSTOM_TABLE_OFFSET);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    if (header->gasoline_region_offset != SCAL_GASOLINE_REGION_BASE) {
        ESP_LOGE(TAG, "Invalid gasoline region offset: 0x%08lX (expected 0x%08X)", 
                 header->gasoline_region_offset, SCAL_GASOLINE_REGION_BASE);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    uint32_t expected_ethanol = header->gasoline_region_offset + header->calibration_region_size;
    if (header->ethanol_region_offset != expected_ethanol) {
        ESP_LOGE(TAG, "Invalid ethanol region offset: 0x%08lX (expected 0x%08lX)", 
                 header->ethanol_region_offset, expected_ethanol);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    if (header->flex_map_index_size != SCAL_FLEX_INDEX_SIZE) {
        ESP_LOGE(TAG, "Invalid flex map index size: %lu (expected %d)", 
                 header->flex_map_index_size, SCAL_FLEX_INDEX_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    
    if (header->custom_table_size != SCAL_CUSTOM_TABLE_SIZE) {
        ESP_LOGE(TAG, "Invalid custom table size: %lu (expected %d)", 
                 header->custom_table_size, SCAL_CUSTOM_TABLE_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    
    return ESP_OK;
}

scal_file_t *scal_file_open(const char *path) {
    if (!path) {
        ESP_LOGE(TAG, "NULL path provided");
        return NULL;
    }
    
    scal_file_t *file = calloc(1, sizeof(scal_file_t));
    if (!file) {
        ESP_LOGE(TAG, "Failed to allocate file handle");
        return NULL;
    }
    
    file->fp = fopen(path, "rb");
    if (!file->fp) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        free(file);
        return NULL;
    }
    
    if (read_header(file->fp, &file->header) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read header");
        fclose(file->fp);
        free(file);
        return NULL;
    }
    
    if (validate_header(&file->header) != ESP_OK) {
        ESP_LOGE(TAG, "Header validation failed");
        fclose(file->fp);
        free(file);
        return NULL;
    }
    
    // Read flex map count
    if (fseek(file->fp, SCAL_FLEX_INDEX_OFFSET, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to flex map index");
        fclose(file->fp);
        free(file);
        return NULL;
    }
    
    if (read_uint32_le(file->fp, &file->flex_map_count) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read flex map count");
        fclose(file->fp);
        free(file);
        return NULL;
    }
    
    if (file->flex_map_count > SCAL_MAX_FLEX_MAPS) {
        ESP_LOGE(TAG, "Invalid flex map count: %lu (max %d)", 
                 file->flex_map_count, SCAL_MAX_FLEX_MAPS);
        fclose(file->fp);
        free(file);
        return NULL;
    }
    
    ESP_LOGI(TAG, "Opened SCAL: version=0x%08lX, size=%lu, flex_maps=%lu",
             file->header.version, file->header.total_size, file->flex_map_count);
    
    return file;
}

void scal_file_close(scal_file_t *file) {
    if (!file) {
        return;
    }
    
    if (file->fp) {
        fclose(file->fp);
    }
    
    free(file);
}

uint32_t scal_file_get_version(const scal_file_t *file) {
    return file ? file->header.version : 0;
}

uint32_t scal_file_get_total_size(const scal_file_t *file) {
    return file ? file->header.total_size : 0;
}

uint32_t scal_file_get_calibration_region_size(const scal_file_t *file) {
    return file ? file->header.calibration_region_size : 0;
}

uint32_t scal_file_get_gasoline_region_offset(const scal_file_t *file) {
    return file ? file->header.gasoline_region_offset : 0;
}

uint32_t scal_file_get_ethanol_region_offset(const scal_file_t *file) {
    return file ? file->header.ethanol_region_offset : 0;
}

uint32_t scal_file_get_flex_map_count(const scal_file_t *file) {
    return file ? file->flex_map_count : 0;
}

esp_err_t scal_file_read_flex_map_entry(scal_file_t *file, uint32_t index, scal_flex_map_entry_t *entry) {
    if (!file || !entry) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (index >= file->flex_map_count) {
        ESP_LOGE(TAG, "Invalid flex map index: %lu", index);
        return ESP_ERR_INVALID_ARG;
    }
    
    uint32_t offset = SCAL_FLEX_INDEX_OFFSET + 4 + (index * SCAL_FLEX_MAP_ENTRY_SIZE);
    if (fseek(file->fp, offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to flex map entry");
        return ESP_FAIL;
    }
    
    uint32_t byte_order_val, data_type_val;
    
    if (read_uint32_le(file->fp, &entry->original_address) != ESP_OK ||
        read_uint32_le(file->fp, &entry->gasoline_address) != ESP_OK ||
        read_uint32_le(file->fp, &entry->ethanol_address) != ESP_OK ||
        read_uint32_le(file->fp, &entry->blend_map_address) != ESP_OK ||
        read_uint32_le(file->fp, &entry->x_dimension) != ESP_OK ||
        read_uint32_le(file->fp, &entry->y_dimension) != ESP_OK ||
        read_uint32_le(file->fp, &byte_order_val) != ESP_OK ||
        read_uint32_le(file->fp, &data_type_val) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read flex map entry");
        return ESP_FAIL;
    }
    
    if (data_type_val > 5) {
        ESP_LOGE(TAG, "Invalid data type: %lu", data_type_val);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    if (byte_order_val > 1) {
        ESP_LOGE(TAG, "Invalid byte order: %lu", byte_order_val);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    if (entry->x_dimension == 0 || entry->x_dimension > 255 ||
        entry->y_dimension == 0 || entry->y_dimension > 255) {
        ESP_LOGE(TAG, "Invalid dimensions: %lux%lu", entry->x_dimension, entry->y_dimension);
        return ESP_ERR_INVALID_SIZE;
    }
    
    entry->byte_order = (scal_byte_order_t)byte_order_val;
    entry->data_type = (scal_data_type_t)data_type_val;
    
    return ESP_OK;
}

esp_err_t scal_file_read_gasoline_data(scal_file_t *file, uint32_t address, uint8_t *buffer, size_t size) {
    if (!file || !buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (fseek(file->fp, address, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to gasoline data");
        return ESP_FAIL;
    }
    
    if (fread(buffer, 1, size, file->fp) != size) {
        ESP_LOGE(TAG, "Failed to read gasoline data");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t scal_file_read_ethanol_data(scal_file_t *file, uint32_t address, uint8_t *buffer, size_t size) {
    if (!file || !buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (fseek(file->fp, address, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to ethanol data");
        return ESP_FAIL;
    }
    
    if (fread(buffer, 1, size, file->fp) != size) {
        ESP_LOGE(TAG, "Failed to read ethanol data");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t scal_file_read_blend_map(scal_file_t *file, uint32_t address, scal_blend_map_t *blend_map) {
    if (!file || !blend_map) {
        return ESP_ERR_INVALID_ARG;
    }
    
    blend_map->x_axis_data = NULL;
    blend_map->z_values_data = NULL;
    
    blend_map->x_axis_data = allocate_memory(9 * sizeof(uint16_t));
    if (!blend_map->x_axis_data) {
        ESP_LOGE(TAG, "Failed to allocate x_axis_data");
        return ESP_ERR_NO_MEM;
    }
    
    blend_map->z_values_data = allocate_memory(9 * sizeof(uint16_t));
    if (!blend_map->z_values_data) {
        ESP_LOGE(TAG, "Failed to allocate z_values_data");
        free(blend_map->x_axis_data);
        blend_map->x_axis_data = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    if (fseek(file->fp, address, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to blend map");
        scal_blend_map_free(blend_map);
        return ESP_FAIL;
    }
    
    for (int i = 0; i < 9; i++) {
        if (read_uint16_le(file->fp, &blend_map->x_axis_data[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read x_axis_data[%d]", i);
            scal_blend_map_free(blend_map);
            return ESP_FAIL;
        }
    }
    
    for (int i = 0; i < 9; i++) {
        if (read_uint16_le(file->fp, &blend_map->z_values_data[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read z_values_data[%d]", i);
            scal_blend_map_free(blend_map);
            return ESP_FAIL;
        }
    }
    
    return ESP_OK;
}

void scal_blend_map_free(scal_blend_map_t *blend_map) {
    if (blend_map) {
        if (blend_map->x_axis_data) {
            free(blend_map->x_axis_data);
            blend_map->x_axis_data = NULL;
        }
        if (blend_map->z_values_data) {
            free(blend_map->z_values_data);
            blend_map->z_values_data = NULL;
        }
    }
}

size_t scal_data_type_get_size(scal_data_type_t type) {
    switch (type) {
        case SCAL_DATA_TYPE_UINT8:
        case SCAL_DATA_TYPE_INT8:
            return 1;
        case SCAL_DATA_TYPE_UINT16:
        case SCAL_DATA_TYPE_INT16:
            return 2;
        case SCAL_DATA_TYPE_UINT32:
        case SCAL_DATA_TYPE_INT32:
            return 4;
        default:
            return 0;
    }
}

uint32_t scal_convert_to_gasoline_address(uint32_t original_address, uint32_t calibration_start) {
    return original_address - calibration_start + SCAL_GASOLINE_REGION_BASE;
}

uint32_t scal_convert_to_ethanol_address(const scal_file_t *file, uint32_t gasoline_address) {
    if (!file) {
        return 0;
    }
    return gasoline_address + file->header.calibration_region_size;
}

