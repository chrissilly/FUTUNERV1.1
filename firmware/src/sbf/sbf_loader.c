#include "sbf_loader.h"
#include "sbf_config.h"

#include "esp_log.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef SBF_HOST_BUILD
#  include "scal/scal_file.h"
#endif

// sbf_loader — see sbf_loader.h. The on-target implementation wraps
// the frozen scal_file_* public API. Host builds provide their own
// iface in test_sbf_orchestrator.c; this file's on-target body is
// gated out under SBF_HOST_BUILD so the frozen .c files are never
// linked into the host runner.

static const char *TAG = "SBF_LDR";

#ifndef SBF_HOST_BUILD

// On-target sbf_loaded_t carries one scal handle plus a small
// per-map cache of read-back data so the applier can call
// map_buffers() without re-reading on every cell.

typedef struct {
    uint32_t       byte_count;
    uint8_t       *gasoline_buf;   // owned, malloc'd
    uint8_t       *ethanol_buf;    // owned, malloc'd
    uint16_t       blend_x[SBF_BLEND_MAP_POINTS];
    uint16_t       blend_z[SBF_BLEND_MAP_POINTS];
    bool           cached;
} target_map_cache_t;

struct sbf_loaded_s {
    scal_file_t       *scal;
    uint32_t           map_count;
    target_map_cache_t *cache;
};

static esp_err_t target_load(const char *path, sbf_loaded_t **out, void *user_ctx) {
    (void)user_ctx;
    if (path == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    scal_file_t *scal = scal_file_open(path);
    if (scal == NULL) {
        ESP_LOGE(TAG, "scal_file_open(%s) failed", path);
        return ESP_FAIL;
    }
    sbf_loaded_t *l = (sbf_loaded_t *)calloc((size_t)1, sizeof(*l));
    if (l == NULL) {
        scal_file_close(scal);
        return ESP_ERR_NO_MEM;
    }
    l->scal      = scal;
    l->map_count = scal_file_get_flex_map_count(scal);
    if (l->map_count > (uint32_t)0) {
        l->cache = (target_map_cache_t *)calloc((size_t)l->map_count,
                                                sizeof(target_map_cache_t));
        if (l->cache == NULL) {
            scal_file_close(scal);
            free(l);
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "loaded %s (%u flex maps)", path, (unsigned)l->map_count);
    *out = l;
    return ESP_OK;
}

static void target_free(sbf_loaded_t *loaded, void *user_ctx) {
    (void)user_ctx;
    if (loaded == NULL) return;
    if (loaded->cache != NULL) {
        for (uint32_t i = (uint32_t)0; i < loaded->map_count; i++) {
            if (loaded->cache[i].gasoline_buf) free(loaded->cache[i].gasoline_buf);
            if (loaded->cache[i].ethanol_buf)  free(loaded->cache[i].ethanol_buf);
        }
        free(loaded->cache);
    }
    if (loaded->scal != NULL) scal_file_close(loaded->scal);
    free(loaded);
}

static uint32_t target_map_count(sbf_loaded_t *loaded, void *user_ctx) {
    (void)user_ctx;
    return loaded != NULL ? loaded->map_count : (uint32_t)0;
}

// Translate scal data type / byte order to our blend_engine enums.
static blend_dtype_t translate_dtype(scal_data_type_t scal_dt) {
    switch (scal_dt) {
        case SCAL_DATA_TYPE_UINT8:  return BLEND_DTYPE_U8;
        case SCAL_DATA_TYPE_UINT16: return BLEND_DTYPE_U16;
        case SCAL_DATA_TYPE_UINT32: return BLEND_DTYPE_U32;
        case SCAL_DATA_TYPE_INT8:   return BLEND_DTYPE_S8;
        case SCAL_DATA_TYPE_INT16:  return BLEND_DTYPE_S16;
        case SCAL_DATA_TYPE_INT32:  return BLEND_DTYPE_S32;
        default:                    return BLEND_DTYPE_U8;
    }
}

static esp_err_t target_map_info(sbf_loaded_t *loaded, uint32_t idx,
                                 sbf_loader_map_info_t *out, void *user_ctx) {
    (void)user_ctx;
    if (loaded == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (idx >= loaded->map_count) return ESP_ERR_INVALID_ARG;

    scal_flex_map_entry_t entry;
    esp_err_t rc = scal_file_read_flex_map_entry(loaded->scal, idx, &entry);
    if (rc != ESP_OK) return rc;
    out->original_address = entry.original_address;
    out->x_dim            = entry.x_dimension;
    out->y_dim            = entry.y_dimension;
    out->dtype            = translate_dtype(entry.data_type);
    out->big_endian       = (entry.byte_order == SCAL_BYTE_ORDER_BIG_ENDIAN);
    return ESP_OK;
}

// Lazily populate the cache for map[idx]: blend axes + gasoline/eth
// data. Idempotent.
static esp_err_t target_ensure_cached(sbf_loaded_t *loaded, uint32_t idx) {
    if (idx >= loaded->map_count) return ESP_ERR_INVALID_ARG;
    if (loaded->cache[idx].cached) return ESP_OK;

    scal_flex_map_entry_t entry;
    esp_err_t rc = scal_file_read_flex_map_entry(loaded->scal, idx, &entry);
    if (rc != ESP_OK) return rc;

    size_t cell_size = blend_dtype_size(translate_dtype(entry.data_type));
    if (cell_size == (size_t)0) return ESP_ERR_INVALID_STATE;
    size_t byte_count = (size_t)entry.x_dimension *
                        (size_t)entry.y_dimension * cell_size;
    if (byte_count == (size_t)0) return ESP_ERR_INVALID_STATE;

    uint8_t *gas = (uint8_t *)malloc(byte_count);
    uint8_t *eth = (uint8_t *)malloc(byte_count);
    if (gas == NULL || eth == NULL) {
        free(gas); free(eth);
        return ESP_ERR_NO_MEM;
    }
    rc = scal_file_read_gasoline_data(loaded->scal, entry.gasoline_address,
                                      gas, byte_count);
    if (rc != ESP_OK) {
        free(gas); free(eth);
        return rc;
    }
    rc = scal_file_read_ethanol_data(loaded->scal, entry.ethanol_address,
                                     eth, byte_count);
    if (rc != ESP_OK) {
        free(gas); free(eth);
        return rc;
    }

    scal_blend_map_t bm = {0};
    rc = scal_file_read_blend_map(loaded->scal, entry.blend_map_address, &bm);
    if (rc != ESP_OK) {
        free(gas); free(eth);
        return rc;
    }
    if (bm.x_axis_data != NULL && bm.z_values_data != NULL) {
        for (size_t i = (size_t)0; i < (size_t)SBF_BLEND_MAP_POINTS; i++) {
            loaded->cache[idx].blend_x[i] = bm.x_axis_data[i];
            loaded->cache[idx].blend_z[i] = bm.z_values_data[i];
        }
    }
    scal_blend_map_free(&bm);

    loaded->cache[idx].byte_count   = (uint32_t)byte_count;
    loaded->cache[idx].gasoline_buf = gas;
    loaded->cache[idx].ethanol_buf  = eth;
    loaded->cache[idx].cached       = true;
    return ESP_OK;
}

static esp_err_t target_blend_axes(sbf_loaded_t *loaded, uint32_t idx,
                                   uint16_t *x_axis_out, uint16_t *z_values_out,
                                   size_t point_count, void *user_ctx) {
    (void)user_ctx;
    if (loaded == NULL || x_axis_out == NULL || z_values_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (point_count != (size_t)SBF_BLEND_MAP_POINTS) return ESP_ERR_INVALID_SIZE;
    esp_err_t rc = target_ensure_cached(loaded, idx);
    if (rc != ESP_OK) return rc;
    for (size_t i = (size_t)0; i < point_count; i++) {
        x_axis_out[i]   = loaded->cache[idx].blend_x[i];
        z_values_out[i] = loaded->cache[idx].blend_z[i];
    }
    return ESP_OK;
}

static esp_err_t target_map_buffers(sbf_loaded_t *loaded, uint32_t idx,
                                    const uint8_t **gas_out, const uint8_t **eth_out,
                                    size_t *byte_count_out, void *user_ctx) {
    (void)user_ctx;
    if (loaded == NULL || gas_out == NULL || eth_out == NULL || byte_count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t rc = target_ensure_cached(loaded, idx);
    if (rc != ESP_OK) return rc;
    *gas_out        = loaded->cache[idx].gasoline_buf;
    *eth_out        = loaded->cache[idx].ethanol_buf;
    *byte_count_out = (size_t)loaded->cache[idx].byte_count;
    return ESP_OK;
}

sbf_loader_iface_t sbf_loader_target_iface(void) {
    sbf_loader_iface_t iface = {
        .load        = target_load,
        .free        = target_free,
        .map_count   = target_map_count,
        .map_info    = target_map_info,
        .blend_axes  = target_blend_axes,
        .map_buffers = target_map_buffers,
        .user_ctx    = NULL,
    };
    return iface;
}

#else /* SBF_HOST_BUILD */

/* The host build does NOT link the frozen scal_file_*. Provide a
 * stub target_iface returning empty function pointers so any
 * accidental call from non-test code crashes loud rather than
 * silently misbehaving. The test fills in its own iface; nothing
 * else should call this on host. */
sbf_loader_iface_t sbf_loader_target_iface(void) {
    sbf_loader_iface_t iface = {0};
    return iface;
}

#endif /* SBF_HOST_BUILD */
