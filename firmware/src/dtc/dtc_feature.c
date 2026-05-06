#include "dtc.h"
#include "dtc_uds.h"
#include "dtc_config.h"

#include "feature_manager.h"
#include "esp_log.h"

#ifndef DTC_FEATURE_HOST_BUILD
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"
#  include "isotp_coordinator/isotp_coordinator.h"
#  include "can/can_manager.h"
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * dtc_feature.c — lifecycle / feature_manager glue for DTC. Owns the
 * feature descriptor, the description database, the dtc_read /
 * dtc_clear high-level entry points, and (on target) the ISO-TP +
 * CAN transport adapter that dtc_uds calls into.
 *
 * Architecture summary:
 *   - dtc_feature_init() wires the on-target transport adapter into
 *     dtc_uds and hands the FEATURE_DTC descriptor to feature_manager.
 *     On host builds (DTC_FEATURE_HOST_BUILD), it only registers the
 *     descriptor; the test harness calls dtc_uds_init() with a mock.
 *   - dtc_feature_start / dtc_feature_stop are the descriptor
 *     callbacks. They flip s_running but do no UDS work themselves —
 *     this feature has no idle background activity, only on-demand
 *     short-lived UDS exchanges.
 *   - dtc_read() / dtc_clear() are the operation entry points called
 *     from dtc_commands.c. They wrap each UDS exchange in
 *     feature_manager_request_start(FEATURE_DTC) → exchange →
 *     feature_manager_request_stop(FEATURE_DTC). While DTC is the
 *     active feature, no other feature can occupy the bus
 *     concurrently — the arbitration discipline is the
 *     mutual-exclusion mechanism, not a literal mutex held across
 *     the exchange.
 */

static const char *TAG = "DTC";

#define DTC_FALLBACK_DESCRIPTION   "manufacturer-specific code (see scan tool)"

/* ------------------------------------------------------------------ */
/* Description database                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *code;
    const char *description;
} dtc_desc_entry_t;

/* Master SAE J2012 description table. Today every supported family
 * consults this same table because the SAE descriptions are
 * family-agnostic. The family parameter on the lookup function is a
 * forward-compat hook — Phase A will introduce family-specific
 * extensions per docs/SCALE_ARCHITECTURE_PROPOSAL.md §2.2 and
 * docs/boxcode_database.md, at which point per-family override tables
 * slot in beneath this master.
 *
 * Coverage: the most common Bosch MG1/MDG1/MED17 powertrain P-codes
 * seen on Sean's RS7 dev car and across the supported boxcode matrix.
 * Full coverage (~5000 SAE codes) is explicitly out of scope per the
 * kickoff prompt — unknown codes fall through to
 * DTC_FALLBACK_DESCRIPTION. */
static const dtc_desc_entry_t k_master_descriptions[] = {
    /* VVT / cam-position */
    { "P0011", "VVT \"A\" Camshaft Position Timing Over-Advanced (Bank 1)" },
    { "P0014", "VVT \"B\" Camshaft Position Timing Over-Advanced (Bank 1)" },
    { "P0016", "Crankshaft/Camshaft Position Correlation (Bank 1, Sensor A)" },
    { "P0017", "Crankshaft/Camshaft Position Correlation (Bank 1, Sensor B)" },

    /* O2 sensor heaters */
    { "P0030", "HO2S Heater Control Circuit (Bank 1, Sensor 1)" },
    { "P0036", "HO2S Heater Control Circuit (Bank 1, Sensor 2)" },
    { "P0050", "HO2S Heater Control Circuit (Bank 2, Sensor 1)" },
    { "P0056", "HO2S Heater Control Circuit (Bank 2, Sensor 2)" },

    /* Fuel pressure / metering */
    { "P0087", "Fuel Rail/System Pressure Too Low" },
    { "P0089", "Fuel Pressure Regulator 1 Performance" },

    /* MAF / MAP */
    { "P0101", "Mass or Volume Air Flow Circuit Range/Performance" },
    { "P0102", "Mass or Volume Air Flow Circuit Low Input" },
    { "P0103", "Mass or Volume Air Flow Circuit High Input" },
    { "P0107", "Manifold Absolute Pressure Sensor Circuit Low Input" },
    { "P0108", "Manifold Absolute Pressure Sensor Circuit High Input" },

    /* Fuel trim */
    { "P0171", "System Too Lean (Bank 1)" },
    { "P0172", "System Too Rich (Bank 1)" },
    { "P0174", "System Too Lean (Bank 2)" },
    { "P0175", "System Too Rich (Bank 2)" },

    /* Boost */
    { "P0234", "Turbocharger/Supercharger Overboost Condition" },
    { "P0299", "Turbocharger/Supercharger Underboost Condition" },

    /* Misfire */
    { "P0300", "Random/Multiple Cylinder Misfire Detected" },
    { "P0301", "Cylinder 1 Misfire Detected" },
    { "P0302", "Cylinder 2 Misfire Detected" },
    { "P0303", "Cylinder 3 Misfire Detected" },
    { "P0304", "Cylinder 4 Misfire Detected" },

    /* Catalyst */
    { "P0420", "Catalyst System Efficiency Below Threshold (Bank 1)" },
    { "P0430", "Catalyst System Efficiency Below Threshold (Bank 2)" },

    /* EVAP */
    { "P0442", "Evaporative Emission System Leak Detected (Small Leak)" },
    { "P0455", "Evaporative Emission System Leak Detected (Large Leak)" },
    { "P0456", "Evaporative Emission System Leak Detected (Very Small Leak)" },

    /* Idle / ECM */
    { "P0507", "Idle Air Control System RPM Higher Than Expected" },
    { "P0606", "ECM/PCM Processor Fault" },

    /* Transmission */
    { "P0700", "Transmission Control System Malfunction" },

    /* Post-catalyst trim and O2 stuck */
    { "P2096", "Post Catalyst Fuel Trim System Too Lean (Bank 1)" },
    { "P2097", "Post Catalyst Fuel Trim System Too Rich (Bank 1)" },
    { "P2270", "O2 Sensor Signal Stuck Lean (Bank 1, Sensor 2)" },
    { "P2271", "O2 Sensor Signal Stuck Rich (Bank 1, Sensor 2)" },
};

/* Per-family description tables. v1 entries all alias the master
 * table; Phase A will replace these with family-specific arrays. */
static const dtc_desc_entry_t * const k_descriptions_by_family[DTC_ECU_FAMILY_COUNT] = {
    k_master_descriptions, /* MG1 */
    k_master_descriptions, /* MDG1 */
    k_master_descriptions, /* MED17 */
};

static const size_t k_descriptions_count_by_family[DTC_ECU_FAMILY_COUNT] = {
    sizeof(k_master_descriptions) / sizeof(k_master_descriptions[0]),
    sizeof(k_master_descriptions) / sizeof(k_master_descriptions[0]),
    sizeof(k_master_descriptions) / sizeof(k_master_descriptions[0]),
};

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static bool             s_initialized   = false;
static bool             s_running       = false;
static dtc_ecu_family_t s_active_family = DTC_ECU_FAMILY_MG1;

/* ------------------------------------------------------------------ */
/* Description lookup                                                  */
/* ------------------------------------------------------------------ */

static const char *resolve_description(dtc_ecu_family_t family, const char *code) {
    if (family >= DTC_ECU_FAMILY_COUNT || code == NULL) {
        return DTC_FALLBACK_DESCRIPTION;
    }
    const dtc_desc_entry_t *table = k_descriptions_by_family[family];
    size_t                   count = k_descriptions_count_by_family[family];
    for (size_t i = (size_t)0; i < count; i++) {
        if (strcmp(table[i].code, code) == (int)0) {
            return table[i].description;
        }
    }
    return DTC_FALLBACK_DESCRIPTION;
}

/* ------------------------------------------------------------------ */
/* On-target ISO-TP + CAN transport adapter                             */
/* ------------------------------------------------------------------ */

#ifndef DTC_FEATURE_HOST_BUILD

static int target_uds_request(const uint8_t *req,
                              size_t         req_len,
                              uint8_t       *resp,
                              size_t         resp_cap,
                              uint32_t       timeout_ms,
                              void          *user_ctx) {
    (void)user_ctx;

    /* Acquire the ISO-TP coordinator before transmitting so the logger
     * does not poll on top of our request and the connection manager's
     * keepalive does not race the response. */
    if (!isotp_coordinator_request(ISOTP_OWNER_CONNECTION_MANAGER,
                                   (uint32_t)DTC_TARGET_ISOTP_REQUEST_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "isotp coordinator busy; skipping UDS exchange");
        return DTC_TARGET_UDS_RESULT_BUS_BUSY;
    }

    esp_err_t send_rc = can_manager_send_isotp(req, (uint16_t)req_len);
    if (send_rc != ESP_OK) {
        ESP_LOGE(TAG, "can_manager_send_isotp rc=%d", (int)send_rc);
        isotp_coordinator_release(ISOTP_OWNER_CONNECTION_MANAGER);
        return DTC_TARGET_UDS_RESULT_SEND_FAIL;
    }

    uint32_t start_ms =
        (uint32_t)((uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS);
    while (true) {
        uint16_t out_size = (uint16_t)0;
        esp_err_t recv_rc = can_manager_receive_isotp(resp,
                                                      (uint16_t)resp_cap,
                                                      &out_size);
        if (recv_rc == ESP_OK && out_size > (uint16_t)0) {
            isotp_coordinator_release(ISOTP_OWNER_CONNECTION_MANAGER);
            return (int)out_size;
        }
        uint32_t now_ms =
            (uint32_t)((uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS);
        if ((now_ms - start_ms) >= timeout_ms) {
            ESP_LOGW(TAG, "UDS response timeout after %u ms",
                     (unsigned)timeout_ms);
            isotp_coordinator_release(ISOTP_OWNER_CONNECTION_MANAGER);
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(DTC_TARGET_POLL_INTERVAL_MS));
    }
}

#endif /* !DTC_FEATURE_HOST_BUILD */

/* ------------------------------------------------------------------ */
/* feature_manager descriptor callbacks                                 */
/* ------------------------------------------------------------------ */

static esp_err_t dtc_feature_start_cb(void) {
    s_running = true;
    ESP_LOGI(TAG, "DTC feature started (active family=%d)", (int)s_active_family);
    return ESP_OK;
}

static esp_err_t dtc_feature_stop_cb(void) {
    s_running = false;
    ESP_LOGI(TAG, "DTC feature stopped");
    return ESP_OK;
}

static const feature_descriptor_t k_descriptor = {
    .id         = FEATURE_DTC,
    .name       = "dtc",
    .start      = dtc_feature_start_cb,
    .stop       = dtc_feature_stop_cb,
    .is_running = dtc_feature_is_running,
};

/* ------------------------------------------------------------------ */
/* Public lifecycle                                                    */
/* ------------------------------------------------------------------ */

esp_err_t dtc_register_with_feature_manager(void) {
    esp_err_t rc = feature_manager_register(&k_descriptor);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        /* INVALID_STATE = already registered → idempotent; treat as OK. */
        return rc;
    }
    return ESP_OK;
}

esp_err_t dtc_feature_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    s_active_family = DTC_ECU_FAMILY_MG1;

#ifndef DTC_FEATURE_HOST_BUILD
    esp_err_t uds_rc = dtc_uds_init(target_uds_request, NULL);
    if (uds_rc != ESP_OK) {
        ESP_LOGE(TAG, "dtc_uds_init rc=%d", (int)uds_rc);
        return uds_rc;
    }
#endif

    esp_err_t reg_rc = dtc_register_with_feature_manager();
    if (reg_rc != ESP_OK) {
        ESP_LOGE(TAG, "feature_manager register rc=%d", (int)reg_rc);
        return reg_rc;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "dtc_feature initialized (default family label=%s)",
             DTC_FAMILY_DEFAULT);
    return ESP_OK;
}

bool dtc_feature_is_running(void) {
    return s_running;
}

dtc_ecu_family_t dtc_feature_set_family(dtc_ecu_family_t family) {
    dtc_ecu_family_t prev = s_active_family;
    if (family >= DTC_ECU_FAMILY_COUNT) {
        family = DTC_ECU_FAMILY_MG1;
    }
    s_active_family = family;
    ESP_LOGI(TAG, "active family set to %d (was %d)", (int)family, (int)prev);
    return prev;
}

/* ------------------------------------------------------------------ */
/* High-level operations                                               */
/* ------------------------------------------------------------------ */

static void copy_err(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == (size_t)0 || src == NULL) {
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - (size_t)1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void format_nrc_err(char *dst, size_t cap, const char *prefix, uint8_t nrc) {
    if (dst == NULL || cap == (size_t)0) return;
    snprintf(dst, cap, "%s NRC 0x%02X", prefix, (unsigned)nrc);
}

esp_err_t dtc_read(uint8_t      status_mask,
                   dtc_entry_t *out_entries,
                   size_t       entries_cap,
                   size_t      *out_count,
                   char        *err_out,
                   size_t       err_cap) {
    if (out_entries == NULL || out_count == NULL || entries_cap == (size_t)0) {
        copy_err(err_out, err_cap, "invalid argument");
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = (size_t)0;

    if (status_mask == (uint8_t)0) {
        status_mask = (uint8_t)DTC_DEFAULT_STATUS_MASK;
    }

    char fm_err[96] = {0};
    esp_err_t fm_rc = feature_manager_request_start(FEATURE_DTC, fm_err, sizeof(fm_err));
    if (fm_rc != ESP_OK) {
        ESP_LOGW(TAG, "feature_manager request_start rc=%d (%s)", (int)fm_rc, fm_err);
        copy_err(err_out, err_cap, fm_err[0] != '\0' ? fm_err : "feature_manager rejected DTC start");
        return fm_rc;
    }

    esp_err_t op_rc = dtc_uds_read_dtcs_by_status_mask((uint8_t)status_mask,
                                                       (uint32_t)DTC_READ_TIMEOUT_MS,
                                                       out_entries, entries_cap,
                                                       out_count);

    /* Resolve descriptions while we still hold the active slot. */
    if (op_rc == ESP_OK) {
        for (size_t i = (size_t)0; i < *out_count; i++) {
            out_entries[i].description = resolve_description(s_active_family,
                                                             out_entries[i].code);
        }
    } else if (op_rc == ESP_ERR_INVALID_RESPONSE) {
        format_nrc_err(err_out, err_cap, "DTC read", dtc_uds_last_nrc());
    } else if (op_rc == ESP_ERR_TIMEOUT) {
        copy_err(err_out, err_cap, "DTC read transport timeout");
    } else {
        copy_err(err_out, err_cap, "DTC read transport error");
    }

    /* Always release the slot, regardless of operation outcome. */
    esp_err_t stop_rc = feature_manager_request_stop(FEATURE_DTC);
    if (stop_rc != ESP_OK) {
        ESP_LOGE(TAG, "feature_manager request_stop rc=%d after read", (int)stop_rc);
    }
    return op_rc;
}

esp_err_t dtc_clear(uint16_t *cleared_count_out,
                    char     *err_out,
                    size_t    err_cap) {
    if (cleared_count_out != NULL) {
        *cleared_count_out = (uint16_t)0;
    }

    char fm_err[96] = {0};
    esp_err_t fm_rc = feature_manager_request_start(FEATURE_DTC, fm_err, sizeof(fm_err));
    if (fm_rc != ESP_OK) {
        ESP_LOGW(TAG, "feature_manager request_start rc=%d (%s)", (int)fm_rc, fm_err);
        copy_err(err_out, err_cap, fm_err[0] != '\0' ? fm_err : "feature_manager rejected DTC start");
        return fm_rc;
    }

    /* UDS 0x14 itself returns no count, so a pre-clear read using
     * DTC_DEFAULT_STATUS_MASK gives the WS response a meaningful
     * cleared_count. The pre-read is within the prompt's allowed UDS
     * surface (0x19 0x02 only), and the cost is one extra exchange
     * (~50–200 ms typical). */
    dtc_entry_t scratch[DTC_MAX_CODES_PER_RESPONSE];
    size_t      pre_count = (size_t)0;
    esp_err_t   pre_rc = dtc_uds_read_dtcs_by_status_mask((uint8_t)DTC_DEFAULT_STATUS_MASK,
                                                          (uint32_t)DTC_READ_TIMEOUT_MS,
                                                          scratch,
                                                          sizeof(scratch) / sizeof(scratch[0]),
                                                          &pre_count);
    if (pre_rc != ESP_OK && pre_rc != ESP_ERR_INVALID_RESPONSE) {
        /* Hard transport failure. Surface and bail. */
        if (pre_rc == ESP_ERR_TIMEOUT) {
            copy_err(err_out, err_cap, "DTC pre-clear read timed out");
        } else {
            copy_err(err_out, err_cap, "DTC pre-clear read transport error");
        }
        feature_manager_request_stop(FEATURE_DTC);
        return pre_rc;
    }
    /* INVALID_RESPONSE on the pre-read (e.g. ECU is in a session that
     * does not support 0x19 0x02): ignore and proceed to clear with
     * cleared_count = 0. */

    esp_err_t clr_rc = dtc_uds_clear_diagnostic_information((uint32_t)DTC_CLEAR_TIMEOUT_MS);
    if (clr_rc == ESP_OK) {
        if (cleared_count_out != NULL) {
            uint16_t reported = (pre_count > (size_t)UINT16_MAX) ? (uint16_t)UINT16_MAX
                                                                  : (uint16_t)pre_count;
            *cleared_count_out = reported;
        }
        ESP_LOGI(TAG, "DTC clear complete; cleared_count=%u",
                 (unsigned)pre_count);
    } else if (clr_rc == ESP_ERR_INVALID_RESPONSE) {
        format_nrc_err(err_out, err_cap, "DTC clear", dtc_uds_last_nrc());
    } else if (clr_rc == ESP_ERR_TIMEOUT) {
        copy_err(err_out, err_cap, "DTC clear transport timeout");
    } else {
        copy_err(err_out, err_cap, "DTC clear transport error");
    }

    esp_err_t stop_rc = feature_manager_request_stop(FEATURE_DTC);
    if (stop_rc != ESP_OK) {
        ESP_LOGE(TAG, "feature_manager request_stop rc=%d after clear", (int)stop_rc);
    }
    return clr_rc;
}
