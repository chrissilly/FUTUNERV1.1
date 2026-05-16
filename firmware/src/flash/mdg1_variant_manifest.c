/*
 * mdg1_variant_manifest.c — minimal-footprint loader for the per-variant
 * manifest JSON files. See mdg1_variant_manifest.h for the API contract.
 *
 * No general-purpose JSON parser is used or vendored — the loader walks
 * the two known JSON schemas with simple substring + value-extraction
 * helpers. This keeps the firmware footprint small (~600 LOC including
 * comments) and avoids pulling cJSON into the host test build.
 *
 * Schemas consumed:
 *   secrets/mdg1_variant_manifest.json:
 *     .boxcode_index["<bc>"]                               → variant name
 *     .variants["<v>"].sa2_scripts_hex[0]                   → hex string
 *     .variants["<v>"].flash_sections_by_boxcode["<bc>"][i].{block_id,
 *         plaintext_size, ecu_address, expected_crc32, file_offset,
 *         file_length, name}
 *     .variants["<v>"].plaintext_source_by_boxcode["<bc>"].bin_path
 *
 *   secrets/aes_keys_per_boxcode.json:
 *     [].{boxcode == target, aes_key.{bin_path, offset, length_bytes,
 *                                     sha256_first8_fingerprint}}
 */

#include "mdg1_variant_manifest.h"

#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
#  include <stdio.h>
#  include <stdlib.h>
#endif

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* SHA-256 — minimal embedded implementation (RFC 6234 reference).    */
/*                                                                    */
/* Used ONLY to compute the first-8-hex fingerprint of the 16 AES key */
/* bytes loaded from the bin, for verification against the manifest's */
/* stored sha256_first8_fingerprint. Not a hot path; not constant     */
/* time; that's fine here.                                            */
/* ------------------------------------------------------------------ */

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_compress(uint32_t H[8], const uint8_t b[64])
{
    uint32_t W[64];
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) |
               ((uint32_t)b[i*4+2] << 8) | (uint32_t)b[i*4+3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(W[i-15], 7) ^ ror32(W[i-15], 18) ^ (W[i-15] >> 3);
        uint32_t s1 = ror32(W[i-2], 17) ^ ror32(W[i-2], 19) ^ (W[i-2] >> 10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }
    uint32_t a = H[0], bv = H[1], c = H[2], d = H[3],
             e = H[4], f = H[5], g = H[6], h = H[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + W[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t mj = (a & bv) ^ (a & c) ^ (bv & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = bv; bv = a; a = t1 + t2;
    }
    H[0]+=a; H[1]+=bv; H[2]+=c; H[3]+=d; H[4]+=e; H[5]+=f; H[6]+=g; H[7]+=h;
}

static void sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint32_t H[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t buf[128];
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) sha256_compress(H, msg + i*64);
    size_t rem = len - full*64;
    memset(buf, 0, sizeof(buf));
    memcpy(buf, msg + full*64, rem);
    buf[rem] = 0x80;
    if (rem >= 56) {
        uint64_t bits = (uint64_t)len * 8;
        for (int i = 0; i < 8; i++) buf[120 + i] = (uint8_t)(bits >> (56 - i*8));
        sha256_compress(H, buf);
        sha256_compress(H, buf + 64);
    } else {
        uint64_t bits = (uint64_t)len * 8;
        for (int i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(bits >> (56 - i*8));
        sha256_compress(H, buf);
    }
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(H[i] >> 24);
        out[i*4+1] = (uint8_t)(H[i] >> 16);
        out[i*4+2] = (uint8_t)(H[i] >> 8);
        out[i*4+3] = (uint8_t)(H[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Tiny JSON walker — sufficient for our two schemas, NOT general.    */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

/* Match a quoted JSON key literally. Returns position after the
 * trailing quote, or NULL on no match at p. */
static const char *match_key(const char *p, const char *end, const char *key)
{
    p = skip_ws(p, end);
    if (p >= end || *p != '"') return NULL;
    p++;
    size_t klen = strlen(key);
    if ((size_t)(end - p) < klen + 1) return NULL;
    if (memcmp(p, key, klen) != 0) return NULL;
    if (p[klen] != '"') return NULL;
    return p + klen + 1;
}

/* Find first occurrence of '"key"' inside the object that starts at p
 * (or that p points just-past-the-`{` of). Returns pointer just after
 * ':' for the matched key, or NULL. Skips nested objects/arrays.
 *
 * Auto-skips a leading '{' (with optional whitespace) so callers can
 * pass either:
 *   (a) the position of the object's opening '{'      (we skip it)
 *   (b) the position just after the '{' (inside)      (we don't see it)
 *   (c) the position just-past the ':' of a key whose value is an
 *       object: skip_ws then auto-skip works.
 *
 * Returns NULL on end-of-object (matching '}') without finding the key. */
static const char *find_obj_key(const char *p, const char *end, const char *key)
{
    /* Auto-skip leading whitespace + '{' so the depth tracking below
     * sees the inside of the object as depth==0. Without this fix,
     * top-level lookups land at the file's opening '{' which would
     * push depth to 1 and prevent any match. */
    p = skip_ws(p, end);
    if (p < end && *p == '{') {
        p++;
    }
    int depth = 0;
    bool in_str = false;
    while (p < end) {
        char c = *p;
        if (in_str) {
            if (c == '\\' && p + 1 < end) { p += 2; continue; }
            if (c == '"') in_str = false;
            p++; continue;
        }
        if (c == '"') {
            if (depth == 0) {
                /* Try to match the key. */
                const char *after = match_key(p, end, key);
                if (after) {
                    after = skip_ws(after, end);
                    if (after < end && *after == ':') return after + 1;
                }
            }
            in_str = true;
            p++; continue;
        }
        if (c == '{' || c == '[') { depth++; p++; continue; }
        if (c == '}' || c == ']') { if (depth == 0) return NULL; depth--; p++; continue; }
        p++;
    }
    return NULL;
}

/* Extract a JSON string value at p (which points to start of a value).
 * Copies up to out_cap-1 bytes + NUL into out. Returns end-of-value
 * pointer or NULL on parse failure. */
static const char *extract_string(const char *p, const char *end,
                                  char *out, size_t out_cap)
{
    p = skip_ws(p, end);
    if (p >= end || *p != '"') return NULL;
    p++;
    size_t o = 0;
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) {
            char esc = p[1];
            char rendered = esc;
            switch (esc) {
                case 'n': rendered = '\n'; break;
                case 't': rendered = '\t'; break;
                case 'r': rendered = '\r'; break;
                case '\\': rendered = '\\'; break;
                case '"': rendered = '"'; break;
                case '/': rendered = '/'; break;
                default: break;
            }
            if (o + 1 < out_cap) out[o++] = rendered;
            p += 2;
            continue;
        }
        if (o + 1 < out_cap) out[o++] = *p;
        p++;
    }
    if (p >= end) return NULL;
    out[o] = '\0';
    return p + 1;  /* skip closing quote */
}

/* Extract a JSON integer (decimal). p points at start of value.
 * Returns end-of-value pointer or NULL. */
static const char *extract_int(const char *p, const char *end, long *out)
{
    p = skip_ws(p, end);
    if (p >= end) return NULL;
    char buf[24] = {0};
    size_t i = 0;
    if (*p == '-' && i + 1 < sizeof(buf)) buf[i++] = *p++;
    while (p < end && *p >= '0' && *p <= '9' && i + 1 < sizeof(buf)) {
        buf[i++] = *p++;
    }
    if (i == 0) return NULL;
    *out = strtol(buf, NULL, 10);
    return p;
}

/* Parse hex-string-as-int: either "0x...." or quoted "0x...." or
 * quoted "1234" (decimal-in-quotes also accepted). */
static const char *extract_hex_or_dec_uint32(const char *p, const char *end, uint32_t *out)
{
    p = skip_ws(p, end);
    if (p < end && *p == '"') {
        char buf[24];
        const char *r = extract_string(p, end, buf, sizeof(buf));
        if (!r) return NULL;
        unsigned long v = strtoul(buf, NULL, 0);  /* auto-detect base */
        *out = (uint32_t)v;
        return r;
    }
    /* bare integer */
    long v = 0;
    const char *r = extract_int(p, end, &v);
    if (!r) return NULL;
    *out = (uint32_t)v;
    return r;
}

/* Find the start position of an array's first element. p points at '['. */
static const char *array_first_element(const char *p, const char *end)
{
    p = skip_ws(p, end);
    if (p >= end || *p != '[') return NULL;
    p++;
    return skip_ws(p, end);
}

/* Given pointer at start of an element, advance past one element + any
 * trailing comma, return pointer at the next element start (or at ']'). */
static const char *array_next_element(const char *p, const char *end)
{
    int depth = 0;
    bool in_str = false;
    while (p < end) {
        char c = *p;
        if (in_str) {
            if (c == '\\' && p + 1 < end) { p += 2; continue; }
            if (c == '"') in_str = false;
            p++; continue;
        }
        if (c == '"') { in_str = true; p++; continue; }
        if (c == '{' || c == '[') { depth++; p++; continue; }
        if (c == '}' || c == ']') {
            if (depth == 0) return p;  /* at ']' of outer array */
            depth--; p++; continue;
        }
        if (c == ',' && depth == 0) {
            p++;
            return skip_ws(p, end);
        }
        p++;
    }
    return p;
}

/* Decode hex byte pair "AB" → 0xAB; returns -1 on bad chars. */
static int hex_pair(const char *p)
{
    int hi = 0, lo = 0;
    char a = p[0], b = p[1];
    if      (a >= '0' && a <= '9') hi = a - '0';
    else if (a >= 'a' && a <= 'f') hi = 10 + a - 'a';
    else if (a >= 'A' && a <= 'F') hi = 10 + a - 'A';
    else return -1;
    if      (b >= '0' && b <= '9') lo = b - '0';
    else if (b >= 'a' && b <= 'f') lo = 10 + b - 'a';
    else if (b >= 'A' && b <= 'F') lo = 10 + b - 'A';
    else return -1;
    return (hi << 4) | lo;
}

/* ------------------------------------------------------------------ */
/* File slurp + boxcode key-form normalization                        */
/* ------------------------------------------------------------------ */

static esp_err_t slurp_file(const char *path, char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;
#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_INVALID_STATE;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return ESP_ERR_INVALID_STATE; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_INVALID_STATE; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return ESP_ERR_INVALID_STATE; }
    buf[sz] = '\0';
    *out_buf = buf;
    *out_len = (size_t)sz;
    return ESP_OK;
#else
    /* TODO: SPIFFS read for firmware. For now, the firmware path is
     * not exercised by this prompt's tests (host-only). */
    (void)path; (void)out_buf; (void)out_len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* Normalize "4K0907557G__0003" (aes_keys_per_boxcode.json form) to
 * "4K0907557G 0003" (mdg1_variant_manifest.json boxcode_index form). */
static void normalize_boxcode_space(const char *in, char *out, size_t out_cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_cap; i++) {
        if (in[i] == '_' && in[i+1] == '_') {
            out[o++] = ' ';
            i++;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void mdg1_variant_manifest_clear(mdg1_variant_t *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
}

esp_err_t mdg1_variant_manifest_load(const char *manifest_json_path,
                                     const char *keys_json_path,
                                     const char *boxcode,
                                     mdg1_variant_t *out)
{
    if (!manifest_json_path || !keys_json_path || !boxcode || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /* Save the input boxcode (used as-is for aes_keys_per_boxcode.json lookup). */
    strncpy(out->boxcode, boxcode, sizeof(out->boxcode) - 1);

    /* Build the space-separator form for mdg1_variant_manifest.json lookup. */
    char boxcode_space[MDG1_VARIANT_BOXCODE_MAX];
    normalize_boxcode_space(boxcode, boxcode_space, sizeof(boxcode_space));

    /* ---- Slurp the two JSON files. ---- */
    char *manifest = NULL, *keys = NULL;
    size_t manifest_len = 0, keys_len = 0;
    esp_err_t e = slurp_file(manifest_json_path, &manifest, &manifest_len);
    if (e != ESP_OK) return e;
    e = slurp_file(keys_json_path, &keys, &keys_len);
    if (e != ESP_OK) {
#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
        free(manifest);
#endif
        return e;
    }

    const char *mend = manifest + manifest_len;
    const char *kend = keys + keys_len;

    /* ---- Step 1: boxcode_index → variant name. ---- */
    const char *bi = find_obj_key(manifest, mend, "boxcode_index");
    if (!bi) goto fail_state;
    bi = skip_ws(bi, mend);
    if (bi >= mend || *bi != '{') goto fail_state;
    bi++;
    /* Linear scan for our boxcode key inside boxcode_index. */
    const char *v_after = find_obj_key(bi, mend, boxcode_space);
    if (!v_after) goto fail_notfound;
    if (!extract_string(v_after, mend, out->variant_name, sizeof(out->variant_name))) {
        goto fail_state;
    }

    /* ---- Step 2: variants[<name>] → SA2 + flash_sections + plaintext_source. ---- */
    const char *variants = find_obj_key(manifest, mend, "variants");
    if (!variants) goto fail_state;
    const char *v_obj = find_obj_key(variants, mend, out->variant_name);
    if (!v_obj) goto fail_notfound;

    /* SA2: variants[v].sa2_scripts_hex[0] */
    const char *sa2 = find_obj_key(v_obj, mend, "sa2_scripts_hex");
    if (!sa2) goto fail_state;
    sa2 = skip_ws(sa2, mend);
    if (sa2 >= mend || *sa2 != '[') goto fail_state;
    sa2 = array_first_element(sa2, mend);
    if (!sa2) goto fail_state;
    char sa2_hex[MDG1_VARIANT_SA2_MAX_BYTES * 2 + 2];
    if (!extract_string(sa2, mend, sa2_hex, sizeof(sa2_hex))) goto fail_state;
    size_t hex_len = strlen(sa2_hex);
    if ((hex_len & 1) != 0) goto fail_state;
    if (hex_len / 2 > MDG1_VARIANT_SA2_MAX_BYTES) goto fail_size;
    for (size_t i = 0; i < hex_len / 2; i++) {
        int b = hex_pair(sa2_hex + i * 2);
        if (b < 0) goto fail_state;
        out->sa2_script[i] = (uint8_t)b;
    }
    out->sa2_script_len = hex_len / 2;

    /* flash_sections_by_boxcode[bc] */
    const char *fsbb = find_obj_key(v_obj, mend, "flash_sections_by_boxcode");
    if (!fsbb) goto fail_state;
    const char *bc_arr = find_obj_key(fsbb, mend, boxcode_space);
    if (!bc_arr) goto fail_notfound;
    bc_arr = skip_ws(bc_arr, mend);
    if (bc_arr >= mend || *bc_arr != '[') goto fail_state;
    const char *el = array_first_element(bc_arr, mend);
    if (!el) goto fail_state;
    size_t sec_i = 0;
    while (el < mend && *el != ']') {
        if (sec_i >= MDG1_VARIANT_MAX_SECTIONS) goto fail_size;
        mdg1_variant_section_t *s = &out->sections[sec_i];

        const char *bid = find_obj_key(el, mend, "block_id");
        if (!bid) goto fail_state;
        long bid_v = 0;
        if (!extract_int(bid, mend, &bid_v)) goto fail_state;
        s->block_id = (uint8_t)bid_v;

        const char *pts = find_obj_key(el, mend, "plaintext_size");
        if (!pts) goto fail_state;
        if (!extract_hex_or_dec_uint32(pts, mend, &s->plaintext_size)) goto fail_state;

        const char *adr = find_obj_key(el, mend, "ecu_address");
        if (!adr) goto fail_state;
        if (!extract_hex_or_dec_uint32(adr, mend, &s->ecu_address)) goto fail_state;

        const char *crc = find_obj_key(el, mend, "expected_crc32");
        if (!crc) goto fail_state;
        if (!extract_hex_or_dec_uint32(crc, mend, &s->expected_crc32)) goto fail_state;

        const char *foff = find_obj_key(el, mend, "file_offset");
        if (!foff) goto fail_state;
        if (!extract_hex_or_dec_uint32(foff, mend, &s->file_offset)) goto fail_state;

        const char *flen = find_obj_key(el, mend, "file_length");
        if (!flen) goto fail_state;
        if (!extract_hex_or_dec_uint32(flen, mend, &s->file_length)) goto fail_state;

        const char *nm = find_obj_key(el, mend, "name");
        if (!nm) goto fail_state;
        if (!extract_string(nm, mend, s->name, sizeof(s->name))) goto fail_state;

        sec_i++;
        el = array_next_element(el, mend);
    }
    out->section_count = sec_i;
    if (out->section_count == 0) goto fail_notfound;

    /* plaintext_source_by_boxcode[bc].bin_path (used for shadow-mode plaintext slicing). */
    const char *psbb = find_obj_key(v_obj, mend, "plaintext_source_by_boxcode");
    if (psbb) {
        const char *src = find_obj_key(psbb, mend, boxcode_space);
        if (src) {
            const char *bp = find_obj_key(src, mend, "bin_path");
            if (bp) {
                extract_string(bp, mend, out->plaintext_bin_path,
                               sizeof(out->plaintext_bin_path));
            }
        }
    }
    /* If plaintext_bin_path is empty, the orchestrator will reject the
     * plan at start time; we don't error here because firmware-mode
     * loads use a different plaintext source path. */

    /* ---- Step 3: aes_keys_per_boxcode.json → key location ---- */
    /* aes_keys is a TOP-LEVEL ARRAY of entries; find the one whose
     * "boxcode" matches the caller's boxcode (the underscore form). */
    const char *kp = keys;
    kp = skip_ws(kp, kend);
    if (kp >= kend || *kp != '[') goto fail_state;
    kp = array_first_element(kp, kend);
    if (!kp) goto fail_state;

    char key_bin_path[MDG1_VARIANT_BIN_PATH_MAX] = {0};
    long key_offset = 0;
    long key_length_bytes = 0;
    char fingerprint_hex[16] = {0};
    bool found_key_entry = false;

    while (kp < kend && *kp != ']') {
        const char *bc = find_obj_key(kp, kend, "boxcode");
        if (!bc) goto fail_state;
        char this_bc[MDG1_VARIANT_BOXCODE_MAX];
        if (!extract_string(bc, kend, this_bc, sizeof(this_bc))) goto fail_state;
        if (strcmp(this_bc, out->boxcode) == 0) {
            const char *ak = find_obj_key(kp, kend, "aes_key");
            if (!ak) goto fail_notfound;
            const char *bp = find_obj_key(ak, kend, "bin_path");
            const char *of = find_obj_key(ak, kend, "offset");
            const char *lb = find_obj_key(ak, kend, "length_bytes");
            const char *fp = find_obj_key(ak, kend, "sha256_first8_fingerprint");
            if (!bp || !of || !lb || !fp) goto fail_state;
            if (!extract_string(bp, kend, key_bin_path, sizeof(key_bin_path))) goto fail_state;
            if (!extract_int(of, kend, &key_offset)) goto fail_state;
            if (!extract_int(lb, kend, &key_length_bytes)) goto fail_state;
            if (!extract_string(fp, kend, fingerprint_hex, sizeof(fingerprint_hex))) goto fail_state;
            found_key_entry = true;
            break;
        }
        kp = array_next_element(kp, kend);
    }
    if (!found_key_entry) goto fail_notfound;
    if (key_length_bytes != 16) goto fail_size;

    /* ---- Step 4: read 16 key bytes from bin + validate fingerprint. ---- */
#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
    FILE *kf = fopen(key_bin_path, "rb");
    if (!kf) goto fail_state;
    if (fseek(kf, key_offset, SEEK_SET) != 0) { fclose(kf); goto fail_state; }
    if (fread(out->aes_key, 1, 16, kf) != 16) { fclose(kf); goto fail_state; }
    fclose(kf);
#else
    /* Firmware path: TODO load from SPIFFS. Not exercised this prompt. */
    goto fail_state;
#endif

    uint8_t h[32];
    sha256(out->aes_key, 16, h);
    char actual_fp[9];
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        actual_fp[i*2]   = hexd[(h[i] >> 4) & 0xF];
        actual_fp[i*2+1] = hexd[h[i] & 0xF];
    }
    actual_fp[8] = '\0';

    if (strcmp(actual_fp, fingerprint_hex) != 0) {
        /* Zeroize the key buffer before bailing — defensive. */
        memset(out->aes_key, 0, sizeof(out->aes_key));
#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
        free(manifest); free(keys);
#endif
        return ESP_ERR_INVALID_CRC;
    }

    /* ---- Step 5: fixed Bosch IV (from mdg1_payload_config.h). ---- */
    static const uint8_t bosch_iv[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
    };
    memcpy(out->aes_iv, bosch_iv, 16);

#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
    free(manifest); free(keys);
#endif
    return ESP_OK;

fail_state:
#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
    if (manifest) free(manifest);
    if (keys) free(keys);
#endif
    memset(out, 0, sizeof(*out));
    return ESP_ERR_INVALID_STATE;

fail_notfound:
#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
    if (manifest) free(manifest);
    if (keys) free(keys);
#endif
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_FOUND;

fail_size:
#ifdef MDG1_VARIANT_MANIFEST_HOST_BUILD
    if (manifest) free(manifest);
    if (keys) free(keys);
#endif
    memset(out, 0, sizeof(*out));
    return ESP_ERR_INVALID_SIZE;
}
