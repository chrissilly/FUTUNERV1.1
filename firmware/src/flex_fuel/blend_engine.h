#ifndef BLEND_ENGINE_H
#define BLEND_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * blend_engine — pure ethanol-blend interpolation math. No state,
 * no I/O, no orchestration. Inputs in, interpolated bytes out.
 *
 * Used by sbf_applier (and eventually any other code that needs to
 * compute "the byte sequence I should write to the ECU for ethanol
 * percent X" given gasoline + ethanol calibration data and a SCAL
 * blend map).
 *
 * Sean's Prompt-5 directive Q5: this module is a pure function.
 * Per-cell computation only. The applier owns the loop, the
 * blend_factor lookup from the blend_map's x/z axes, and the
 * ecu_write_data call. blend_engine just does per-cell math.
 *
 * SCAL blend map convention (per scal_file.h):
 *   - x-axis: 9 uint16 values representing ethanol-percent grid
 *     points (e.g. [0, 12, 25, 37, 50, 62, 75, 87, 100] scaled to
 *     uint16 full-scale).
 *   - z-axis: 9 uint16 interpolation factors aligned with x-axis;
 *     0 = pure gasoline, SBF_BLEND_FACTOR_FULL_SCALE = pure ethanol.
 *
 * Endianness handling: SCAL data may be little- or big-endian per
 * the flex map entry's byte_order field. The applier passes raw
 * bytes plus a byte_order_swap flag. The engine returns interpolated
 * bytes in the same wire order the applier should pass to
 * ecu_write_data.
 */

/* Data type codes mirror SCAL's. Re-declared here so blend_engine
 * has no dependency on the frozen scal_file.h beyond ABI shape. */
typedef enum {
    BLEND_DTYPE_U8  = 0,
    BLEND_DTYPE_U16 = 1,
    BLEND_DTYPE_U32 = 2,
    BLEND_DTYPE_S8  = 3,
    BLEND_DTYPE_S16 = 4,
    BLEND_DTYPE_S32 = 5,
} blend_dtype_t;

/* Returns the byte width of `dtype` (1 / 2 / 4). */
size_t blend_dtype_size(blend_dtype_t dtype);

/*
 * Compute the blend factor (Q0.16, [0..SBF_BLEND_FACTOR_FULL_SCALE])
 * for a given ethanol percentage by linear interpolation across the
 * blend map's x-axis grid.
 *
 * x_axis / z_values: 9-element uint16 arrays from the SCAL blend map.
 * ethanol_pct:       [0..100] inclusive; clamped if out of range.
 *
 * Returns the interpolated z-value as a uint16. Pure function.
 */
uint16_t blend_engine_compute_factor(const uint16_t *x_axis,
                                     const uint16_t *z_values,
                                     size_t          point_count,
                                     uint8_t         ethanol_pct);

/*
 * Per-cell interpolation. Reads one cell from gasoline_bytes and
 * ethanol_bytes (each `dtype`-wide, in `big_endian` byte order),
 * computes:
 *
 *   interp = gasoline + ((ethanol - gasoline) * blend_factor) / FULL_SCALE
 *
 * with clamping appropriate to the data type, and writes the result
 * to out_bytes in the same byte order. Out buffer must have
 * blend_dtype_size(dtype) bytes available.
 *
 * Returns ESP_OK on success, ESP_ERR_INVALID_ARG on NULL pointers
 * or invalid dtype.
 */
esp_err_t blend_engine_interpolate_cell(const uint8_t *gasoline_bytes,
                                        const uint8_t *ethanol_bytes,
                                        uint8_t       *out_bytes,
                                        blend_dtype_t  dtype,
                                        bool           big_endian,
                                        uint16_t       blend_factor);

/*
 * Convenience: interpolate a contiguous buffer of `cell_count` cells
 * back-to-back. Each cell occupies blend_dtype_size(dtype) bytes.
 * Buffers must all be at least cell_count * blend_dtype_size(dtype)
 * bytes long.
 *
 * Returns ESP_OK on success, ESP_ERR_INVALID_ARG on NULL or invalid
 * dtype.
 */
esp_err_t blend_engine_interpolate_buffer(const uint8_t *gasoline_buf,
                                          const uint8_t *ethanol_buf,
                                          uint8_t       *out_buf,
                                          size_t         cell_count,
                                          blend_dtype_t  dtype,
                                          bool           big_endian,
                                          uint16_t       blend_factor);

/* Legacy stub kept so the existing callers in flex_fuel.c still
 * link. Phase 7 wires up the real flex-fuel runtime; this is just
 * a placeholder so init order doesn't break. */
esp_err_t blend_engine_init(void);

#endif /* BLEND_ENGINE_H */
