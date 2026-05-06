#ifndef MDG1_CRC_H
#define MDG1_CRC_H

/*
 * Bosch MDG1 Checksum Tool — ESP32 Port
 * ======================================
 * Ported from MDG1_CRC.cpp by Aftab Hussain (chiptuningshop.com)
 *
 * Scans firmware binary for 0xDEADBEEF markers and validates/fixes
 * checksums using ADD8, ADD16, ADD32, and CRC32 algorithms.
 *
 * Each DEADBEEF block contains:
 *   - Header at offset +0x100 from marker
 *   - Number of checksum entries at header+0x13
 *   - Address fix calculated from header+0x50
 *   - CRC32 of header itself
 *   - N checksum entries, each 16 bytes: start, end, algo, value_addr
 *
 * Algorithm indices: 1=ADD32, 2=CRC32, 8=ADD8, 16=ADD16
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint32_t start;
    uint32_t end;
    uint8_t algorithm;       /* 1=ADD32, 2=CRC32, 8=ADD8, 16=ADD16 */
    uint32_t value_address;
    uint32_t file_value;
    uint32_t calc_value;
    bool ok;
} mdg1_cs_entry_t;

typedef struct {
    uint32_t block_offset;   /* DEADBEEF marker position */
    bool little_endian;
    uint32_t header_start;
    uint32_t address_fix;
    uint8_t num_entries;
    bool header_crc_ok;
    mdg1_cs_entry_t entries[64];
} mdg1_cs_block_t;

typedef struct {
    int blocks_found;
    int checksums_total;
    int checksums_ok;
    int checksums_fixed;
    mdg1_cs_block_t blocks[16];
} mdg1_cs_result_t;

/*
 * Validate checksums in a firmware buffer.
 * Does NOT modify the buffer — read-only analysis.
 * Returns number of checksum errors found (0 = all OK).
 */
int mdg1_crc_validate(const uint8_t *buf, size_t buf_size, mdg1_cs_result_t *result);

/*
 * Fix checksums in a firmware buffer.
 * Modifies the buffer in-place.
 * Returns number of checksums fixed (0 = none needed).
 */
int mdg1_crc_fix(uint8_t *buf, size_t buf_size, mdg1_cs_result_t *result);

/* Individual algorithm functions (exposed for testing) */
uint32_t mdg1_add8(const uint8_t *buf, uint32_t start, uint32_t end, uint32_t init);
uint32_t mdg1_add16(const uint8_t *buf, uint32_t start, uint32_t end, uint32_t init, bool little_endian);
uint32_t mdg1_add32(const uint8_t *buf, uint32_t start, uint32_t end, uint32_t init, bool little_endian);
uint32_t mdg1_crc32(const uint8_t *buf, uint32_t start, uint32_t end, uint32_t init);

#endif // MDG1_CRC_H
