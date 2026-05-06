#ifndef SBF_LOADER_H
#define SBF_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "blend_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sbf_loader — opens an SBF (binary SCAL/BDEF) file from disk and
 * exposes per-map accessors so the applier can walk it without
 * caring whether the underlying parser is the frozen scal_file
 * module on target or a mock in the host test.
 *
 * On target, sbf_loader.c provides the iface implementation by
 * wrapping scal_file_open / scal_file_read_flex_map_entry /
 * scal_file_read_blend_map / scal_file_read_gasoline_data /
 * scal_file_read_ethanol_data / scal_file_close (the frozen module's
 * public API).
 *
 * In host tests, the test installs its own sbf_loader_iface_t with
 * deterministic synthetic data — the frozen .c files are NEVER
 * linked into the host runner.
 */

/* Opaque handle returned by load(). Stable until free() is called. */
typedef struct sbf_loaded_s sbf_loaded_t;

typedef struct {
    /* Original ECU address — the WRITE TARGET. */
    uint32_t      original_address;

    /* Logical 2-D dimensions of the map cells. byte_count =
     * x_dim * y_dim * blend_dtype_size(dtype). */
    uint32_t      x_dim;
    uint32_t      y_dim;

    /* Per-cell width + endianness. */
    blend_dtype_t dtype;
    bool          big_endian;
} sbf_loader_map_info_t;

typedef struct {
    /* Open + parse the SBF at `path`. On success returns ESP_OK and
     * fills *out with a non-NULL handle owned by the loader. */
    esp_err_t (*load)(const char *path, sbf_loaded_t **out, void *user_ctx);

    /* Release the handle. Idempotent on NULL. */
    void      (*free)(sbf_loaded_t *loaded, void *user_ctx);

    /* Number of flex maps in the loaded SBF. */
    uint32_t  (*map_count)(sbf_loaded_t *loaded, void *user_ctx);

    /* Per-map metadata. */
    esp_err_t (*map_info)(sbf_loaded_t *loaded,
                          uint32_t      idx,
                          sbf_loader_map_info_t *out,
                          void         *user_ctx);

    /* Blend-map x-axis + z-values for map at idx. Caller provides
     * fixed-size arrays of length point_count (typically
     * SBF_BLEND_MAP_POINTS = 9). Returns ESP_ERR_INVALID_SIZE if
     * point_count doesn't match the SBF's blend map. */
    esp_err_t (*blend_axes)(sbf_loaded_t *loaded,
                            uint32_t      idx,
                            uint16_t     *x_axis_out,
                            uint16_t     *z_values_out,
                            size_t        point_count,
                            void         *user_ctx);

    /* Pointers to the gasoline + ethanol calibration data for map
     * at idx, plus the per-map byte_count (cell_count *
     * dtype_size). The pointers are loader-owned and remain valid
     * until free() is called. */
    esp_err_t (*map_buffers)(sbf_loaded_t  *loaded,
                             uint32_t       idx,
                             const uint8_t **gasoline_buf_out,
                             const uint8_t **ethanol_buf_out,
                             size_t        *byte_count_out,
                             void          *user_ctx);

    void *user_ctx;
} sbf_loader_iface_t;

/*
 * Returns the on-target iface (wraps the frozen scal_file_* calls).
 * Host test build does NOT implement this — the test provides its
 * own iface struct directly to sbf_orchestrator's config.
 */
sbf_loader_iface_t sbf_loader_target_iface(void);

#ifdef __cplusplus
}
#endif

#endif /* SBF_LOADER_H */
