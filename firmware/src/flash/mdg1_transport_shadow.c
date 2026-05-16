/*
 * mdg1_transport_shadow.c — host-side shadow transport.
 *
 * Strategy:
 *   - On send_request: write "TX <hex>" line to the log. Stash the
 *     request bytes for the next recv_response.
 *   - On recv_response: choose a response by:
 *       (1) For SecurityAccess seed/key, fingerprint write, TesterPresent
 *           — synthesize procedurally (these are session-variant and the
 *           diff tool masks them anyway).
 *       (2) For all other requests — look up the next playback entry
 *           in expected_responses.json whose `request_prefix_hex` matches
 *           the head of the current request. Return that entry's
 *           `response_hex`.
 *   - Write "RX <hex>" line to the log.
 *
 * The playback is ORDERED: each request type advances a per-key cursor,
 * matching the order MM saw them in the original capture.
 *
 * Host-only. NOT included in firmware build.
 */

#include "mdg1_transport_shadow.h"
#include "mdg1_flash_orchestrator_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Expected-responses playback fixture                                */
/* ------------------------------------------------------------------ */

/*
 * The fixture JSON has the shape (produced by tools/extract_mm_expected_responses.py):
 *
 *   {
 *     "boxcode": "4K0907557G__0003",
 *     "responses": [
 *       {"tx_hex": "1003",          "rx_hex": "50031E01E0"},
 *       {"tx_hex": "22F190",        "rx_hex": "62F190...VIN..."},
 *       ...
 *     ]
 *   }
 *
 * We parse this with the same minimal JSON helpers used elsewhere.
 */

typedef struct {
    char *tx_hex;
    char *rx_hex;
} response_pair_t;

typedef struct {
    response_pair_t *entries;
    size_t           count;
    size_t           cursor;   /* index of next unconsumed entry */
    char            *raw_buf;  /* heap buffer holding the entries' string storage */
} response_playback_t;

/* Search inside JSON for the array of {"tx_hex": ..., "rx_hex": ...} objects.
 * Returns malloc'd response_playback_t* or NULL on parse failure. */
static response_playback_t *load_responses(const char *json_path)
{
    FILE *f = fopen(json_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    buf[sz] = '\0';

    /* Find the "responses": [ array. */
    char *p = strstr(buf, "\"responses\"");
    if (!p) { free(buf); return NULL; }
    p = strchr(p, '[');
    if (!p) { free(buf); return NULL; }
    p++;

    /* First pass: count {} elements at top level. */
    size_t count = 0;
    {
        char *q = p;
        int depth = 0;
        bool in_str = false;
        while (*q && !(depth == 0 && *q == ']')) {
            if (in_str) {
                if (*q == '\\' && q[1]) q++;
                else if (*q == '"') in_str = false;
                q++; continue;
            }
            if (*q == '"') { in_str = true; q++; continue; }
            if (*q == '{') { if (depth == 0) count++; depth++; q++; continue; }
            if (*q == '}') { depth--; q++; continue; }
            if (*q == '[') { depth++; q++; continue; }
            if (*q == ']') { depth--; q++; continue; }
            q++;
        }
    }

    response_playback_t *pb = (response_playback_t *)calloc(1, sizeof(*pb));
    if (!pb) { free(buf); return NULL; }
    pb->entries = (response_pair_t *)calloc(count, sizeof(response_pair_t));
    if (count > 0 && !pb->entries) { free(pb); free(buf); return NULL; }
    pb->count = count;
    pb->cursor = 0;
    pb->raw_buf = buf;

    /* Second pass: extract tx_hex / rx_hex for each entry. */
    size_t idx = 0;
    int depth = 0;
    bool in_str = false;
    char *q = p;
    char *el_start = NULL;
    while (*q && !(depth == 0 && *q == ']')) {
        if (in_str) {
            if (*q == '\\' && q[1]) q++;
            else if (*q == '"') in_str = false;
            q++; continue;
        }
        if (*q == '"') { in_str = true; q++; continue; }
        if (*q == '{') {
            if (depth == 0) el_start = q;
            depth++; q++; continue;
        }
        if (*q == '}') {
            depth--;
            if (depth == 0 && el_start && idx < count) {
                /* Find "tx_hex": "..." and "rx_hex": "..." within el_start..q. */
                char *txk = strstr(el_start, "\"tx_hex\"");
                char *rxk = strstr(el_start, "\"rx_hex\"");
                if (txk && rxk && txk < q && rxk < q) {
                    char *txv = strchr(txk, '"'); if (txv) txv = strchr(txv+1, ':');
                    char *rxv = strchr(rxk, '"'); if (rxv) rxv = strchr(rxv+1, ':');
                    if (txv && rxv) {
                        txv = strchr(txv, '"');
                        rxv = strchr(rxv, '"');
                        if (txv && rxv) {
                            char *tx_end = strchr(txv + 1, '"');
                            char *rx_end = strchr(rxv + 1, '"');
                            if (tx_end && rx_end) {
                                /* Replace closing quotes with NUL so the
                                 * strings are valid in-place. */
                                *tx_end = '\0';
                                *rx_end = '\0';
                                pb->entries[idx].tx_hex = txv + 1;
                                pb->entries[idx].rx_hex = rxv + 1;
                                idx++;
                            }
                        }
                    }
                }
                el_start = NULL;
            }
            q++; continue;
        }
        if (*q == '[') { depth++; q++; continue; }
        if (*q == ']') { depth--; q++; continue; }
        q++;
    }
    pb->count = idx;
    return pb;
}

static void free_responses(response_playback_t *pb)
{
    if (!pb) return;
    free(pb->entries);
    free(pb->raw_buf);
    free(pb);
}

/* Decode a hex string into a byte buffer. Returns -1 on bad input. */
static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_cap)
{
    size_t hlen = strlen(hex);
    if ((hlen & 1) != 0) return -1;
    size_t blen = hlen / 2;
    if (blen > out_cap) return -1;
    for (size_t i = 0; i < blen; i++) {
        char a = hex[i*2], b = hex[i*2+1];
        int hi, lo;
        if      (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = 10 + a - 'a';
        else if (a >= 'A' && a <= 'F') hi = 10 + a - 'A';
        else return -1;
        if      (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = 10 + b - 'a';
        else if (b >= 'A' && b <= 'F') lo = 10 + b - 'A';
        else return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)blen;
}

/* Lowercase + canonicalize for prefix comparison. */
static void canon_hex_buf(const uint8_t *bytes, size_t len, char *out)
{
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i*2]   = hexd[(bytes[i] >> 4) & 0xF];
        out[i*2+1] = hexd[bytes[i] & 0xF];
    }
    out[len*2] = '\0';
}

/* Heap-alloc hex string sized for `len` bytes; caller must free.
 * Stack-alloc was MDG1_MAX_UDS_MESSAGE_BYTES * 2 + 4 = 8204 bytes per
 * call — fine on host where main has megabytes of stack, but on
 * ESP32-S3 main_task it overflows immediately. Heap is plentiful. */
static char *hex_alloc_and_canon(const uint8_t *bytes, size_t len)
{
    char *out = (char *)malloc(len * 2 + 1);
    if (!out) return NULL;
    canon_hex_buf(bytes, len, out);
    return out;
}

/* ------------------------------------------------------------------ */
/* Shadow transport context                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE                *log;          /* shadow log file */
    response_playback_t *pb;
    uint8_t              last_tx[MDG1_MAX_UDS_MESSAGE_BYTES];
    size_t               last_tx_len;
    uint32_t             security_seed_be;  /* synthesized for 27 11 response */
} shadow_ctx_t;

/* ------------------------------------------------------------------ */
/* Response synthesis for session-variant frames                      */
/* ------------------------------------------------------------------ */

static int synth_session_variant_response(shadow_ctx_t *sx,
                                          const uint8_t *req, size_t req_len,
                                          uint8_t *out, size_t out_cap)
{
    if (req_len < 1) return -1;
    /* SecurityAccess request seed (0x27 0x11) → 0x67 0x11 + 4-byte seed */
    if (req_len >= 2 && req[0] == MDG1_UDS_SID_SECURITY_ACCESS &&
        req[1] == MDG1_SECURITY_LEVEL_SEED) {
        if (out_cap < 6) return -1;
        out[0] = 0x67;
        out[1] = MDG1_SECURITY_LEVEL_SEED;
        out[2] = (uint8_t)(sx->security_seed_be >> 24);
        out[3] = (uint8_t)(sx->security_seed_be >> 16);
        out[4] = (uint8_t)(sx->security_seed_be >> 8);
        out[5] = (uint8_t)(sx->security_seed_be);
        return 6;
    }
    /* SecurityAccess send key (0x27 0x12 + 4) → 0x67 0x12 */
    if (req_len >= 2 && req[0] == MDG1_UDS_SID_SECURITY_ACCESS &&
        req[1] == MDG1_SECURITY_LEVEL_KEY) {
        if (out_cap < 2) return -1;
        out[0] = 0x67;
        out[1] = MDG1_SECURITY_LEVEL_KEY;
        return 2;
    }
    /* WriteDataByIdentifier fingerprint (0x2E F1 5A …) → 0x6E F1 5A */
    if (req_len >= 3 && req[0] == MDG1_UDS_SID_WRITE_DID &&
        req[1] == 0xF1 && req[2] == 0x5A) {
        if (out_cap < 3) return -1;
        out[0] = 0x6E; out[1] = 0xF1; out[2] = 0x5A;
        return 3;
    }
    /* TesterPresent (0x3E 0x00) → 0x7E 0x00 */
    if (req_len >= 2 && req[0] == MDG1_UDS_SID_TESTER_PRESENT &&
        req[1] == MDG1_TESTER_PRESENT_SUBFUNCTION) {
        if (out_cap < 2) return -1;
        out[0] = 0x7E; out[1] = 0x00;
        return 2;
    }
    return 0;  /* not a session-variant frame */
}

/* ------------------------------------------------------------------ */
/* iface callbacks                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t shadow_send(void *ctx, const uint8_t *data, size_t len)
{
    shadow_ctx_t *sx = (shadow_ctx_t *)ctx;
    if (!sx || !sx->log) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > sizeof(sx->last_tx)) return ESP_ERR_INVALID_SIZE;

    /* Log "TX <HEX>" line. */
    char *hex_buf = hex_alloc_and_canon(data, len);
    if (!hex_buf) return ESP_ERR_INVALID_STATE;
    fprintf(sx->log, "TX %s\n", hex_buf);
    free(hex_buf);

    /* Stash for the upcoming recv_response. */
    memcpy(sx->last_tx, data, len);
    sx->last_tx_len = len;
    return ESP_OK;
}

static esp_err_t shadow_recv(void *ctx, uint8_t *buf, size_t cap,
                             size_t *out_len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    shadow_ctx_t *sx = (shadow_ctx_t *)ctx;
    if (!sx || !sx->log) return ESP_ERR_INVALID_STATE;
    if (!buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_len = 0;

    /* (1) Session-variant procedural response? */
    int n = synth_session_variant_response(sx, sx->last_tx, sx->last_tx_len,
                                           buf, cap);
    if (n > 0) {
        *out_len = (size_t)n;
        char *hex_buf = hex_alloc_and_canon(buf, (size_t)n);
        if (!hex_buf) return ESP_ERR_INVALID_STATE;
        fprintf(sx->log, "RX %s\n", hex_buf);
        free(hex_buf);
        return ESP_OK;
    }

    /* (2) Procedural defaults for the well-known flash-critical SIDs.
     * We do this BEFORE playback because the extracted playback fixture
     * may be mis-paired (TX/RX alignment is fragile when the source
     * candump has interleaved pending negative responses and our
     * extractor's pairing heuristic doesn't perfectly recover the
     * 1:1 mapping). Procedural responses for these SIDs are exact-shape
     * by construction and don't depend on accurate playback. */
    if (sx->last_tx_len >= 1) {
        uint8_t sid = sx->last_tx[0];
        size_t n2 = 0;
        switch (sid) {
            case MDG1_UDS_SID_DIAG_SESSION:
                if (cap >= 6 && sx->last_tx_len >= 2) {
                    buf[0]=0x50; buf[1]=sx->last_tx[1];
                    buf[2]=0x00; buf[3]=0x32; buf[4]=0x01; buf[5]=0xF4;
                    n2 = 6;
                }
                break;
            case MDG1_UDS_SID_ECU_RESET:
                if (cap >= 2 && sx->last_tx_len >= 2) {
                    buf[0]=0x51; buf[1]=sx->last_tx[1]; n2 = 2;
                }
                break;
            case MDG1_UDS_SID_ROUTINE_CONTROL:
                /* 71 01 <RID hi> <RID lo> 00 */
                if (cap >= 5 && sx->last_tx_len >= 4) {
                    buf[0]=0x71; buf[1]=0x01;
                    buf[2]=sx->last_tx[2]; buf[3]=sx->last_tx[3];
                    buf[4]=0x00; n2 = 5;
                }
                break;
            case MDG1_UDS_SID_REQUEST_DOWNLOAD:
                if (cap >= 4) {
                    buf[0]=0x74; buf[1]=0x20; buf[2]=0x0F; buf[3]=0xFF;
                    n2 = 4;
                }
                break;
            case MDG1_UDS_SID_TRANSFER_DATA:
                if (cap >= 2 && sx->last_tx_len >= 2) {
                    buf[0]=0x76; buf[1]=sx->last_tx[1]; n2 = 2;
                }
                break;
            case MDG1_UDS_SID_REQUEST_TRANSFER_EXIT:
                /* MM emits just `77` (1 byte). The MM analysis doc shows
                 * `77 00` but the 0x00 is ISO-TP padding inside the
                 * 8-byte CAN frame, not part of the UDS message. */
                if (cap >= 1) { buf[0]=0x77; n2 = 1; }
                break;
            case MDG1_UDS_SID_CLEAR_DTC:
                if (cap >= 3) { buf[0]=0x7F; buf[1]=0x14; buf[2]=0x11; n2 = 3; }
                break;
            default:
                break;
        }
        if (n2 > 0) {
            *out_len = n2;
            char *rx_hex = hex_alloc_and_canon(buf, n2);
            if (!rx_hex) return ESP_ERR_INVALID_STATE;
            fprintf(sx->log, "RX %s\n", rx_hex);
            free(rx_hex);
            return ESP_OK;
        }
    }

    /* (3) Playback fallback by prefix-match against next available entry.
     * Used for SIDs procedural doesn't handle (e.g., 22 DID reads where
     * the response payload varies per DID and only the fixture knows it). */
    if (sx->pb && sx->pb->cursor < sx->pb->count) {
        /* Compute lowercase hex of the request to match against
         * playback's tx_hex. */
        char *req_hex = hex_alloc_and_canon(sx->last_tx, sx->last_tx_len);
        if (!req_hex) return ESP_ERR_INVALID_STATE;

        /* Linear scan from cursor for a tx_hex that equals or is a
         * prefix of req_hex. Returning prefix match is intentional —
         * playback entries can carry full-message responses keyed by
         * their head (e.g. "22F190" matches any 22 F190 ... read). */
        for (size_t i = sx->pb->cursor; i < sx->pb->count; i++) {
            const char *want = sx->pb->entries[i].tx_hex;
            if (!want) continue;
            size_t wlen = strlen(want);
            if (wlen > strlen(req_hex)) continue;
            if (memcmp(req_hex, want, wlen) != 0) continue;
            /* Match. Emit this entry's response. */
            int got = hex_to_bytes(sx->pb->entries[i].rx_hex, buf, cap);
            if (got < 0) { free(req_hex); return ESP_ERR_INVALID_STATE; }
            *out_len = (size_t)got;
            char *rx_hex = hex_alloc_and_canon(buf, (size_t)got);
            if (!rx_hex) { free(req_hex); return ESP_ERR_INVALID_STATE; }
            fprintf(sx->log, "RX %s\n", rx_hex);
            free(rx_hex);
            sx->pb->cursor = i + 1;
            free(req_hex);
            return ESP_OK;
        }
        free(req_hex);
    }

    /* (4) Read-DID fallback (procedural minimal echo). Lower priority
     * than playback because the fixture has the real payloads. */
    if (sx->last_tx_len >= 3 && sx->last_tx[0] == MDG1_UDS_SID_READ_DID) {
        if (cap >= 4) {
            buf[0]=0x62; buf[1]=sx->last_tx[1]; buf[2]=sx->last_tx[2]; buf[3]=0x00;
            *out_len = 4;
            char *rx_hex = hex_alloc_and_canon(buf, 4);
            if (!rx_hex) return ESP_ERR_INVALID_STATE;
            fprintf(sx->log, "RX %s\n", rx_hex);
            free(rx_hex);
            return ESP_OK;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t shadow_flush(void *ctx)
{
    shadow_ctx_t *sx = (shadow_ctx_t *)ctx;
    if (!sx) return ESP_ERR_INVALID_STATE;
    sx->last_tx_len = 0;
    if (sx->log) fflush(sx->log);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t mdg1_transport_shadow_open(const char *out_log_path,
                                     const char *expected_responses_json_path,
                                     mdg1_uds_transport_t *out_iface)
{
    if (!out_log_path || !out_iface) return ESP_ERR_INVALID_ARG;
    memset(out_iface, 0, sizeof(*out_iface));

    shadow_ctx_t *sx = (shadow_ctx_t *)calloc(1, sizeof(*sx));
    if (!sx) return ESP_ERR_INVALID_STATE;
    sx->security_seed_be = MDG1_SHADOW_SECURITY_SEED_PLACEHOLDER;

    sx->log = fopen(out_log_path, "wb");
    if (!sx->log) { free(sx); return ESP_ERR_INVALID_STATE; }

    if (expected_responses_json_path) {
        sx->pb = load_responses(expected_responses_json_path);
        /* Missing fixture is non-fatal — procedural fallbacks handle
         * most service codes. The diff tool will catch any divergence. */
    }

    out_iface->send_request  = shadow_send;
    out_iface->recv_response = shadow_recv;
    out_iface->flush         = shadow_flush;
    out_iface->ctx           = sx;
    return ESP_OK;
}

void mdg1_transport_shadow_close(mdg1_uds_transport_t *iface)
{
    if (!iface || !iface->ctx) return;
    shadow_ctx_t *sx = (shadow_ctx_t *)iface->ctx;
    if (sx->log) { fflush(sx->log); fclose(sx->log); sx->log = NULL; }
    if (sx->pb) { free_responses(sx->pb); sx->pb = NULL; }
    free(sx);
    memset(iface, 0, sizeof(*iface));
}

void mdg1_transport_shadow_set_seed(mdg1_uds_transport_t *iface, uint32_t seed_be)
{
    if (!iface || !iface->ctx) return;
    shadow_ctx_t *sx = (shadow_ctx_t *)iface->ctx;
    sx->security_seed_be = seed_be;
}
