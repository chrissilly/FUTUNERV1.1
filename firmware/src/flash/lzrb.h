#ifndef LZRB_H
#define LZRB_H

/*
 * LZRB — the LZ77 variant Bosch uses to compress flash payloads inside
 * MG1/MDG1 ODX containers (after AES-128-CBC decryption). Bit format
 * derived from the Java reference at
 * hw_reference/vag_mdg1_drive_pull/packing/lzrb_src/.
 *
 * Wire format (token stream, MSB-first bit packing):
 *   flag: 1 bit
 *     0 → 8-bit literal byte follows
 *     1 → back-reference: variable-length offset, then variable-length length
 *
 * Offset encoding (3-bit prefix + payload):
 *   prefix == 7  → 11-bit raw wire-offset (used for offsets >= 1016)
 *   prefix < 7   → bits = prefix + 3, payload = `bits` bits,
 *                  wire_offset = payload + (1 << bits) - 8
 *
 * Offset transform (BasicCoder.transformOffset, even/odd interleave):
 *   wire_offset >= 1024 → actual = 2 * (wire_offset - 1024) + 1     (odd)
 *   wire_offset <  1024 → actual = 2 * wire_offset                  (even)
 *   wire_offset == 1024 represents both actual=0 (termination sentinel)
 *   and actual=1 (legitimate offset-1 back-reference). Disambiguated by
 *   the expected output length — the caller passes it in.
 *
 * Length encoding (Elias-gamma over (length - 1)):
 *   count zeros until the leading 1 → call that `bits`
 *   the leading 1 is the high bit of (length - 1)
 *   read `bits` more bits → low bits of (length - 1)
 *   length = ((1 << bits) | low_bits) + 1
 *   Minimum legitimate length is 2.
 *
 * Sliding window: 2048 bytes. Compressor uses pattern-fill encoding for
 * patternLengths {1, 2, 4} so large 0xFF / repeating-word regions compress
 * efficiently — the decompressor doesn't care about pattern detection,
 * it just copies `length` bytes from `actual` bytes back.
 *
 * Termination: encoder emits flag=1, wire_offset=1024 (actual=0 sentinel),
 * then 8 zero bits as flush padding. Those 8 zeros are NOT uniquely
 * identifiable in the bit stream because legitimate length encodings can
 * also produce 8+ leading zeros. The decompressor uses out_capacity (or
 * the ODX `UNCOMPRESSED-SIZE` it reflects) as the stop condition.
 */

#include <stddef.h>
#include <stdint.h>

typedef enum {
    LZRB_OK = 0,
    LZRB_ERR_INPUT_EXHAUSTED,   /* input ended mid-token */
    LZRB_ERR_OUTPUT_OVERFLOW,   /* would write past out_cap */
    LZRB_ERR_BAD_OFFSET,        /* back-reference points before start of output */
    LZRB_ERR_INTERNAL,          /* invariant broken — bug, not bad data */
} lzrb_status_t;

const char *lzrb_status_str(lzrb_status_t s);

/*
 * Decompress LZRB-encoded `in` (in_len bytes) into `out` (capacity out_cap).
 * Stops as soon as `expected_out_len` bytes have been emitted. Returns
 * LZRB_OK on success and writes the actual number of bytes produced via
 * *out_len_actual (== expected_out_len on success).
 */
lzrb_status_t lzrb_decompress(const uint8_t *in,
                              size_t        in_len,
                              uint8_t       *out,
                              size_t        out_cap,
                              size_t        expected_out_len,
                              size_t       *out_len_actual);

/*
 * Compress `in` (in_len bytes) into `out` (capacity out_cap), using the
 * same wire format the Java reference emits. Returns LZRB_OK and writes
 * the produced byte count via *out_len_actual.
 */
lzrb_status_t lzrb_compress(const uint8_t *in,
                            size_t        in_len,
                            uint8_t       *out,
                            size_t        out_cap,
                            size_t       *out_len_actual);

#endif /* LZRB_H */
