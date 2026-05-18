#ifndef BDEF_FILE_H
#define BDEF_FILE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// BDEF Format Constants
#define BDEF_SIGNATURE 0x46454442  // "BDEF" in ASCII (little-endian)
#define BDEF_VERSION 1
#define BDEF_HEADER_SIZE 32
#define BDEF_SEGMENT_ENTRY_SIZE 12
#define BDEF_TABLE_COUNT_SIZE 4
#define BDEF_MAX_SEGMENT_SIZE 64  // 64 bytes

// Opaque handle for BDEF file
typedef struct bdef_file_s bdef_file_t;

// Segment structure
typedef struct {
    uint32_t address;
    uint32_t size;
    uint32_t data_offset;
    uint8_t *data;  // Allocated data buffer
} bdef_segment_t;

// Calibration data structure
typedef struct {
    uint32_t offset;
    uint32_t size;
    uint8_t *data;  // Allocated data buffer
} bdef_calibration_data_t;

/**
 * @brief Open a BDEF file for reading
 * 
 * @param path Path to the BDEF file
 * @return bdef_file_t* Handle to the opened file, or NULL on error
 */
bdef_file_t *bdef_file_open(const char *path);

/**
 * @brief Close a BDEF file and free resources
 * 
 * @param file BDEF file handle
 */
void bdef_file_close(bdef_file_t *file);

/**
 * @brief Get the BDEF format version
 * 
 * @param file BDEF file handle
 * @return uint32_t Version number
 */
uint32_t bdef_file_get_version(const bdef_file_t *file);

/**
 * @brief Get the total file size
 * 
 * @param file BDEF file handle
 * @return uint32_t File size in bytes
 */
uint32_t bdef_file_get_total_size(const bdef_file_t *file);

/**
 * @brief Get the number of segments
 * 
 * @param file BDEF file handle
 * @return uint32_t Number of segments
 */
uint32_t bdef_file_get_segment_count(const bdef_file_t *file);

/**
 * @brief Get the number of inverse segments
 * 
 * @param file BDEF file handle
 * @return uint32_t Number of inverse segments
 */
uint32_t bdef_file_get_inverse_segment_count(const bdef_file_t *file);

/**
 * @brief Get calibration start address
 * 
 * @param file BDEF file handle
 * @return uint32_t Calibration start address
 */
uint32_t bdef_file_get_cal_start(const bdef_file_t *file);

/**
 * @brief Get calibration end address
 * 
 * @param file BDEF file handle
 * @return uint32_t Calibration end address
 */
uint32_t bdef_file_get_cal_end(const bdef_file_t *file);

/**
 * @brief Read a segment by index
 * 
 * @param file BDEF file handle
 * @param index Segment index
 * @param segment Output segment structure (memory allocated by this function)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t bdef_file_read_segment(bdef_file_t *file, uint32_t index, bdef_segment_t *segment);

/**
 * @brief Read an inverse segment by index
 * 
 * @param file BDEF file handle
 * @param index Inverse segment index
 * @param segment Output segment structure (memory allocated by this function)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t bdef_file_read_inverse_segment(bdef_file_t *file, uint32_t index, bdef_segment_t *segment);

/**
 * @brief Read pre-calibration data
 * 
 * @param file BDEF file handle
 * @param data Output calibration data structure (memory allocated by this function)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t bdef_file_read_pre_calibration(bdef_file_t *file, bdef_calibration_data_t *data);

/**
 * @brief Read post-calibration data
 * 
 * @param file BDEF file handle
 * @param data Output calibration data structure (memory allocated by this function)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t bdef_file_read_post_calibration(bdef_file_t *file, bdef_calibration_data_t *data);

/**
 * @brief Free memory allocated for a segment
 * 
 * @param segment Segment to free
 */
void bdef_segment_free(bdef_segment_t *segment);

/**
 * @brief Free memory allocated for calibration data
 * 
 * @param data Calibration data to free
 */
void bdef_calibration_data_free(bdef_calibration_data_t *data);

#endif // BDEF_FILE_H

