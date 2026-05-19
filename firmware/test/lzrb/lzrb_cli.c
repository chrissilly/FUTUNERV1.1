/*
 * lzrb_cli.c — tiny host CLI wrapping lzrb_decompress() so
 * tools/flash_shadow_diff.py can run the plaintext-equivalence step.
 *
 * Usage:  lzrb_cli IN_PATH OUT_PATH EXPECTED_LEN
 * Reads compressed bytes from IN_PATH, decompresses to EXPECTED_LEN
 * bytes, writes to OUT_PATH. Exits 0 on success.
 */

#include "lzrb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int read_all(const char *path, uint8_t **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) { fclose(f); return -1; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "short read %s\n", path);
        free(b); fclose(f); return -1;
    }
    fclose(f);
    *buf = b; *len = (size_t)sz;
    return 0;
}

static int write_all(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "create %s: %s\n", path, strerror(errno)); return -1; }
    int rc = (fwrite(buf, 1, len, f) == len) ? 0 : -1;
    fclose(f);
    if (rc) fprintf(stderr, "short write %s\n", path);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s IN OUT EXPECTED_LEN\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];
    size_t expected = (size_t)strtoull(argv[3], NULL, 0);

    uint8_t *in = NULL; size_t in_len = 0;
    if (read_all(in_path, &in, &in_len) != 0) return 1;

    /* Output buffer sized to expected + headroom. */
    size_t cap = expected + 64;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) { free(in); fprintf(stderr, "malloc out\n"); return 1; }

    size_t actual = 0;
    lzrb_status_t s = lzrb_decompress(in, in_len, out, cap, expected, &actual);
    if (s != LZRB_OK || actual != expected) {
        fprintf(stderr, "lzrb_decompress: %s (status=%d actual=%zu expected=%zu)\n",
                lzrb_status_str(s), (int)s, actual, expected);
        free(in); free(out);
        return 1;
    }
    int rc = write_all(out_path, out, actual);
    free(in); free(out);
    return rc;
}
