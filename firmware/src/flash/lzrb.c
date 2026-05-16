#include "lzrb.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Bit reader                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         byte_pos;
    int            bit_pos;   /* 0..7, MSB-first; 0 means next read is bit 7 */
    int            error;     /* sticky: set when we run past end */
} bit_reader_t;

static void br_init(bit_reader_t *r, const uint8_t *buf, size_t len) {
    r->buf = buf;
    r->len = len;
    r->byte_pos = 0;
    r->bit_pos = 0;
    r->error = 0;
}

static uint32_t br_read(bit_reader_t *r, int n) {
    uint32_t out = 0;
    while (n-- > 0) {
        if (r->byte_pos >= r->len) {
            r->error = 1;
            return 0;
        }
        int bit = (r->buf[r->byte_pos] >> (7 - r->bit_pos)) & 1;
        out = (out << 1) | (uint32_t)bit;
        if (++r->bit_pos == 8) {
            r->bit_pos = 0;
            r->byte_pos++;
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Bit writer (MSB-first, mirror of Java BitSequenceWriter)            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   byte_pos;
    uint32_t buffer;       /* up to 24 bits queued */
    int      bits_in_buf;
    int      error;        /* sticky: out_cap exceeded */
} bit_writer_t;

static void bw_init(bit_writer_t *w, uint8_t *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->byte_pos = 0;
    w->buffer = 0;
    w->bits_in_buf = 0;
    w->error = 0;
}

static void bw_write(bit_writer_t *w, uint32_t bits, int count) {
    /* The Java code does `buffer = buffer << count | bits`; if `bits` has
     * extraneous high bits (it shouldn't given the encoder's contract, but
     * we mirror exactly), they shift into earlier-emitted bytes. Mask to
     * be safe against caller bugs. */
    if (count < 32) bits &= (1u << count) - 1u;
    w->buffer = (w->buffer << count) | bits;
    w->bits_in_buf += count;
    while (w->bits_in_buf >= 8) {
        if (w->byte_pos >= w->cap) {
            w->error = 1;
            return;
        }
        w->buf[w->byte_pos++] = (uint8_t)(w->buffer >> (w->bits_in_buf - 8));
        w->bits_in_buf -= 8;
        if (w->bits_in_buf > 0) {
            w->buffer &= (1u << w->bits_in_buf) - 1u;
        } else {
            w->buffer = 0;
        }
    }
}

/* Pad current byte with zero bits, then close. Mirrors BitSequenceWriter.close(). */
static void bw_flush(bit_writer_t *w) {
    if (w->bits_in_buf > 0) {
        bw_write(w, 0, 8 - w->bits_in_buf);
    }
}

/* ------------------------------------------------------------------ */
/* Status string                                                       */
/* ------------------------------------------------------------------ */

const char *lzrb_status_str(lzrb_status_t s) {
    switch (s) {
    case LZRB_OK:                    return "ok";
    case LZRB_ERR_INPUT_EXHAUSTED:   return "input exhausted mid-token";
    case LZRB_ERR_OUTPUT_OVERFLOW:   return "output capacity exceeded";
    case LZRB_ERR_BAD_OFFSET:        return "back-reference before start";
    case LZRB_ERR_INTERNAL:          return "internal invariant broken";
    default:                         return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Decompressor                                                        */
/* ------------------------------------------------------------------ */

static uint32_t decode_offset(bit_reader_t *r) {
    uint32_t prefix = br_read(r, 3);
    if (prefix == 7) {
        return br_read(r, 11);
    }
    int bits = (int)prefix + 3;
    uint32_t payload = br_read(r, bits);
    /* wire = payload + (1 << bits) - 8.  For prefix=0: wire = payload. */
    return payload + ((uint32_t)1 << bits) - 8u;
}

static uint32_t decode_length(bit_reader_t *r) {
    /* count leading zeros until the leading 1 */
    int bits = 0;
    while (br_read(r, 1) == 0) {
        if (r->error) return 0;
        bits++;
        if (bits > 11) {
            /* length > 2048 is impossible given the 2048-byte sliding
             * window. Treat as input corruption. */
            r->error = 1;
            return 0;
        }
    }
    /* the leading 1 is the high bit of (length - 1); read `bits` more bits */
    uint32_t v = (uint32_t)1 << bits;
    if (bits > 0) {
        v |= br_read(r, bits);
    }
    return v + 1;
}

static uint32_t inverse_transform_offset(uint32_t wire) {
    /* The encoder's transformOffset() maps input 2048 → wire 0 (a legit
     * reference to the start of the 2048-byte window) and input 0 → wire
     * 1024 (used only at termination, never as a real reference). So
     * wire=0 must invert to 2048, not 0. */
    if (wire == 0) return 2048;
    if (wire >= 1024) {
        return ((wire - 1024) << 1) | 1u;   /* odd input */
    }
    return wire << 1;                        /* even input, wire in [1, 1023] */
}

lzrb_status_t lzrb_decompress(const uint8_t *in,
                              size_t        in_len,
                              uint8_t       *out,
                              size_t        out_cap,
                              size_t        expected_out_len,
                              size_t       *out_len_actual)
{
    if (out_len_actual) *out_len_actual = 0;
    if (expected_out_len > out_cap) return LZRB_ERR_OUTPUT_OVERFLOW;

    bit_reader_t r;
    br_init(&r, in, in_len);
    size_t produced = 0;

    while (produced < expected_out_len) {
        uint32_t flag = br_read(&r, 1);
        if (r.error) return LZRB_ERR_INPUT_EXHAUSTED;

        if (flag == 0) {
            uint32_t b = br_read(&r, 8);
            if (r.error) return LZRB_ERR_INPUT_EXHAUSTED;
            out[produced++] = (uint8_t)b;
            continue;
        }

        /* back-reference */
        uint32_t wire_off = decode_offset(&r);
        if (r.error) return LZRB_ERR_INPUT_EXHAUSTED;
        uint32_t actual_off = inverse_transform_offset(wire_off);
        uint32_t length = decode_length(&r);
        if (r.error) return LZRB_ERR_INPUT_EXHAUSTED;

        if (actual_off == 0) return LZRB_ERR_INTERNAL;  /* unreachable: wire=0 → 2048 */
        if ((size_t)actual_off > produced) return LZRB_ERR_BAD_OFFSET;
        if (produced + length > expected_out_len) {
            /* The reference would over-fill — clamp to keep the API
             * contract that out_len_actual <= expected_out_len. */
            length = (uint32_t)(expected_out_len - produced);
        }
        /* Byte-by-byte copy because src may overlap dest (RLE-style refs). */
        for (uint32_t i = 0; i < length; i++) {
            out[produced + i] = out[produced + i - actual_off];
        }
        produced += length;
    }

    if (out_len_actual) *out_len_actual = produced;
    return (produced == expected_out_len) ? LZRB_OK : LZRB_ERR_INPUT_EXHAUSTED;
}

/* ------------------------------------------------------------------ */
/* Compressor                                                          */
/* ------------------------------------------------------------------ */

/*
 * Mechanical port of Encoder.java + SlidingStringBuffer.java + BitEncoder.java.
 *
 * Java naming → C naming:
 *   sb.length()                → win_size
 *   sb.charAt(pos)             → win[(win_origin + pos) & MASK] (logical pos 0..win_size)
 *   matches / newMatches       → match_pos[] arrays of in-window offsets
 *   numMatched                 → run_len  (length of the candidate match in progress)
 *   patternLengths[1,2,4]      → PATTERN_LENS
 *   getPatternLength(i)        → pat_len[i] — current run length of the pattern of size PATTERN_LENS[i]
 *   arrFirstIndex[256]         → first_index[256]
 *   arrNextIndex[2*WIN_MAX]    → next_index[2*WIN_MAX]
 *
 * The Java code keeps the "logical" sliding window as a StringBuffer that
 * grows up to 2*MAX, then relocates by deleting the first MAX. Indices in
 * first_index / next_index are absolute into that buffer. We use a flat
 * 2*MAX byte buffer + an `origin` cursor that advances on relocate(), which
 * matches the Java semantics 1:1.
 */

#define WIN_MAX 2048
#define WIN_BUF (2 * WIN_MAX)

typedef struct {
    uint8_t  buf[WIN_BUF];
    int      origin;     /* logical position 0 corresponds to buf[origin] */
    int      buf_len;    /* current valid length of buf (== origin + size + append_size) */
    int      size;       /* "committed" length within the window */
    int      append_size;
    int      pat_len[3]; /* current run lengths for PATTERN_LENS = {1,2,4} */
    int      first_index[256];
    int      next_index[WIN_BUF];
} sliding_t;

static const int PATTERN_LENS[3] = { 1, 2, 4 };

static void sl_init(sliding_t *s) {
    /* `buf` and counters live on the stack — the encoder caller decides
     * lifetime. Memset only what matters. */
    s->origin = 0;
    s->buf_len = 0;
    s->size = 0;
    s->append_size = 0;
    s->pat_len[0] = s->pat_len[1] = s->pat_len[2] = 0;
    for (int i = 0; i < 256; i++) s->first_index[i] = -1;
    for (int i = 0; i < WIN_BUF; i++) s->next_index[i] = -1;
}

static int sl_charAt(const sliding_t *s, int pos) {
    return s->buf[s->origin + pos];
}

static int sl_lastChar(const sliding_t *s) {
    return s->buf[s->buf_len - 1];
}

static int sl_length(const sliding_t *s) {
    return s->size;
}

static int sl_getPatternLength(const sliding_t *s, int ix) {
    return s->pat_len[ix];
}

static void sl_checkPatterns(sliding_t *s, int c) {
    for (int i = 0; i < 3; i++) {
        int pl = PATTERN_LENS[i];
        if (s->size > pl) {
            int prev = s->buf[s->origin + s->size - pl];
            s->pat_len[i] = (c == prev) ? ((s->pat_len[i] + 1) < WIN_MAX ? (s->pat_len[i] + 1) : WIN_MAX) : pl;
        }
    }
}

static void sl_relocate(sliding_t *s, int moveBy) {
    /* delete first moveBy bytes; shift indices by -moveBy */
    int new_len = s->buf_len - moveBy;
    memmove(s->buf, s->buf + moveBy, (size_t)new_len);
    s->buf_len = new_len;
    for (int i = 0; i < 256; i++) s->first_index[i] -= moveBy;
    for (int i = 0; i < WIN_BUF - moveBy; i++) s->next_index[i] = s->next_index[i + moveBy] - moveBy;
    /* tail is left at -moveBy values; reset for cleanliness */
    for (int i = WIN_BUF - moveBy; i < WIN_BUF; i++) s->next_index[i] = -1;
    s->origin -= moveBy;
}

static void sl_append(sliding_t *s, int c) {
    sl_checkPatterns(s, c);
    if (s->size < WIN_MAX) {
        s->next_index[s->buf_len] = s->first_index[c];
        s->first_index[c] = s->buf_len;
        s->buf[s->buf_len++] = (uint8_t)c;
        s->size++;
    } else {
        if (s->origin == WIN_MAX) {
            sl_relocate(s, s->origin);
        }
        s->next_index[s->buf_len] = s->first_index[c];
        s->first_index[c] = s->buf_len;
        s->buf[s->buf_len++] = (uint8_t)c;
        s->origin++;
    }
}

static void sl_appendLater(sliding_t *s, int c) {
    sl_checkPatterns(s, c);
    if (s->size + s->append_size < WIN_MAX) {
        s->buf[s->buf_len++] = (uint8_t)c;
        s->append_size++;
    } else {
        if (s->origin + s->size + s->append_size == WIN_BUF) {
            sl_relocate(s, WIN_MAX);
        }
        s->buf[s->buf_len++] = (uint8_t)c;
        s->append_size++;
    }
}

static void sl_release(sliding_t *s) {
    if (s->size < WIN_MAX) {
        int fill = (s->append_size < WIN_MAX - s->size) ? s->append_size : (WIN_MAX - s->size);
        for (int i = 0; i < fill; i++) {
            int absp = s->origin + s->size + i;
            int c = s->buf[absp];
            s->next_index[absp] = s->first_index[c];
            s->first_index[c] = absp;
        }
        s->size += fill;
        s->append_size -= fill;
    }
    if (s->size == WIN_MAX) {
        for (int i = 0; i < s->append_size; i++) {
            int absp = s->origin + s->size + i;
            int c = s->buf[absp];
            s->next_index[absp] = s->first_index[c];
            s->first_index[c] = absp;
        }
        s->origin += s->append_size;
        s->append_size = 0;
    }
}

static int sl_findChars(const sliding_t *s, int c, int *result, int cap) {
    int n = 0;
    int idx = s->first_index[c];
    while (idx >= s->origin) {
        if (n >= cap) break;
        result[n++] = idx - s->origin;
        idx = s->next_index[idx];
    }
    return n;
}

/* ------- Bit-level emitters: appendRaw / appendReference / appendLength / appendOffset ------- */

static int transform_offset(int offset) {
    if (offset == 0)    offset = 1;
    if (offset == 2048) offset = 0;
    if ((offset & 1) == 0) {
        return offset >> 1;
    }
    return 1024 | (offset >> 1);
}

static void enc_offset(bit_writer_t *w, int offset) {
    if (offset >= 1016) {
        bw_write(w, 7, 3);
        bw_write(w, (uint32_t)offset, 11);
    } else {
        int bits = 3;
        int max = 1 << bits;
        while (offset >= max) {
            offset -= max;
            ++bits;
            max <<= 1;
        }
        bw_write(w, (uint32_t)(bits - 3), 3);
        bw_write(w, (uint32_t)offset, bits);
    }
}

/* count = floor(log2(--length)); emit `count` zeros then `length` in count+1 bits */
static int ilog2_floor(int v) {
    /* v > 0 by construction (length >= 2 → length-1 >= 1) */
    int b = 0;
    while ((v >> b) > 1) b++;
    return b;
}

static void enc_length(bit_writer_t *w, int length) {
    int v = length - 1;
    int bits = ilog2_floor(v);
    for (int i = 0; i < bits; i++) bw_write(w, 0, 1);
    bw_write(w, (uint32_t)v, bits + 1);
}

static void enc_raw(bit_writer_t *w, int ch) {
    bw_write(w, 0, 1);
    bw_write(w, (uint32_t)(ch & 0xFF), 8);
}

static void enc_reference(bit_writer_t *w, int offset, int length) {
    int t_off = transform_offset(offset);
    bw_write(w, 1, 1);
    enc_offset(w, t_off);
    enc_length(w, length);
}

static void enc_terminate(bit_writer_t *w) {
    bw_write(w, 1, 1);
    enc_offset(w, transform_offset(0));
    bw_write(w, 0, 8);
    bw_flush(w);
}

/* ------------------------------------------------------------------ */
/* Compressor main loop — straight port of Encoder.java                */
/* ------------------------------------------------------------------ */

static void check_next_char(const sliding_t *sb,
                            const int *old_matches, int old_n,
                            int run_len, int c,
                            int *new_matches, int *new_n)
{
    int nn = 0;
    for (int i = 0; i < old_n; i++) {
        int pos = old_matches[i];
        if (pos + run_len < sl_length(sb) && sl_charAt(sb, pos + run_len) == c) {
            new_matches[nn++] = pos;
        }
    }
    *new_n = nn;
}

lzrb_status_t lzrb_compress(const uint8_t *in,
                            size_t        in_len,
                            uint8_t       *out,
                            size_t        out_cap,
                            size_t       *out_len_actual)
{
    if (out_len_actual) *out_len_actual = 0;

    bit_writer_t w;
    bw_init(&w, out, out_cap);

    sliding_t sb;
    sl_init(&sb);

    /* Two parallel match-position arrays, swapped on each character that
     * extends the current match. Cap at WIN_MAX (the worst case is "every
     * window position matches", e.g. all-zeros input). */
    static int matches[WIN_MAX];
    static int newm[WIN_MAX];
    int matches_n = 0;
    int newm_n = 0;
    int run_len = 0;

    size_t i_in = 0;
    while (i_in < in_len) {
        int c = in[i_in++];
        int new_search = 0;

        if (run_len > 0) {
            check_next_char(&sb, matches, matches_n, run_len, c, newm, &newm_n);
            if (newm_n > 0) {
                /* swap matches / newm */
                int tmp_n = matches_n; matches_n = newm_n; newm_n = tmp_n;
                int *tmp_p; (void)tmp_p; /* arrays are static, swap via memcpy */
                /* Faster: keep two arrays and swap pointers. We can't swap
                 * static array names, so copy newm into matches. */
                memcpy(matches, newm, sizeof(int) * (size_t)matches_n);
                run_len++;
                sl_appendLater(&sb, c);
            } else {
                if (run_len >= 2) {
                    int offset = sl_length(&sb) - matches[0];
                    enc_reference(&w, offset, run_len);
                } else {
                    enc_raw(&w, sl_lastChar(&sb));
                }
                matches_n = 0;
                run_len = 0;
                sl_release(&sb);
                new_search = 1;
            }
        } else {
            new_search = 1;
        }

        while (new_search) {
            new_search = 0;
            int offset = -1;
            int pattern[4];   /* PATTERN_LENS max is 4 */
            int pat_pl = 0;

            for (int idx = 0; idx < 3; idx++) {
                if (sl_getPatternLength(&sb, idx) == WIN_MAX) {
                    int pl = PATTERN_LENS[idx];
                    int found = 0;
                    for (int j = 0; j < pl; j++) {
                        if (c == sl_charAt(&sb, j)) { found = 1; break; }
                    }
                    if (found) {
                        for (int j = 0; j < pl; j++) {
                            pattern[j] = sl_charAt(&sb, j);
                            if (offset == -1 && c == pattern[j]) offset = j;
                        }
                        pat_pl = pl;
                        break;
                    }
                }
            }

            if (pat_pl > 0) {
                run_len = 1;
                sl_append(&sb, c);
                while (i_in < in_len && offset + run_len < WIN_MAX) {
                    c = in[i_in++];
                    if (c != pattern[(offset + run_len) % pat_pl]) {
                        int try_off = offset;
                        int matches_try = 0;
                        while (!matches_try && try_off + 1 < pat_pl && try_off + run_len < WIN_MAX) {
                            ++try_off;
                            matches_try = (c == pattern[(try_off + run_len) % pat_pl]);
                            if (!matches_try) continue;
                            for (int k = 0; k < run_len; k++) {
                                int sb_byte = sl_charAt(&sb, sl_length(&sb) - run_len + k);
                                int pat_byte = pattern[(try_off + k) % pat_pl];
                                if (sb_byte != pat_byte) { matches_try = 0; break; }
                            }
                        }
                        if (!matches_try) {
                            /* Java: "break" — exit with c held (not appended to sb).
                             * The post-loop logic re-enters the inner while(new_search)
                             * loop with bNewSearch=true, which re-uses the same c in
                             * pattern/hash-chain paths. We do NOT push back to i_in
                             * because c is fully processed by that retry; the next
                             * outer iteration reads the next byte. */
                            break;
                        }
                        offset = try_off;
                    }
                    sl_append(&sb, c);
                    run_len++;
                }
                if (run_len >= 2) {
                    int remain = WIN_MAX - offset - run_len;
                    int addOff = remain % pat_pl;
                    enc_reference(&w, run_len + addOff, run_len);
                    if (i_in < in_len && offset + run_len < WIN_MAX) {
                        new_search = 1;
                        run_len = 0;
                        continue;
                    }
                    run_len = 0;
                    continue;
                }
                if (run_len == 1) {
                    enc_raw(&w, pattern[offset]);
                    new_search = 1;
                    run_len = 0;
                    continue;
                }
                continue;
            }

            /* No pattern match: search hash chain for matching positions. */
            matches_n = sl_findChars(&sb, c, matches, WIN_MAX);
            if (matches_n == 0) {
                enc_raw(&w, c);
                sl_append(&sb, c);
                run_len = 0;
                continue;
            }
            sl_appendLater(&sb, c);
            run_len = 1;
        }

        if (w.error) return LZRB_ERR_OUTPUT_OVERFLOW;
    }

    /* Flush any in-progress run */
    if (run_len >= 2) {
        int last = matches[matches_n - 1];
        int offset = sl_length(&sb) - last;
        enc_reference(&w, offset, run_len);
    } else if (run_len == 1) {
        sl_release(&sb);
        enc_raw(&w, sl_lastChar(&sb));
    }

    enc_terminate(&w);
    if (w.error) return LZRB_ERR_OUTPUT_OVERFLOW;

    if (out_len_actual) *out_len_actual = w.byte_pos;
    return LZRB_OK;
}
