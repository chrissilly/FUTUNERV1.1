/*
 * Bosch MDG1 Checksum Tool — ESP32 Port
 * Ported from MDG1_CRC.cpp by Aftab Hussain (chiptuningshop.com)
 */

#include "mdg1_crc.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "MDG1_CRC";

/* ---- Byte order helpers ---- */

static uint16_t read_word(const uint8_t *buf, uint32_t addr, bool le) {
    uint16_t v = (buf[addr] << 8) | buf[addr + 1]; /* big-endian */
    if (le) v = (buf[addr + 1] << 8) | buf[addr];  /* little-endian */
    return v;
}

static uint32_t read_dword(const uint8_t *buf, uint32_t addr, bool le) {
    if (le) {
        return buf[addr] | (buf[addr+1] << 8) | (buf[addr+2] << 16) | (buf[addr+3] << 24);
    }
    return (buf[addr] << 24) | (buf[addr+1] << 16) | (buf[addr+2] << 8) | buf[addr+3];
}

static void write_dword(uint8_t *buf, uint32_t addr, uint32_t val, bool le) {
    if (le) {
        buf[addr]   = val & 0xFF;
        buf[addr+1] = (val >> 8) & 0xFF;
        buf[addr+2] = (val >> 16) & 0xFF;
        buf[addr+3] = (val >> 24) & 0xFF;
    } else {
        buf[addr]   = (val >> 24) & 0xFF;
        buf[addr+1] = (val >> 16) & 0xFF;
        buf[addr+2] = (val >> 8) & 0xFF;
        buf[addr+3] = val & 0xFF;
    }
}

/* ---- Checksum algorithms ---- */

uint32_t mdg1_add8(const uint8_t *buf, uint32_t start, uint32_t end, uint32_t init) {
    uint32_t sum = init;
    for (uint32_t i = start; i < end; i++) {
        sum += buf[i];
    }
    return sum;
}

uint32_t mdg1_add16(const uint8_t *buf, uint32_t start, uint32_t end, uint32_t init, bool le) {
    uint32_t sum = init;
    for (uint32_t i = start; i < end; i += 2) {
        sum += read_word(buf, i, le);
    }
    return sum;
}

uint32_t mdg1_add32(const uint8_t *buf, uint32_t start, uint32_t end, uint32_t init, bool le) {
    uint32_t sum = init;
    for (uint32_t i = start; i < end; i += 4) {
        sum += read_dword(buf, i, le);
    }
    return sum;
}

static void crc32_table(uint32_t *table) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & ((crc & 1) * 0xFFFFFFFF));
        }
        table[i] = crc;
    }
}

uint32_t mdg1_crc32(const uint8_t *buf, uint32_t start, uint32_t end, uint32_t init) {
    uint32_t crc = init;
    uint32_t table[256];
    crc32_table(table);
    for (uint32_t i = start; i <= end; i++) {
        crc = (crc >> 8) ^ table[(crc ^ buf[i]) & 0xFF];
    }
    return ~crc;
}

/* ---- Block processing ---- */

static int process_block(const uint8_t *buf, size_t buf_size, uint32_t marker_offset,
                         bool le, mdg1_cs_block_t *block, bool fix, uint8_t *fix_buf) {
    int errors = 0;
    uint32_t bs = marker_offset + 0x100; /* header start */

    block->block_offset = marker_offset;
    block->little_endian = le;
    block->header_start = bs;

    /* Validate block start pattern XX X0 00 XX */
    if ((read_dword(buf, bs, le) & 0x000FFF00) != 0) return -1;

    /* Calculate address fix */
    block->address_fix = read_dword(buf, bs + 0x50, le) - bs;
    block->num_entries = buf[bs + 0x13];

    ESP_LOGI(TAG, "Block at 0x%08lX (%s endian), %d entries, addr_fix=0x%08lX",
             (unsigned long)marker_offset, le ? "little" : "big",
             block->num_entries, (unsigned long)block->address_fix);

    /* Header CRC32 */
    uint32_t cs_start = bs;
    uint32_t cs_end = bs + 0x4F + (block->num_entries * 16);
    uint32_t cs_val_addr = cs_end + 1;

    uint32_t calc_crc = mdg1_crc32(buf, cs_start, cs_end, 0xFFFFFFFF);
    uint32_t file_crc = read_dword(buf, cs_val_addr, le);
    uint32_t file_inv = read_dword(buf, cs_val_addr + 4, le);

    block->header_crc_ok = (file_crc == calc_crc && file_inv == ~calc_crc);
    if (!block->header_crc_ok) {
        errors++;
        ESP_LOGW(TAG, "  Header CRC32: file=%08lX calc=%08lX %s",
                 (unsigned long)file_crc, (unsigned long)calc_crc, "MISMATCH");
        if (fix && fix_buf) {
            write_dword(fix_buf, cs_val_addr, calc_crc, le);
            write_dword(fix_buf, cs_val_addr + 4, ~calc_crc, le);
        }
    } else {
        ESP_LOGI(TAG, "  Header CRC32: OK");
    }

    /* Process each checksum entry */
    uint32_t af = block->address_fix;
    for (int x = 0; x < block->num_entries && x < 64; x++) {
        mdg1_cs_entry_t *e = &block->entries[x];
        uint32_t entry_addr = bs + 0x50 + 16 * x;

        e->start = read_dword(buf, entry_addr, le) - af;
        e->end = read_dword(buf, entry_addr + 4, le) - af;
        e->algorithm = buf[entry_addr + 11];
        e->value_address = read_dword(buf, entry_addr + 12, le) - af;

        /* Bounds check */
        if (e->start >= buf_size || e->end >= buf_size || e->value_address >= buf_size - 3) {
            ESP_LOGE(TAG, "  Entry %d: address out of range", x);
            continue;
        }

        e->file_value = read_dword(buf, e->value_address, le);

        switch (e->algorithm) {
        case 1:  /* ADD32 */
            e->calc_value = mdg1_add32(buf, e->start, e->end, 0xFFFFFFFF, le);
            break;
        case 2:  /* CRC32 */
            e->calc_value = mdg1_crc32(buf, e->start, e->end, 0xFFFFFFFF);
            break;
        case 8:  /* ADD8 */
            e->calc_value = mdg1_add8(buf, e->start, e->end, 0xFFFFFFFF);
            break;
        case 16: /* ADD16 */
            e->calc_value = mdg1_add16(buf, e->start, e->end, 0xFFFFFFFF, le);
            break;
        default:
            ESP_LOGE(TAG, "  Entry %d: unknown algorithm %d", x, e->algorithm);
            continue;
        }

        e->ok = (e->file_value == e->calc_value);
        if (!e->ok) {
            errors++;
            if (fix && fix_buf) {
                write_dword(fix_buf, e->value_address, e->calc_value, le);
            }
        }

        const char *algo_names[] = {"?","ADD32","CRC32","?","?","?","?","?","ADD8",
                                     "?","?","?","?","?","?","?","ADD16"};
        const char *algo = (e->algorithm <= 16) ? algo_names[e->algorithm] : "?";
        ESP_LOGI(TAG, "  %s: %08lX-%08lX file=%08lX calc=%08lX %s",
                 algo, (unsigned long)e->start, (unsigned long)e->end,
                 (unsigned long)e->file_value, (unsigned long)e->calc_value,
                 e->ok ? "OK" : "MISMATCH");
    }

    return errors;
}

/* ---- Public API ---- */

static int scan_blocks(const uint8_t *buf, size_t buf_size, mdg1_cs_result_t *result,
                       bool fix, uint8_t *fix_buf) {
    memset(result, 0, sizeof(mdg1_cs_result_t));

    for (uint32_t i = 0; i < buf_size - 4; i++) {
        bool le = false;
        uint32_t be = read_dword(buf, i, false);
        uint32_t lev = read_dword(buf, i, true);

        if (be != 0xDEADBEEF && lev != 0xDEADBEEF) continue;
        if (lev == 0xDEADBEEF) le = true;

        if (result->blocks_found >= 16) break;

        mdg1_cs_block_t *block = &result->blocks[result->blocks_found];
        int errs = process_block(buf, buf_size, i, le, block, fix, fix_buf);
        if (errs < 0) continue; /* not a valid block */

        result->blocks_found++;
        result->checksums_total += 1 + block->num_entries; /* header + entries */
        result->checksums_ok += (1 + block->num_entries) - (errs > 0 ? errs : 0);
        if (fix) result->checksums_fixed += errs;
    }

    ESP_LOGI(TAG, "Scan complete: %d blocks, %d checksums, %d OK, %d errors",
             result->blocks_found, result->checksums_total,
             result->checksums_ok, result->checksums_total - result->checksums_ok);

    return result->checksums_total - result->checksums_ok;
}

int mdg1_crc_validate(const uint8_t *buf, size_t buf_size, mdg1_cs_result_t *result) {
    return scan_blocks(buf, buf_size, result, false, NULL);
}

int mdg1_crc_fix(uint8_t *buf, size_t buf_size, mdg1_cs_result_t *result) {
    return scan_blocks(buf, buf_size, result, true, buf);
}
