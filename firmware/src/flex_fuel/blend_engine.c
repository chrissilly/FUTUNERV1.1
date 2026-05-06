#include "blend_engine.h"
#include "sbf_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"

// blend_engine — see blend_engine.h. Pure math, no state.
//
// The interpolation model is the standard linear mix:
//   interp = gasoline + ((ethanol - gasoline) * f) / FULL_SCALE
// where `f` is a Q0.16 blend factor produced by walking the SCAL
// blend map's 9-point x-axis (ethanol percent grid) and finding the
// matching z-axis interpolation factor.
//
// Endianness: SCAL data may be little- or big-endian per each flex
// map entry's byte_order field. Reads and writes are byte-by-byte
// with explicit shift assembly so the engine works on any host.

static const char *TAG = "BLEND";

// ------------------------------------------------------------------
// Helpers — read/write integer cells in either byte order
// ------------------------------------------------------------------

#define BLEND_BITS_PER_BYTE          8
#define BLEND_BITS_PER_TWO_BYTES     16
#define BLEND_BITS_PER_THREE_BYTES   24

size_t blend_dtype_size(blend_dtype_t dtype) {
    switch (dtype) {
        case BLEND_DTYPE_U8:
        case BLEND_DTYPE_S8:
            return sizeof(uint8_t);
        case BLEND_DTYPE_U16:
        case BLEND_DTYPE_S16:
            return sizeof(uint16_t);
        case BLEND_DTYPE_U32:
        case BLEND_DTYPE_S32:
            return sizeof(uint32_t);
        default:
            return (size_t)0;
    }
}

static int64_t read_signed_cell(const uint8_t *src, blend_dtype_t dtype, bool big_endian) {
    switch (dtype) {
        case BLEND_DTYPE_U8:
            return (int64_t)src[0];
        case BLEND_DTYPE_S8:
            return (int64_t)((int8_t)src[0]);
        case BLEND_DTYPE_U16: {
            uint16_t v = big_endian
                ? (uint16_t)(((uint16_t)src[0] << BLEND_BITS_PER_BYTE) | (uint16_t)src[1])
                : (uint16_t)(((uint16_t)src[1] << BLEND_BITS_PER_BYTE) | (uint16_t)src[0]);
            return (int64_t)v;
        }
        case BLEND_DTYPE_S16: {
            uint16_t v = big_endian
                ? (uint16_t)(((uint16_t)src[0] << BLEND_BITS_PER_BYTE) | (uint16_t)src[1])
                : (uint16_t)(((uint16_t)src[1] << BLEND_BITS_PER_BYTE) | (uint16_t)src[0]);
            return (int64_t)((int16_t)v);
        }
        case BLEND_DTYPE_U32: {
            uint32_t v;
            if (big_endian) {
                v = ((uint32_t)src[0] << BLEND_BITS_PER_THREE_BYTES) |
                    ((uint32_t)src[1] << BLEND_BITS_PER_TWO_BYTES)   |
                    ((uint32_t)src[2] << BLEND_BITS_PER_BYTE)        |
                    ((uint32_t)src[3]);
            } else {
                v = ((uint32_t)src[3] << BLEND_BITS_PER_THREE_BYTES) |
                    ((uint32_t)src[2] << BLEND_BITS_PER_TWO_BYTES)   |
                    ((uint32_t)src[1] << BLEND_BITS_PER_BYTE)        |
                    ((uint32_t)src[0]);
            }
            return (int64_t)v;
        }
        case BLEND_DTYPE_S32: {
            uint32_t v;
            if (big_endian) {
                v = ((uint32_t)src[0] << BLEND_BITS_PER_THREE_BYTES) |
                    ((uint32_t)src[1] << BLEND_BITS_PER_TWO_BYTES)   |
                    ((uint32_t)src[2] << BLEND_BITS_PER_BYTE)        |
                    ((uint32_t)src[3]);
            } else {
                v = ((uint32_t)src[3] << BLEND_BITS_PER_THREE_BYTES) |
                    ((uint32_t)src[2] << BLEND_BITS_PER_TWO_BYTES)   |
                    ((uint32_t)src[1] << BLEND_BITS_PER_BYTE)        |
                    ((uint32_t)src[0]);
            }
            return (int64_t)((int32_t)v);
        }
        default:
            return (int64_t)0;
    }
}

static void write_signed_cell(int64_t v, uint8_t *dst, blend_dtype_t dtype, bool big_endian) {
    switch (dtype) {
        case BLEND_DTYPE_U8:
            if (v < (int64_t)0) v = (int64_t)0;
            if (v > (int64_t)0xFF) v = (int64_t)0xFF;
            dst[0] = (uint8_t)v;
            break;
        case BLEND_DTYPE_S8:
            if (v < (int64_t)INT8_MIN) v = (int64_t)INT8_MIN;
            if (v > (int64_t)INT8_MAX) v = (int64_t)INT8_MAX;
            dst[0] = (uint8_t)((int8_t)v);
            break;
        case BLEND_DTYPE_U16: {
            if (v < (int64_t)0) v = (int64_t)0;
            if (v > (int64_t)0xFFFF) v = (int64_t)0xFFFF;
            uint16_t u = (uint16_t)v;
            if (big_endian) {
                dst[0] = (uint8_t)(u >> BLEND_BITS_PER_BYTE);
                dst[1] = (uint8_t)(u & (uint16_t)0xFF);
            } else {
                dst[0] = (uint8_t)(u & (uint16_t)0xFF);
                dst[1] = (uint8_t)(u >> BLEND_BITS_PER_BYTE);
            }
            break;
        }
        case BLEND_DTYPE_S16: {
            if (v < (int64_t)INT16_MIN) v = (int64_t)INT16_MIN;
            if (v > (int64_t)INT16_MAX) v = (int64_t)INT16_MAX;
            uint16_t u = (uint16_t)((int16_t)v);
            if (big_endian) {
                dst[0] = (uint8_t)(u >> BLEND_BITS_PER_BYTE);
                dst[1] = (uint8_t)(u & (uint16_t)0xFF);
            } else {
                dst[0] = (uint8_t)(u & (uint16_t)0xFF);
                dst[1] = (uint8_t)(u >> BLEND_BITS_PER_BYTE);
            }
            break;
        }
        case BLEND_DTYPE_U32: {
            if (v < (int64_t)0) v = (int64_t)0;
            if (v > (int64_t)0xFFFFFFFFLL) v = (int64_t)0xFFFFFFFFLL;
            uint32_t u = (uint32_t)v;
            if (big_endian) {
                dst[0] = (uint8_t)(u >> BLEND_BITS_PER_THREE_BYTES);
                dst[1] = (uint8_t)(u >> BLEND_BITS_PER_TWO_BYTES);
                dst[2] = (uint8_t)(u >> BLEND_BITS_PER_BYTE);
                dst[3] = (uint8_t)(u & (uint32_t)0xFF);
            } else {
                dst[0] = (uint8_t)(u & (uint32_t)0xFF);
                dst[1] = (uint8_t)(u >> BLEND_BITS_PER_BYTE);
                dst[2] = (uint8_t)(u >> BLEND_BITS_PER_TWO_BYTES);
                dst[3] = (uint8_t)(u >> BLEND_BITS_PER_THREE_BYTES);
            }
            break;
        }
        case BLEND_DTYPE_S32: {
            if (v < (int64_t)INT32_MIN) v = (int64_t)INT32_MIN;
            if (v > (int64_t)INT32_MAX) v = (int64_t)INT32_MAX;
            uint32_t u = (uint32_t)((int32_t)v);
            if (big_endian) {
                dst[0] = (uint8_t)(u >> BLEND_BITS_PER_THREE_BYTES);
                dst[1] = (uint8_t)(u >> BLEND_BITS_PER_TWO_BYTES);
                dst[2] = (uint8_t)(u >> BLEND_BITS_PER_BYTE);
                dst[3] = (uint8_t)(u & (uint32_t)0xFF);
            } else {
                dst[0] = (uint8_t)(u & (uint32_t)0xFF);
                dst[1] = (uint8_t)(u >> BLEND_BITS_PER_BYTE);
                dst[2] = (uint8_t)(u >> BLEND_BITS_PER_TWO_BYTES);
                dst[3] = (uint8_t)(u >> BLEND_BITS_PER_THREE_BYTES);
            }
            break;
        }
        default:
            break;
    }
}

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

uint16_t blend_engine_compute_factor(const uint16_t *x_axis,
                                     const uint16_t *z_values,
                                     size_t          point_count,
                                     uint8_t         ethanol_pct) {
    if (x_axis == NULL || z_values == NULL || point_count == (size_t)0) {
        return (uint16_t)0;
    }
    // SCAL convention: x-axis values are uint16 percentages scaled to
    // full-scale. The input ethanol_pct (0..100) is mapped to that
    // scale and looked up by interpolation between adjacent x_axis
    // points.
    if (ethanol_pct > (uint8_t)SBF_ETHANOL_MAX_PCT) {
        ethanol_pct = (uint8_t)SBF_ETHANOL_MAX_PCT;
    }
    uint32_t scaled = ((uint32_t)ethanol_pct *
                       (uint32_t)SBF_BLEND_FACTOR_FULL_SCALE) /
                      ((uint32_t)SBF_ETHANOL_MAX_PCT);

    if (scaled <= (uint32_t)x_axis[0]) {
        return z_values[0];
    }
    if (scaled >= (uint32_t)x_axis[point_count - (size_t)1]) {
        return z_values[point_count - (size_t)1];
    }
    for (size_t i = (size_t)1; i < point_count; i++) {
        if (scaled <= (uint32_t)x_axis[i]) {
            uint32_t x0 = (uint32_t)x_axis[i - (size_t)1];
            uint32_t x1 = (uint32_t)x_axis[i];
            uint32_t z0 = (uint32_t)z_values[i - (size_t)1];
            uint32_t z1 = (uint32_t)z_values[i];
            uint32_t span = (x1 > x0) ? (x1 - x0) : (uint32_t)1;
            uint32_t pos  = scaled - x0;
            int64_t  delta = (int64_t)z1 - (int64_t)z0;
            int64_t  step  = (delta * (int64_t)pos) / (int64_t)span;
            int64_t  out   = (int64_t)z0 + step;
            if (out < (int64_t)0) out = (int64_t)0;
            if (out > (int64_t)SBF_BLEND_FACTOR_FULL_SCALE) {
                out = (int64_t)SBF_BLEND_FACTOR_FULL_SCALE;
            }
            return (uint16_t)out;
        }
    }
    return z_values[point_count - (size_t)1];
}

esp_err_t blend_engine_interpolate_cell(const uint8_t *gasoline_bytes,
                                        const uint8_t *ethanol_bytes,
                                        uint8_t       *out_bytes,
                                        blend_dtype_t  dtype,
                                        bool           big_endian,
                                        uint16_t       blend_factor) {
    if (gasoline_bytes == NULL || ethanol_bytes == NULL || out_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (blend_dtype_size(dtype) == (size_t)0) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t gas = read_signed_cell(gasoline_bytes, dtype, big_endian);
    int64_t eth = read_signed_cell(ethanol_bytes,  dtype, big_endian);

    int64_t delta = eth - gas;
    int64_t step  = (delta * (int64_t)blend_factor) /
                    (int64_t)SBF_BLEND_FACTOR_FULL_SCALE;
    int64_t out   = gas + step;
    write_signed_cell(out, out_bytes, dtype, big_endian);
    return ESP_OK;
}

esp_err_t blend_engine_interpolate_buffer(const uint8_t *gasoline_buf,
                                          const uint8_t *ethanol_buf,
                                          uint8_t       *out_buf,
                                          size_t         cell_count,
                                          blend_dtype_t  dtype,
                                          bool           big_endian,
                                          uint16_t       blend_factor) {
    if (gasoline_buf == NULL || ethanol_buf == NULL || out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t cell_size = blend_dtype_size(dtype);
    if (cell_size == (size_t)0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = (size_t)0; i < cell_count; i++) {
        size_t off = i * cell_size;
        esp_err_t rc = blend_engine_interpolate_cell(
            &gasoline_buf[off], &ethanol_buf[off], &out_buf[off],
            dtype, big_endian, blend_factor);
        if (rc != ESP_OK) return rc;
    }
    return ESP_OK;
}

esp_err_t blend_engine_init(void) {
    ESP_LOGI(TAG, "blend_engine ready (pure-function module)");
    return ESP_OK;
}
