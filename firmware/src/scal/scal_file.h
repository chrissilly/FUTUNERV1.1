#ifndef SCAL_FILE_H
#define SCAL_FILE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// SCAL Format Constants
#define SCAL_SIGNATURE 0x5343414C  // "SCAL" in ASCII
#define SCAL_VERSION 0x00000001
#define SCAL_HEADER_SIZE 0x00000400      // 1KB
#define SCAL_FLEX_INDEX_SIZE 0x00010000  // 64KB
#define SCAL_CUSTOM_TABLE_SIZE 0x0007D000  // 500KB
#define SCAL_GASOLINE_REGION_BASE 0x0008D800
#define SCAL_FLEX_MAP_ENTRY_SIZE 32
#define SCAL_FLEX_INDEX_OFFSET 0x00000400
#define SCAL_CUSTOM_TABLE_OFFSET 0x00010400
#define SCAL_MAX_FLEX_MAPS 2047

// Data type codes for flex maps
typedef enum {
    SCAL_DATA_TYPE_UINT8 = 0,
    SCAL_DATA_TYPE_UINT16 = 1,
    SCAL_DATA_TYPE_UINT32 = 2,
    SCAL_DATA_TYPE_INT8 = 3,
    SCAL_DATA_TYPE_INT16 = 4,
    SCAL_DATA_TYPE_INT32 = 5
} scal_data_type_t;

// Byte order codes
typedef enum {
    SCAL_BYTE_ORDER_LITTLE_ENDIAN = 0,
    SCAL_BYTE_ORDER_BIG_ENDIAN = 1
} scal_byte_order_t;

// Opaque handle for SCAL file
typedef struct scal_file_s scal_file_t;

// Flex map entry structure
typedef struct {
    uint32_t original_address;    // Original address in source binary (write target)
    uint32_t gasoline_address;    // Address for gasoline calibration data in SCAL
    uint32_t ethanol_address;     // Address for ethanol calibration data in SCAL
    uint32_t blend_map_address;   // Address of blend map in custom table region
    uint32_t x_dimension;         // X dimension of the map
    uint32_t y_dimension;         // Y dimension of the map
    scal_byte_order_t byte_order; // Byte order of the map data
    scal_data_type_t data_type;   // Data type of the map elements
} scal_flex_map_entry_t;

// Blend map data structure
typedef struct {
    uint16_t *x_axis_data;      // 9 uint16 values for blend percentages
    uint16_t *z_values_data;    // 9 uint16 values for interpolation factors
} scal_blend_map_t;

/**
 * @brief Open a SCAL file for reading
 * 
 * @param path Path to the SCAL file
 * @return scal_file_t* Handle to the opened file, or NULL on error
 */
scal_file_t *scal_file_open(const char *path);

/**
 * @brief Close a SCAL file and free resources
 * 
 * @param file SCAL file handle
 */
void scal_file_close(scal_file_t *file);

/**
 * @brief Get the SCAL format version
 * 
 * @param file SCAL file handle
 * @return uint32_t Version number
 */
uint32_t scal_file_get_version(const scal_file_t *file);

/**
 * @brief Get the total file size
 * 
 * @param file SCAL file handle
 * @return uint32_t File size in bytes
 */
uint32_t scal_file_get_total_size(const scal_file_t *file);

/**
 * @brief Get the calibration region size
 * 
 * @param file SCAL file handle
 * @return uint32_t Calibration region size in bytes
 */
uint32_t scal_file_get_calibration_region_size(const scal_file_t *file);

/**
 * @brief Get the gasoline region offset
 * 
 * @param file SCAL file handle
 * @return uint32_t Gasoline region offset in bytes
 */
uint32_t scal_file_get_gasoline_region_offset(const scal_file_t *file);

/**
 * @brief Get the ethanol region offset
 * 
 * @param file SCAL file handle
 * @return uint32_t Ethanol region offset in bytes
 */
uint32_t scal_file_get_ethanol_region_offset(const scal_file_t *file);

/**
 * @brief Get the number of flex maps
 * 
 * @param file SCAL file handle
 * @return uint32_t Number of flex maps
 */
uint32_t scal_file_get_flex_map_count(const scal_file_t *file);

/**
 * @brief Read a flex map entry by index
 * 
 * @param file SCAL file handle
 * @param index Flex map index
 * @param entry Output flex map entry structure
 * @return esp_err_t ESP_OK on success
 */
esp_err_t scal_file_read_flex_map_entry(scal_file_t *file, uint32_t index, scal_flex_map_entry_t *entry);

/**
 * @brief Read data from gasoline calibration region
 * 
 * @param file SCAL file handle
 * @param address Address in gasoline region
 * @param buffer Output buffer
 * @param size Number of bytes to read
 * @return esp_err_t ESP_OK on success
 */
esp_err_t scal_file_read_gasoline_data(scal_file_t *file, uint32_t address, uint8_t *buffer, size_t size);

/**
 * @brief Read data from ethanol calibration region
 * 
 * @param file SCAL file handle
 * @param address Address in ethanol region
 * @param buffer Output buffer
 * @param size Number of bytes to read
 * @return esp_err_t ESP_OK on success
 */
esp_err_t scal_file_read_ethanol_data(scal_file_t *file, uint32_t address, uint8_t *buffer, size_t size);

/**
 * @brief Read blend map data
 * 
 * @param file SCAL file handle
 * @param address Address of blend map in custom table region
 * @param blend_map Output blend map structure (memory allocated by this function)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t scal_file_read_blend_map(scal_file_t *file, uint32_t address, scal_blend_map_t *blend_map);

/**
 * @brief Free memory allocated for a blend map
 * 
 * @param blend_map Blend map to free
 */
void scal_blend_map_free(scal_blend_map_t *blend_map);

/**
 * @brief Get the size in bytes of a data type
 * 
 * @param type Data type
 * @return size_t Size in bytes
 */
size_t scal_data_type_get_size(scal_data_type_t type);

/**
 * @brief Convert original address to gasoline region address
 * 
 * @param original_address Original address in ECU
 * @param calibration_start Start of calibration region in ECU
 * @return uint32_t Address in SCAL gasoline region
 */
uint32_t scal_convert_to_gasoline_address(uint32_t original_address, uint32_t calibration_start);

/**
 * @brief Convert gasoline address to ethanol region address
 * 
 * @param file SCAL file handle
 * @param gasoline_address Address in gasoline region
 * @return uint32_t Address in ethanol region
 */
uint32_t scal_convert_to_ethanol_address(const scal_file_t *file, uint32_t gasoline_address);

#endif // SCAL_FILE_H

