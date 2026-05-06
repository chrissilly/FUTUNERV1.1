#include "wot_recorder.h"
#include "wot_logger_config.h"

#include "esp_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// wot_recorder — see wot_recorder.h for the contract.
//
// State machine:
//   IDLE      no recording. Throttle below threshold OR recorder
//             disarmed. Samples are dropped.
//   RECORDING throttle crossed threshold; samples appended to the
//             in-memory buffer. Cooldown timer resets each time
//             throttle returns above threshold.
//   FLUSHING  end condition met (cooldown elapsed below threshold,
//             max-duration cap hit, or stop()/disarm called).
//             CSV is built, gzip wrapper applied, on_finish callback
//             invoked. Then back to IDLE.
//
// Gzip backend:
//   On target the deflate body comes from esp_rom_miniz. On host
//   we emit a stored DEFLATE block (uncompressed) so the structural
//   gzip check (header, CRC32, ISIZE) passes without linking system
//   zlib. Selection is at compile time via WOT_RECORDER_HOST_BUILD.

#ifdef WOT_RECORDER_HOST_BUILD
#  include <stdint.h>
   // Host build: stub miniz signatures so the surrounding code compiles
   // without esp_rom_miniz.h. The host emits stored blocks instead.
#else
#  include "miniz.h"
#endif

static const char *TAG = "WOT_REC";

// ------------------------------------------------------------------
// Internal state
// ------------------------------------------------------------------

typedef enum {
    REC_STATE_IDLE,
    REC_STATE_RECORDING,
    REC_STATE_FLUSHING,
} rec_state_t;

typedef struct {
    uint32_t timestamp_ms;
    float    values[WOT_RECORDER_MAX_VARS_PER_SAMPLE];
} rec_sample_t;

typedef struct {
    bool                          initialized;
    bool                          armed;
    rec_state_t                   state;

    wot_recorder_clock_fn_t       clock_now_ms;
    wot_recorder_on_finish_fn_t   on_finish;
    void                         *user_ctx;
    uint8_t                       trigger_var_index;
    uint8_t                       variables_per_sample;
    const char * const           *variable_names;

    rec_sample_t                 *samples;
    uint32_t                      samples_recorded;

    uint32_t                      record_started_ms;
    uint32_t                      below_threshold_since_ms;
    bool                          below_threshold_active;

    // Output buffers — owned by the recorder, valid only during
    // the on_finish callback.
    uint8_t                      *gzip_buf;
    size_t                        gzip_capacity;
} rec_ctx_t;

static rec_ctx_t s_ctx;

// ------------------------------------------------------------------
// CRC32 (gzip uses the standard ISO/IEEE polynomial)
// ------------------------------------------------------------------

#define WOT_CRC32_POLY     ((uint32_t)0xEDB88320)
#define WOT_CRC32_BITS     ((uint32_t)8)

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint32_t b = 0; b < WOT_CRC32_BITS; b++) {
            uint32_t mask = (uint32_t)0 - (crc & (uint32_t)1);
            crc = (crc >> (uint32_t)1) ^ (WOT_CRC32_POLY & mask);
        }
    }
    return ~crc;
}

// ------------------------------------------------------------------
// CSV builder
// ------------------------------------------------------------------

// Conservative per-cell CSV size estimate. Each float worst-case is
// roughly twelve chars ("-1234.5678" + comma); each timestamp is up
// to ten digits + comma; one newline per row. Names live in config.
#define WOT_REC_PER_CELL_MAX   ((size_t)12)
#define WOT_REC_PER_TS_MAX     ((size_t)11)

static size_t estimate_csv_capacity(uint8_t cols, uint32_t rows) {
    size_t header = (size_t)cols * (size_t)WOT_CSV_NAME_FIELD_MAX
                  + (size_t)WOT_CSV_HEADER_PADDING_BYTES;
    size_t body = (size_t)rows *
                  (WOT_REC_PER_TS_MAX + (size_t)cols * WOT_REC_PER_CELL_MAX + (size_t)1);
    return header + body;
}

static size_t append_str(char *dst, size_t cap, size_t pos, const char *s) {
    while (*s && pos < cap) dst[pos++] = *s++;
    return pos;
}

static size_t append_char(char *dst, size_t cap, size_t pos, char c) {
    if (pos < cap) dst[pos++] = c;
    return pos;
}

static size_t build_csv(char *dst, size_t cap) {
    size_t pos = 0;

    // Header row.
    pos = append_str(dst, cap, pos, "timestamp_ms");
    for (uint8_t i = 0; i < s_ctx.variables_per_sample; i++) {
        pos = append_char(dst, cap, pos, ',');
        if (s_ctx.variable_names != NULL && s_ctx.variable_names[i] != NULL) {
            pos = append_str(dst, cap, pos, s_ctx.variable_names[i]);
        } else {
            char gen[WOT_REC_PER_CELL_MAX];
            snprintf(gen, sizeof(gen), "v%u", (unsigned)i);
            pos = append_str(dst, cap, pos, gen);
        }
    }
    pos = append_char(dst, cap, pos, '\n');

    // Body rows.
    for (uint32_t r = 0; r < s_ctx.samples_recorded; r++) {
        char tsbuf[WOT_REC_PER_TS_MAX + (size_t)WOT_CSV_TS_BUF_PAD_BYTES];
        int n = snprintf(tsbuf, sizeof(tsbuf), "%u",
                         (unsigned)s_ctx.samples[r].timestamp_ms);
        if (n > 0) {
            for (int i = 0; i < n && pos < cap; i++) dst[pos++] = tsbuf[i];
        }
        for (uint8_t c = 0; c < s_ctx.variables_per_sample; c++) {
            pos = append_char(dst, cap, pos, ',');
            char cell[WOT_REC_PER_CELL_MAX];
            int m = snprintf(cell, sizeof(cell), "%.3f",
                             (double)s_ctx.samples[r].values[c]);
            if (m > 0) {
                for (int i = 0; i < m && pos < cap; i++) dst[pos++] = cell[i];
            }
        }
        pos = append_char(dst, cap, pos, '\n');
    }

    return pos;
}

// ------------------------------------------------------------------
// Gzip wrapping
// ------------------------------------------------------------------

static const uint8_t k_gzip_header[] = {
    // magic
    (uint8_t)0x1F, (uint8_t)0x8B,
    // method (DEFLATE)
    (uint8_t)0x08,
    // flags
    (uint8_t)0x00,
    // mtime (zeros)
    (uint8_t)0, (uint8_t)0, (uint8_t)0, (uint8_t)0,
    // xfl
    (uint8_t)0,
    // os (unknown)
    (uint8_t)0xFF,
};

#ifdef WOT_RECORDER_HOST_BUILD
// Host: emit "stored" DEFLATE blocks (BTYPE=0). Each block frames up
// to WOT_DEFLATE_STORED_BLOCK_MAX_LEN bytes raw. Layout per RFC:
//   one byte  : BFINAL (lsb 0 or 1) | BTYPE=0 (next two bits) | pad
//   two bytes : LEN  little-endian
//   two bytes : NLEN ones-complement of LEN
//   LEN bytes : raw payload
// That's enough for the on-host structural gzip check; full DEFLATE
// verification happens on-target.
static size_t deflate_stored(const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t out_cap) {
    size_t out_pos = 0;
    size_t in_pos = 0;
    do {
        size_t chunk = in_len - in_pos;
        if (chunk > (size_t)WOT_DEFLATE_STORED_BLOCK_MAX_LEN) {
            chunk = (size_t)WOT_DEFLATE_STORED_BLOCK_MAX_LEN;
        }
        bool is_final = (in_pos + chunk) >= in_len;
        if (out_pos + (size_t)WOT_DEFLATE_STORED_BLOCK_HEADER + chunk > out_cap) {
            return 0;
        }
        out[out_pos++] = is_final ? (uint8_t)0x01 : (uint8_t)0x00;
        out[out_pos++] = (uint8_t)(chunk & (uint32_t)0xFF);
        out[out_pos++] = (uint8_t)((chunk >> WOT_CRC32_BITS) & (uint32_t)0xFF);
        uint32_t nlen = ~((uint32_t)chunk) & (uint32_t)0xFFFF;
        out[out_pos++] = (uint8_t)(nlen & (uint32_t)0xFF);
        out[out_pos++] = (uint8_t)((nlen >> WOT_CRC32_BITS) & (uint32_t)0xFF);
        if (chunk > 0) {
            memcpy(&out[out_pos], &in[in_pos], chunk);
            out_pos += chunk;
            in_pos += chunk;
        }
        if (is_final) break;
    } while (in_pos < in_len);
    return out_pos;
}
#endif

// Build a complete gzip stream around the CSV payload.
// Returns ESP_OK and *out_len on success, ESP_ERR_NO_MEM if the
// output buffer is too small.
static esp_err_t wrap_gzip(const uint8_t *csv, size_t csv_len,
                           uint8_t *out, size_t out_cap, size_t *out_len) {
    if (out_cap < sizeof(k_gzip_header) + (size_t)WOT_GZIP_FOOTER_BYTES) {
        return ESP_ERR_NO_MEM;
    }
    size_t pos = 0;
    memcpy(out, k_gzip_header, sizeof(k_gzip_header));
    pos += sizeof(k_gzip_header);

#ifdef WOT_RECORDER_HOST_BUILD
    size_t deflate_room = out_cap - pos - (size_t)WOT_GZIP_FOOTER_BYTES;
    size_t deflate_len  = deflate_stored(csv, csv_len, &out[pos], deflate_room);
    if (deflate_len == 0 && csv_len != 0) {
        return ESP_ERR_NO_MEM;
    }
    pos += deflate_len;
#else
    // On-target: ESP-IDF v5.5 miniz has MINIZ_NO_ZLIB_APIS set, so the
    // mz_compress2 helper is unavailable. Use the lower-level
    // tdefl_compress_mem_to_mem with flags=0 to emit RAW DEFLATE
    // directly (no zlib framing to strip). Output goes straight into
    // our gzip wrapper.
    size_t avail = out_cap - pos - (size_t)WOT_GZIP_FOOTER_BYTES;
    size_t produced = tdefl_compress_mem_to_mem(&out[pos], avail,
                                                csv, csv_len,
                                                (int)0);
    if (produced == (size_t)0 && csv_len != (size_t)0) {
        ESP_LOGE(TAG, "tdefl_compress_mem_to_mem failed (out cap=%u)", (unsigned)avail);
        return ESP_ERR_NO_MEM;
    }
    pos += produced;
#endif

    // Footer: CRC32 little-endian, then ISIZE little-endian (mod 2^32).
    uint32_t crc = crc32_update((uint32_t)0, csv, csv_len);
    uint32_t isize = (uint32_t)csv_len;
    out[pos++] = (uint8_t)(crc & (uint32_t)0xFF);
    out[pos++] = (uint8_t)((crc >> WOT_CRC32_BITS) & (uint32_t)0xFF);
    out[pos++] = (uint8_t)((crc >> ((uint32_t)2 * WOT_CRC32_BITS)) & (uint32_t)0xFF);
    out[pos++] = (uint8_t)((crc >> ((uint32_t)3 * WOT_CRC32_BITS)) & (uint32_t)0xFF);
    out[pos++] = (uint8_t)(isize & (uint32_t)0xFF);
    out[pos++] = (uint8_t)((isize >> WOT_CRC32_BITS) & (uint32_t)0xFF);
    out[pos++] = (uint8_t)((isize >> ((uint32_t)2 * WOT_CRC32_BITS)) & (uint32_t)0xFF);
    out[pos++] = (uint8_t)((isize >> ((uint32_t)3 * WOT_CRC32_BITS)) & (uint32_t)0xFF);

    *out_len = pos;
    return ESP_OK;
}

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

esp_err_t wot_recorder_init(const wot_recorder_config_t *cfg) {
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg == NULL || cfg->clock_now_ms == NULL || cfg->on_finish == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->variables_per_sample == 0 ||
        cfg->variables_per_sample > WOT_RECORDER_MAX_VARS_PER_SAMPLE) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.clock_now_ms         = cfg->clock_now_ms;
    s_ctx.on_finish            = cfg->on_finish;
    s_ctx.user_ctx             = cfg->user_ctx;
    s_ctx.trigger_var_index    = cfg->trigger_var_index;
    s_ctx.variables_per_sample = cfg->variables_per_sample;
    s_ctx.variable_names       = cfg->variable_names;

    s_ctx.samples = (rec_sample_t *)calloc(WOT_RECORDER_MAX_SAMPLES, sizeof(rec_sample_t));
    if (s_ctx.samples == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Output buffer sized to comfortably hold the CSV expanded
    // estimate plus gzip overhead. Stored-block worst case adds
    // WOT_DEFLATE_STORED_BLOCK_HEADER bytes per max-len chunk; real
    // compression is smaller.
    size_t csv_cap = estimate_csv_capacity(s_ctx.variables_per_sample,
                                           WOT_RECORDER_MAX_SAMPLES);
    size_t worst_blocks = csv_cap / (size_t)WOT_DEFLATE_STORED_BLOCK_MAX_LEN
                        + (size_t)1;
    s_ctx.gzip_capacity = csv_cap
                        + sizeof(k_gzip_header)
                        + (size_t)WOT_GZIP_FOOTER_BYTES
                        + worst_blocks * (size_t)WOT_DEFLATE_STORED_BLOCK_HEADER;
    s_ctx.gzip_buf = (uint8_t *)malloc(s_ctx.gzip_capacity);
    if (s_ctx.gzip_buf == NULL) {
        free(s_ctx.samples);
        s_ctx.samples = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ctx.state       = REC_STATE_IDLE;
    s_ctx.armed       = false;
    s_ctx.initialized = true;

    ESP_LOGI(TAG, "recorder initialized (vars=%u, max_samples=%u)",
             (unsigned)s_ctx.variables_per_sample,
             (unsigned)WOT_RECORDER_MAX_SAMPLES);
    return ESP_OK;
}

void wot_recorder_deinit(void) {
    if (!s_ctx.initialized) {
        return;
    }
    free(s_ctx.samples);
    free(s_ctx.gzip_buf);
    memset(&s_ctx, 0, sizeof(s_ctx));
}

esp_err_t wot_recorder_arm(void) {
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.armed = true;
    s_ctx.state = REC_STATE_IDLE;
    s_ctx.samples_recorded = 0;
    s_ctx.below_threshold_active = false;
    ESP_LOGI(TAG, "recorder armed");
    return ESP_OK;
}

// End the in-progress recording (if any) and emit the gzipped CSV
// via the on_finish callback. Called from disarm and from the
// cooldown / max-duration end paths.
static void flush_recording(void) {
    if (s_ctx.state == REC_STATE_IDLE || s_ctx.samples_recorded == 0) {
        s_ctx.state = REC_STATE_IDLE;
        s_ctx.samples_recorded = 0;
        return;
    }
    s_ctx.state = REC_STATE_FLUSHING;

    size_t csv_cap = estimate_csv_capacity(s_ctx.variables_per_sample,
                                           s_ctx.samples_recorded);
    char *csv = (char *)malloc(csv_cap);
    if (csv == NULL) {
        ESP_LOGE(TAG, "flush: out of memory for CSV");
        s_ctx.state = REC_STATE_IDLE;
        s_ctx.samples_recorded = 0;
        return;
    }
    size_t csv_len = build_csv(csv, csv_cap);

    size_t gz_len = 0;
    esp_err_t rc = wrap_gzip((const uint8_t *)csv, csv_len,
                             s_ctx.gzip_buf, s_ctx.gzip_capacity, &gz_len);
    free(csv);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "flush: gzip wrap failed rc=%d", (int)rc);
        s_ctx.state = REC_STATE_IDLE;
        s_ctx.samples_recorded = 0;
        return;
    }

    ESP_LOGI(TAG, "flush: %u samples, %u csv bytes, %u gzip bytes",
             (unsigned)s_ctx.samples_recorded,
             (unsigned)csv_len, (unsigned)gz_len);
    if (s_ctx.on_finish != NULL) {
        s_ctx.on_finish(s_ctx.gzip_buf, gz_len, s_ctx.user_ctx);
    }

    s_ctx.state = REC_STATE_IDLE;
    s_ctx.samples_recorded = 0;
    s_ctx.below_threshold_active = false;
}

esp_err_t wot_recorder_disarm(void) {
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.armed) {
        s_ctx.armed = false;
        flush_recording();
        ESP_LOGI(TAG, "recorder disarmed");
    }
    return ESP_OK;
}

bool wot_recorder_is_armed(void) {
    return s_ctx.initialized && s_ctx.armed;
}

bool wot_recorder_is_recording(void) {
    return s_ctx.initialized && s_ctx.state == REC_STATE_RECORDING;
}

esp_err_t wot_recorder_feed_sample(const float *values) {
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (values == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.armed) {
        return ESP_OK; // dropped: armed check is the gate
    }

    uint32_t now = s_ctx.clock_now_ms();

    // Resolve trigger variable. If not configured, the recorder
    // silently does nothing — start() succeeded but there's no
    // variable to monitor.
    if (s_ctx.trigger_var_index == WOT_RECORDER_TRIGGER_VAR_NONE ||
        s_ctx.trigger_var_index >= s_ctx.variables_per_sample) {
        return ESP_OK;
    }
    float throttle = values[s_ctx.trigger_var_index];
    bool above = throttle >= (float)WOT_TRIGGER_THRESHOLD_PERCENT;

    switch (s_ctx.state) {
        case REC_STATE_IDLE:
            if (above) {
                s_ctx.state = REC_STATE_RECORDING;
                s_ctx.samples_recorded = 0;
                s_ctx.record_started_ms = now;
                s_ctx.below_threshold_active = false;
                ESP_LOGI(TAG, "trigger crossed; recording started at %u ms", (unsigned)now);
                // Fall through to record this sample.
            } else {
                return ESP_OK;
            }
            // fall through

        case REC_STATE_RECORDING:
            if (s_ctx.samples_recorded < (uint32_t)WOT_RECORDER_MAX_SAMPLES) {
                s_ctx.samples[s_ctx.samples_recorded].timestamp_ms = now;
                memcpy(s_ctx.samples[s_ctx.samples_recorded].values,
                       values, (size_t)s_ctx.variables_per_sample * sizeof(float));
                s_ctx.samples_recorded++;
            }
            // End conditions: hard cap, or cooldown elapsed below threshold.
            if ((now - s_ctx.record_started_ms) >= (uint32_t)WOT_MAX_RECORD_DURATION_MS) {
                ESP_LOGI(TAG, "max-duration cap reached at %u ms; flushing",
                         (unsigned)WOT_MAX_RECORD_DURATION_MS);
                flush_recording();
                return ESP_OK;
            }
            if (above) {
                s_ctx.below_threshold_active = false;
            } else {
                if (!s_ctx.below_threshold_active) {
                    s_ctx.below_threshold_active = true;
                    s_ctx.below_threshold_since_ms = now;
                } else if ((now - s_ctx.below_threshold_since_ms) >=
                           (uint32_t)WOT_TRIGGER_COOLDOWN_MS) {
                    ESP_LOGI(TAG, "cooldown elapsed; flushing");
                    flush_recording();
                    return ESP_OK;
                }
            }
            break;

        case REC_STATE_FLUSHING:
            // Should not happen — flush is synchronous.
            break;
    }

    return ESP_OK;
}
