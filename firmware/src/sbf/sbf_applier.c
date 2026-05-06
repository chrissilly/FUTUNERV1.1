#include "sbf_applier.h"
#include "sbf_config.h"
#include "blend_engine.h"

#include "esp_log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// TODO Phase 2 sentinel check — verify the ECU's currently-running
// binary contains the live-tune patches before applying. Sentinel
// address/value is per-variant and lives in the variant manifest.
// Until the manifest schema lands, dev-car operator discipline is
// the only safeguard against a "successful" apply against a
// stock-flashed ECU. Tracked as P-11 in
// docs/PHASE_2_PREREQUISITES.md (item (b)).
//
// sbf_applier — see sbf_applier.h. Pure orchestration of:
//   1. for each flex map in the loaded SBF:
//      a. read map_info (orig_addr, dims, dtype, endian)
//      b. read blend_axes
//      c. compute blend_factor for ethanol_pct via blend_engine
//      d. fetch gasoline + ethanol byte buffers
//      e. interpolate into a scratch buffer via blend_engine
//      f. ecu_write_data(orig_addr, scratch, byte_count, mid_byte, off, ...)
//      g. wait for per-write callback (ecu_write_iface.write blocks)
//   2. emit progress every SBF_PROGRESS_EVENT_EVERY_N_MAPS maps
//   3. record elapsed_ms

static const char *TAG = "SBF_APPLY";

esp_err_t sbf_applier_apply(sbf_loaded_t              *loaded,
                            const sbf_loader_iface_t  *loader,
                            uint8_t                    ethanol_pct,
                            uint8_t                    mid_byte,
                            uint32_t                   address_offset,
                            const sbf_applier_deps_t  *deps,
                            sbf_apply_result_t        *out) {
    if (loaded == NULL || loader == NULL || deps == NULL || out == NULL) {
        if (out != NULL) {
            snprintf(out->failure_reason, sizeof(out->failure_reason),
                     "invalid argument");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (deps->write == NULL || deps->clock_now_ms == NULL) {
        snprintf(out->failure_reason, sizeof(out->failure_reason),
                 "deps incomplete");
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (ethanol_pct > (uint8_t)SBF_ETHANOL_MAX_PCT) {
        ethanol_pct = (uint8_t)SBF_ETHANOL_MAX_PCT;
    }

    uint32_t total_maps = loader->map_count(loaded, loader->user_ctx);
    if (total_maps == (uint32_t)0) {
        snprintf(out->failure_reason, sizeof(out->failure_reason),
                 "loaded SBF has zero maps");
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t start_ms = deps->clock_now_ms(deps->user_ctx);

    // Per-map scratch buffer reused across iterations. Size grows on
    // demand to fit the largest map encountered.
    uint8_t *scratch     = NULL;
    size_t   scratch_cap = (size_t)0;

    esp_err_t outcome = ESP_OK;

    for (uint32_t i = (uint32_t)0; i < total_maps; i++) {
        sbf_loader_map_info_t info;
        esp_err_t rc = loader->map_info(loaded, i, &info, loader->user_ctx);
        if (rc != ESP_OK) {
            snprintf(out->failure_reason, sizeof(out->failure_reason),
                     "map_info(%u) rc=%d", (unsigned)i, (int)rc);
            outcome = rc; goto done;
        }

        uint16_t blend_x[SBF_BLEND_MAP_POINTS];
        uint16_t blend_z[SBF_BLEND_MAP_POINTS];
        rc = loader->blend_axes(loaded, i, blend_x, blend_z,
                                (size_t)SBF_BLEND_MAP_POINTS,
                                loader->user_ctx);
        if (rc != ESP_OK) {
            snprintf(out->failure_reason, sizeof(out->failure_reason),
                     "blend_axes(%u) rc=%d", (unsigned)i, (int)rc);
            outcome = rc; goto done;
        }
        uint16_t factor = blend_engine_compute_factor(blend_x, blend_z,
                                                      (size_t)SBF_BLEND_MAP_POINTS,
                                                      ethanol_pct);

        const uint8_t *gas_buf = NULL;
        const uint8_t *eth_buf = NULL;
        size_t         byte_count = (size_t)0;
        rc = loader->map_buffers(loaded, i, &gas_buf, &eth_buf,
                                 &byte_count, loader->user_ctx);
        if (rc != ESP_OK || gas_buf == NULL || eth_buf == NULL || byte_count == (size_t)0) {
            snprintf(out->failure_reason, sizeof(out->failure_reason),
                     "map_buffers(%u) rc=%d", (unsigned)i, (int)rc);
            outcome = rc != ESP_OK ? rc : ESP_ERR_INVALID_STATE;
            goto done;
        }

        if (byte_count > scratch_cap) {
            uint8_t *grew = (uint8_t *)realloc(scratch, byte_count);
            if (grew == NULL) {
                snprintf(out->failure_reason, sizeof(out->failure_reason),
                         "OOM growing scratch to %u bytes", (unsigned)byte_count);
                outcome = ESP_ERR_NO_MEM;
                goto done;
            }
            scratch     = grew;
            scratch_cap = byte_count;
        }

        size_t cell_size = blend_dtype_size(info.dtype);
        if (cell_size == (size_t)0) {
            snprintf(out->failure_reason, sizeof(out->failure_reason),
                     "map %u: invalid dtype", (unsigned)i);
            outcome = ESP_ERR_INVALID_STATE;
            goto done;
        }
        size_t cell_count = byte_count / cell_size;
        rc = blend_engine_interpolate_buffer(gas_buf, eth_buf, scratch,
                                             cell_count, info.dtype,
                                             info.big_endian, factor);
        if (rc != ESP_OK) {
            snprintf(out->failure_reason, sizeof(out->failure_reason),
                     "interp(%u) rc=%d", (unsigned)i, (int)rc);
            outcome = rc; goto done;
        }

        rc = deps->write(info.original_address, scratch, byte_count,
                         mid_byte, address_offset,
                         (uint32_t)SBF_PER_WRITE_TIMEOUT_MS,
                         deps->user_ctx);
        if (rc != ESP_OK) {
            snprintf(out->failure_reason, sizeof(out->failure_reason),
                     "ecu_write(%u, addr=0x%08X) rc=%d",
                     (unsigned)i, (unsigned)info.original_address, (int)rc);
            outcome = rc; goto done;
        }

        out->maps_applied++;
        out->total_bytes_written += (uint32_t)byte_count;

        if (deps->progress != NULL &&
            (out->maps_applied % (uint32_t)SBF_PROGRESS_EVENT_EVERY_N_MAPS) == (uint32_t)0) {
            deps->progress(out->maps_applied, total_maps, deps->user_ctx);
        }
    }

done:
    out->elapsed_ms = deps->clock_now_ms(deps->user_ctx) - start_ms;
    if (scratch != NULL) free(scratch);

    if (outcome == ESP_OK) {
        ESP_LOGI(TAG, "apply complete: %u maps, %u bytes, %u ms",
                 (unsigned)out->maps_applied,
                 (unsigned)out->total_bytes_written,
                 (unsigned)out->elapsed_ms);
    } else {
        ESP_LOGE(TAG, "apply failed at map %u: %s",
                 (unsigned)out->maps_applied, out->failure_reason);
    }
    return outcome;
}
