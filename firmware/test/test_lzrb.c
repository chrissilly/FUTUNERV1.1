/*
 * test_lzrb.c — host-runnable round-trip tests for the LZRB codec.
 *
 *   compress(X) → decompress(Y) == X
 *
 * across several input shapes that exercise different parts of the
 * encoder: empty, 1-byte, all-zeros (RLE / pattern path), all-0xFF
 * (long-run pattern path), repeating 4-byte pattern (PATTERN_LENS=4
 * branch), random bytes (literal-heavy path), and a structured payload
 * that mixes literals with back-references.
 *
 * Built into firmware/test/lzrb/host_test_runner via the Makefile in
 * that directory.
 */

#include "lzrb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond, msg) do {                                              \
    if (!(cond)) {                                                          \
        fprintf(stderr, "  FAIL  %s — %s (line %d)\n",                      \
                __func__, (msg), __LINE__);                                 \
        g_failures++;                                                       \
    } else {                                                                \
        fprintf(stdout, "  PASS  %s — %s\n", __func__, (msg));              \
    }                                                                       \
} while (0)

static void roundtrip(const char *label,
                      const uint8_t *src, size_t src_len)
{
    /* Generous output buffer: 2x src_len + 64 B for tiny inputs. */
    size_t cap = src_len * 2 + 64;
    uint8_t *cmp = malloc(cap);
    uint8_t *dec = malloc(src_len + 64);
    if (!cmp || !dec) {
        fprintf(stderr, "  FAIL  %s — malloc\n", label);
        g_failures++;
        free(cmp); free(dec);
        return;
    }

    size_t cmp_len = 0;
    lzrb_status_t s = lzrb_compress(src, src_len, cmp, cap, &cmp_len);
    if (s != LZRB_OK) {
        fprintf(stderr, "  FAIL  %s — compress: %s\n", label, lzrb_status_str(s));
        g_failures++;
        free(cmp); free(dec);
        return;
    }

    size_t dec_len = 0;
    s = lzrb_decompress(cmp, cmp_len, dec, src_len + 64, src_len, &dec_len);
    if (s != LZRB_OK) {
        fprintf(stderr, "  FAIL  %s — decompress: %s (got %zu/%zu bytes; cmp=%zu)\n",
                label, lzrb_status_str(s), dec_len, src_len, cmp_len);
        g_failures++;
        free(cmp); free(dec);
        return;
    }

    if (dec_len != src_len) {
        fprintf(stderr, "  FAIL  %s — length mismatch: got %zu expected %zu\n",
                label, dec_len, src_len);
        g_failures++;
        free(cmp); free(dec);
        return;
    }

    if (memcmp(src, dec, src_len) != 0) {
        /* Print first diff for diagnostics */
        size_t diff = 0;
        while (diff < src_len && src[diff] == dec[diff]) diff++;
        fprintf(stderr, "  FAIL  %s — byte mismatch at offset %zu (src=0x%02X dec=0x%02X)\n",
                label, diff, src[diff], dec[diff]);
        g_failures++;
        free(cmp); free(dec);
        return;
    }

    fprintf(stdout, "  PASS  %s — %zu B → %zu B → %zu B (ratio %.2f)\n",
            label, src_len, cmp_len, dec_len,
            src_len ? (double)cmp_len / (double)src_len : 0.0);
    free(cmp);
    free(dec);
}

/* ------------------------------------------------------------------ */

static void test_empty(void) {
    roundtrip("empty", (const uint8_t *)"", 0);
}

static void test_one_byte(void) {
    uint8_t b = 0x42;
    roundtrip("1B literal", &b, 1);
}

static void test_two_distinct(void) {
    uint8_t b[2] = { 0xAB, 0xCD };
    roundtrip("2B distinct", b, 2);
}

static void test_two_same(void) {
    uint8_t b[2] = { 0x55, 0x55 };
    roundtrip("2B same", b, 2);
}

static void test_zeros_4k(void) {
    size_t n = 4096;
    uint8_t *buf = calloc(1, n);
    roundtrip("4 KB zeros", buf, n);
    free(buf);
}

static void test_ff_4k(void) {
    size_t n = 4096;
    uint8_t *buf = malloc(n);
    memset(buf, 0xFF, n);
    roundtrip("4 KB 0xFF", buf, n);
    free(buf);
}

static void test_repeating_4byte(void) {
    size_t n = 4096;
    uint8_t *buf = malloc(n);
    static const uint8_t pat[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    for (size_t i = 0; i < n; i++) buf[i] = pat[i & 3];
    roundtrip("4 KB repeating 4-byte pattern", buf, n);
    free(buf);
}

static void test_random_4k(void) {
    size_t n = 4096;
    uint8_t *buf = malloc(n);
    /* Fixed seed so the test is deterministic across runs. */
    uint32_t s = 0xC0FFEE42u;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(s >> 16);
    }
    roundtrip("4 KB random (LCG)", buf, n);
    free(buf);
}

static void test_mixed_64k(void) {
    /* Half random, half repeating — exercises both code paths. */
    size_t n = 65536;
    uint8_t *buf = malloc(n);
    uint32_t s = 0xCAFEBABEu;
    for (size_t i = 0; i < n / 2; i++) {
        s = s * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(s >> 16);
    }
    for (size_t i = n / 2; i < n; i++) buf[i] = (uint8_t)(i & 0xFF);
    roundtrip("64 KB mixed (random + ramp)", buf, n);
    free(buf);
}

static void test_short_repeated_phrase(void) {
    /* Tests that small back-references work after literal preamble. */
    static const char *phrase = "the quick brown fox jumps over the lazy dog. ";
    size_t pl = strlen(phrase);
    size_t n = pl * 50;
    uint8_t *buf = malloc(n);
    for (size_t i = 0; i < 50; i++) memcpy(buf + i * pl, phrase, pl);
    roundtrip("repeating English phrase (50x)", buf, n);
    free(buf);
}

/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stdout, "== LZRB round-trip tests ==\n");
    test_empty();
    test_one_byte();
    test_two_distinct();
    test_two_same();
    test_zeros_4k();
    test_ff_4k();
    test_repeating_4byte();
    test_random_4k();
    test_mixed_64k();
    test_short_repeated_phrase();

    if (g_failures > 0) {
        fprintf(stderr, "\n== %d FAILURES ==\n", g_failures);
        return 1;
    }
    fprintf(stdout, "\n== all LZRB tests passed ==\n");
    return 0;
}
