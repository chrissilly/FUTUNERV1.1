#include "phase2_hil_preflight_commands.h"

#include "futuner_config.h"
#include "esp_log.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if FUTUNER_PHASE2_ENABLED
#include "mdg1_flash_orchestrator.h"
#include "mdg1_flash_orchestrator_config.h"
#include "mdg1_variant_manifest.h"
#include "mdg1_transport_shadow.h"
#include "mdg1_payload.h"
#include "filesystem/fs_manager.h"
#include "flash/phase2_hil_autostart.h"
#endif

static const char *TAG = "P2_HIL_PREFLIGHT";

/*
 * Absolute path inside the LittleFS mount where the shadow log lands.
 * fs_manager mounts FS_PARTITION_STORAGE at "/cal" — the same partition
 * fs_read / fs_write serve, so the log is retrievable via the existing
 * WS file-pull command using the relative path below.
 */
#define PHASE2_HIL_SHADOW_LOG_ABS_PATH    "/cal/phase2_hil_preflight.log"
#define PHASE2_HIL_SHADOW_LOG_FS_PATH     "phase2_hil_preflight.log"

static void emit_object(char *response, size_t response_size, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        strncpy(response, json, response_size - (size_t)1);
        response[response_size - (size_t)1] = '\0';
        free(json);
    }
    cJSON_Delete(root);
}

#if FUTUNER_PHASE2_ENABLED

/*
 * Build a minimal variant_t sufficient to walk the orchestrator UP TO
 * (but not into) the per-section loop. The halt gate fires after
 * fingerprint write and before the bin_path check + per-section loop,
 * so we don't need real section metadata, plaintext bin, or SHA-256-
 * validated AES key bytes.
 *
 * What the orchestrator *does* reach before the halt:
 *   - section_count check (must be 1..MDG1_VARIANT_MAX_SECTIONS)
 *   - mdg1_payload_get_aes_iface() != NULL check
 *   - phase_security_access (SA seed + SA2 key — sa2_run is weak,
 *     unlinked in firmware → seed-XOR sentinel fallback runs)
 *   - phase_fingerprint (uses MDG1_PROG_FINGERPRINT_BYTES default)
 * The shadow transport synthesizes all responses procedurally for the
 * flash-critical SIDs in this window, so a NULL playback fixture is
 * fine — verified by Layer 1 host harness.
 */
static void build_preflight_variant(mdg1_variant_t *v)
{
    memset(v, 0, sizeof(*v));
    snprintf(v->boxcode, sizeof(v->boxcode), "4K0907557G__0003");
    snprintf(v->variant_name, sizeof(v->variant_name),
             "MG1 CS002IFX RS (HIL stub)");
    v->sa2_script_len = 0;
    v->section_count = 5;
    static const char *kSecNames[5] = { "ASW1", "ASW2", "ASW3", "CBOOT", "CAL" };
    for (size_t i = 0; i < 5; i++) {
        v->sections[i].block_id = (uint8_t)(0x02 + i);
        v->sections[i].plaintext_size = 0;
        snprintf(v->sections[i].name, sizeof(v->sections[i].name),
                 "%s", kSecNames[i]);
    }
}

typedef struct {
    int events;
    int erase_seen;
    int halt_seen;
    int done_seen;
    int failed_seen;
} preflight_progress_t;

static void preflight_progress_cb(const mdg1_flash_progress_t *p, void *ux)
{
    preflight_progress_t *pl = (preflight_progress_t *)ux;
    pl->events++;
    switch (p->phase) {
        case MDG1_FLASH_PHASE_SECTION_ERASE:           pl->erase_seen++;  break;
        case MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE:   pl->halt_seen++;   break;
        case MDG1_FLASH_PHASE_DONE:                    pl->done_seen++;   break;
        case MDG1_FLASH_PHASE_FAILED:                  pl->failed_seen++; break;
        default: break;
    }
}

esp_err_t cmd_phase2_hil_preflight(int fd,
                                   const char *params,
                                   char *response,
                                   size_t response_size)
{
    (void)fd;
    const char *mode = (params != NULL && *params != '\0') ? params : "shadow";
    cJSON *root = cJSON_CreateObject();

    if (strcmp(mode, "shadow") != 0) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
            "Only 'shadow' mode is supported in this build. Prod CAN path "
            "is gated behind 'go HIL' authorization (see "
            "docs/HIL_PREFLIGHT_RS7_CAL_FLASH_READINESS.md).");
        emit_object(response, response_size, root);
        return ESP_OK;
    }

    if (mdg1_payload_get_aes_iface() == NULL) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
            "AES iface not registered — check FUTUNER_PHASE2_ENABLED "
            "and main.c's mdg1_aes_mbedtls_register() call.");
        emit_object(response, response_size, root);
        return ESP_OK;
    }

    if (!fs_manager_is_mounted(FS_PARTITION_STORAGE)) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
            "LittleFS partition (/cal) not mounted — cannot write shadow log.");
        emit_object(response, response_size, root);
        return ESP_OK;
    }

    mdg1_variant_t v;
    build_preflight_variant(&v);

    mdg1_uds_transport_t iface;
    esp_err_t e = mdg1_transport_shadow_open(PHASE2_HIL_SHADOW_LOG_ABS_PATH,
                                              NULL, &iface);
    if (e != ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error",
            "shadow transport open failed (file write?)");
        cJSON_AddNumberToObject(root, "rc", (double)e);
        mdg1_variant_manifest_clear(&v);
        emit_object(response, response_size, root);
        return ESP_OK;
    }

    preflight_progress_t plog;
    memset(&plog, 0, sizeof(plog));

    mdg1_flash_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.variant = &v;
    plan.plaintext_bin_path = NULL;
    plan.use_default_fingerprint = true;
    plan.hil_halt_before_erase = true;

    esp_err_t rc = mdg1_flash_orchestrator_run(&plan, &iface,
                                                preflight_progress_cb, &plog);
    mdg1_transport_shadow_close(&iface);
    mdg1_variant_manifest_clear(&v);

    bool halt_ok = (rc == ESP_ERR_NOT_FINISHED) &&
                   (plog.halt_seen == 1) &&
                   (plog.erase_seen == 0) &&
                   (plog.done_seen == 0);

    cJSON_AddBoolToObject(root, "ok", halt_ok);
    cJSON_AddStringToObject(root, "log_path", PHASE2_HIL_SHADOW_LOG_ABS_PATH);
    cJSON_AddStringToObject(root, "log_fs_read_path", PHASE2_HIL_SHADOW_LOG_FS_PATH);
    cJSON_AddNumberToObject(root, "rc", (double)rc);
    cJSON_AddNumberToObject(root, "events", (double)plog.events);
    cJSON_AddNumberToObject(root, "halt_events_seen", (double)plog.halt_seen);
    cJSON_AddNumberToObject(root, "erase_events_seen", (double)plog.erase_seen);
    if (!halt_ok) {
        cJSON_AddStringToObject(root, "error",
            "HIL halt did not fire as expected — inspect rc / halt_events_seen "
            "/ erase_events_seen and the shadow log.");
    }
    ESP_LOGI(TAG,
             "phase2_hil_preflight shadow run: rc=%d halt=%d erase=%d events=%d log=%s",
             (int)rc, plog.halt_seen, plog.erase_seen, plog.events,
             PHASE2_HIL_SHADOW_LOG_ABS_PATH);

    emit_object(response, response_size, root);
    return ESP_OK;
}

esp_err_t cmd_phase2_hil_preflight_arm(int fd,
                                       const char *params,
                                       char *response,
                                       size_t response_size)
{
    (void)fd;
    /* params may arrive as either the bare string "shadow"/"prod" (serial)
     * or a JSON object {"mode":"shadow"|"prod"} (WS dispatcher pre-prints
     * the object). Default = shadow. */
    phase2_hil_mode_t mode = PHASE2_HIL_MODE_SHADOW;
    if (params != NULL && *params != '\0') {
        if (strstr(params, "prod") != NULL) {
            mode = PHASE2_HIL_MODE_PROD;
        } else if (strstr(params, "shadow") != NULL) {
            mode = PHASE2_HIL_MODE_SHADOW;
        }
    }
    cJSON *root = cJSON_CreateObject();
    esp_err_t e = phase2_hil_autostart_arm_with_mode(mode);
    bool ok = (e == ESP_OK);
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddBoolToObject(root, "armed", ok);
    cJSON_AddStringToObject(root, "mode",
                            mode == PHASE2_HIL_MODE_PROD ? "prod" : "shadow");
    if (!ok) {
        cJSON_AddStringToObject(root, "error",
            "phase2_hil_autostart_arm failed — check NVS partition state");
        cJSON_AddNumberToObject(root, "rc", (double)e);
    }
    ESP_LOGI(TAG, "phase2_hil_preflight_arm: mode=%d ok=%d rc=%d",
             (int)mode, (int)ok, (int)e);
    emit_object(response, response_size, root);
    return ESP_OK;
}

#else /* FUTUNER_PHASE2_ENABLED == 0 */

esp_err_t cmd_phase2_hil_preflight(int fd,
                                   const char *params,
                                   char *response,
                                   size_t response_size)
{
    (void)fd;
    (void)params;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error",
        "Phase 2 not enabled in this build. Rebuild with "
        "-DFUTUNER_PHASE2_ENABLED=1.");
    ESP_LOGW(TAG, "phase2_hil_preflight invoked but FUTUNER_PHASE2_ENABLED=0");
    emit_object(response, response_size, root);
    return ESP_OK;
}

esp_err_t cmd_phase2_hil_preflight_arm(int fd,
                                       const char *params,
                                       char *response,
                                       size_t response_size)
{
    (void)fd;
    (void)params;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error",
        "Phase 2 not enabled in this build. Rebuild with "
        "-DFUTUNER_PHASE2_ENABLED=1.");
    ESP_LOGW(TAG, "phase2_hil_preflight_arm invoked but FUTUNER_PHASE2_ENABLED=0");
    emit_object(response, response_size, root);
    return ESP_OK;
}

#endif
