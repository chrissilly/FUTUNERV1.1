#include "phase2_hil_autostart.h"
#include "mdg1_flash_orchestrator_config.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(MDG1_HIL_AUTOSTART_DRY_RUN)
/* Phase A wired the autostart in dry-run mode (no orchestrator).
 * Phase B flips this to 0 to invoke the actual shadow preflight + UART
 * base64 dump. Override at build-time with -DMDG1_HIL_AUTOSTART_DRY_RUN=1
 * to fall back to the dry-run lifecycle. */
#define MDG1_HIL_AUTOSTART_DRY_RUN 0
#endif

#if !MDG1_HIL_AUTOSTART_DRY_RUN
#include "mdg1_flash_orchestrator.h"
#include "mdg1_variant_manifest.h"
#include "mdg1_transport_shadow.h"
#include "mdg1_transport_can.h"
#include "mdg1_uds_transport.h"
#include "mdg1_payload.h"
#include "filesystem/fs_manager.h"
#endif

static const char *TAG = "P2_HIL_AUTO";

/* ------------------------------------------------------------------ */
/* NVS helpers                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t nvs_set_armed_byte(uint8_t value)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(MDG1_HIL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s, rw) failed: %d", MDG1_HIL_NVS_NAMESPACE, (int)e);
        return e;
    }
    e = nvs_set_u8(h, MDG1_HIL_NVS_KEY_ARMED, value);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

static esp_err_t nvs_get_armed_byte(uint8_t *out)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(MDG1_HIL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        return ESP_OK;
    }
    if (e != ESP_OK) return e;
    uint8_t v = 0;
    e = nvs_get_u8(h, MDG1_HIL_NVS_KEY_ARMED, &v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        return ESP_OK;
    }
    if (e != ESP_OK) return e;
    *out = v;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t phase2_hil_autostart_arm_with_mode(phase2_hil_mode_t mode)
{
    if (mode != PHASE2_HIL_MODE_SHADOW && mode != PHASE2_HIL_MODE_PROD) {
        ESP_LOGE(TAG, "arm_with_mode: invalid mode=%d", (int)mode);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t e = nvs_set_armed_byte((uint8_t)mode);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "armed — next boot will run phase2_hil_preflight (mode=%s)",
                 mode == PHASE2_HIL_MODE_SHADOW ? "shadow" : "prod");
    } else {
        ESP_LOGE(TAG, "arm failed: %d", (int)e);
    }
    return e;
}

esp_err_t phase2_hil_autostart_arm(void)
{
    return phase2_hil_autostart_arm_with_mode(PHASE2_HIL_MODE_SHADOW);
}

bool phase2_hil_autostart_is_armed(void)
{
    uint8_t v = 0;
    if (nvs_get_armed_byte(&v) != ESP_OK) return false;
    return v != 0;
}

#if !MDG1_HIL_AUTOSTART_DRY_RUN

/* ------------------------------------------------------------------ */
/* Base64 dump helpers (Phase B path)                                 */
/* ------------------------------------------------------------------ */

static const char B64_TBL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_emit_chunk(const uint8_t *in, size_t n, char out[5])
{
    uint32_t v = ((uint32_t)in[0]) << 16;
    if (n > 1) v |= ((uint32_t)in[1]) << 8;
    if (n > 2) v |= (uint32_t)in[2];
    out[0] = B64_TBL[(v >> 18) & 0x3F];
    out[1] = B64_TBL[(v >> 12) & 0x3F];
    out[2] = (n > 1) ? B64_TBL[(v >> 6) & 0x3F] : '=';
    out[3] = (n > 2) ? B64_TBL[v & 0x3F]        : '=';
    out[4] = '\0';
}

static void dump_log_as_base64(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for base64 dump", path);
        return;
    }
    /* Print marker on its own line. Use printf so it lands on the
     * console with no log-prefix interference (the tooling greps
     * the exact literal). */
    printf("\n%s\n", MDG1_HIL_AUTOSTART_LOG_BEGIN_MARKER);
    uint8_t inbuf[48];   /* multiple of 3 → no padding mid-stream */
    char    outbuf[65];
    size_t  got;
    while ((got = fread(inbuf, 1, sizeof(inbuf), f)) > 0) {
        char *o = outbuf;
        size_t i = 0;
        while (i < got) {
            size_t take = (got - i) >= 3 ? 3 : (got - i);
            char chunk[5];
            b64_emit_chunk(&inbuf[i], take, chunk);
            memcpy(o, chunk, 4);
            o += 4;
            i += take;
        }
        *o = '\0';
        printf("%s\n", outbuf);
    }
    fclose(f);
    printf("%s\n", MDG1_HIL_AUTOSTART_LOG_END_MARKER);
}

/* ------------------------------------------------------------------ */
/* Orchestrator invocation (Phase B path)                             */
/* ------------------------------------------------------------------ */

/* Minimal variant_t — same shape as cmd_phase2_hil_preflight uses.
 * Halt fires before the per-section loop touches sections. */
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
    int halt_seen;
    int erase_seen;
} preflight_counters_t;

static void preflight_progress_cb(const mdg1_flash_progress_t *p, void *ux)
{
    preflight_counters_t *c = (preflight_counters_t *)ux;
    c->events++;
    if (p->phase == MDG1_FLASH_PHASE_SECTION_ERASE)         c->erase_seen++;
    if (p->phase == MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE) c->halt_seen++;
}

static esp_err_t run_shadow_preflight(preflight_counters_t *counters)
{
    if (mdg1_payload_get_aes_iface() == NULL) {
        ESP_LOGE(TAG, "AES iface unset — Phase 2 init didn't run");
        return ESP_ERR_INVALID_STATE;
    }
    if (!fs_manager_is_mounted(FS_PARTITION_STORAGE)) {
        ESP_LOGE(TAG, "/cal not mounted — cannot write shadow log");
        return ESP_ERR_INVALID_STATE;
    }

    mdg1_variant_t v;
    build_preflight_variant(&v);

    mdg1_uds_transport_t iface;
    esp_err_t e = mdg1_transport_shadow_open(MDG1_HIL_AUTOSTART_LOG_ABS_PATH,
                                              NULL, &iface);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "shadow open failed: %d", (int)e);
        return e;
    }

    mdg1_flash_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.variant = &v;
    plan.plaintext_bin_path = NULL;
    plan.use_default_fingerprint = true;
    plan.hil_halt_before_erase = true;

    esp_err_t rc = mdg1_flash_orchestrator_run(&plan, &iface,
                                                preflight_progress_cb,
                                                counters);
    mdg1_transport_shadow_close(&iface);
    mdg1_variant_manifest_clear(&v);
    return rc;
}

/* Prod-mode preflight. Identical orchestration as run_shadow_preflight,
 * differs ONLY in the transport opener — mdg1_transport_can_open instead
 * of mdg1_transport_shadow_open. Halt-before-erase is set on the plan,
 * so the orchestrator returns ESP_ERR_NOT_FINISHED after the fingerprint
 * write succeeds (when there IS an ECU on the bus). On a quiet bench
 * (no ECU), the SA seed-request recv times out and the orchestrator
 * returns ESP_FAIL — the tee log in mdg1_transport_can.c::can_send
 * still captures the SA seed-request bytes that left the orchestrator,
 * which is the validation signal Step B-2 cares about. */
static esp_err_t run_prod_preflight(preflight_counters_t *counters)
{
    if (mdg1_payload_get_aes_iface() == NULL) {
        ESP_LOGE(TAG, "AES iface unset — Phase 2 init didn't run");
        return ESP_ERR_INVALID_STATE;
    }

    mdg1_variant_t v;
    build_preflight_variant(&v);

    mdg1_uds_transport_t iface;
    esp_err_t e = mdg1_transport_can_open(&iface);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "prod transport open failed: %d", (int)e);
        return e;
    }

    mdg1_flash_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.variant = &v;
    plan.plaintext_bin_path = NULL;
    plan.use_default_fingerprint = true;
    plan.hil_halt_before_erase = true;   /* CRITICAL — must be true for prod */

    ESP_LOGI(TAG, "running prod preflight (halt-before-erase ENFORCED). "
                  "On quiet bench expect SA timeout; on car expect halt at "
                  "MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE.");
    esp_err_t rc = mdg1_flash_orchestrator_run(&plan, &iface,
                                                preflight_progress_cb,
                                                counters);
    mdg1_transport_can_close(&iface);
    mdg1_variant_manifest_clear(&v);
    return rc;
}

#endif /* !MDG1_HIL_AUTOSTART_DRY_RUN */

esp_err_t phase2_hil_autostart_run_if_armed(void)
{
    uint8_t mode_byte = 0;
    esp_err_t e = nvs_get_armed_byte(&mode_byte);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "NVS read failed (e=%d) — assuming not armed", (int)e);
        return ESP_OK;
    }
    if (mode_byte == PHASE2_HIL_MODE_NONE) {
        return ESP_OK;
    }

    /* Clear the flag IMMEDIATELY — before invoking the orchestrator —
     * so any crash / watchdog / panic during the run does not loop
     * the dongle into a perpetual preflight cycle on every reboot. */
    e = nvs_set_armed_byte(0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "failed to clear armed flag (e=%d) — refusing to "
                      "run preflight to avoid reboot loop", (int)e);
        return e;
    }
    const char *mode_name =
        (mode_byte == PHASE2_HIL_MODE_SHADOW) ? "shadow" :
        (mode_byte == PHASE2_HIL_MODE_PROD)   ? "prod"   : "?";
    ESP_LOGI(TAG, "armed flag detected (mode=%s) + cleared (one-shot)", mode_name);

#if MDG1_HIL_AUTOSTART_DRY_RUN
    (void)mode_name;
    ESP_LOGI(TAG, "%s — dry-run (orchestrator not wired this build)",
             MDG1_HIL_AUTOSTART_COMPLETE_MARKER);
    return ESP_OK;
#else
    preflight_counters_t c = {0};
    esp_err_t rc;
    const char *log_path;
    if (mode_byte == PHASE2_HIL_MODE_PROD) {
        rc = run_prod_preflight(&c);
        log_path = MDG1_HIL_AUTOSTART_LOG_PROD_ABS_PATH;
    } else {
        rc = run_shadow_preflight(&c);
        log_path = MDG1_HIL_AUTOSTART_LOG_ABS_PATH;
    }

    /* "ok" semantics differ per mode:
     *   shadow: orchestrator runs end-to-end up to halt → ESP_ERR_NOT_FINISHED
     *           + halt_seen=1 + erase_seen=0 is success.
     *   prod (quiet bench): SA recv times out → ESP_FAIL is EXPECTED. The
     *           validation signal is the TX-tee log emitting the SA seed
     *           request bytes BEFORE the timeout. erase_seen must still be 0.
     *   prod (with ECU): orchestrator halts at fingerprint-ack → same as shadow. */
    bool ok;
    if (mode_byte == PHASE2_HIL_MODE_PROD) {
        ok = (c.erase_seen == 0) &&
             (rc == ESP_ERR_NOT_FINISHED || rc == ESP_FAIL || rc == ESP_ERR_TIMEOUT);
    } else {
        ok = (rc == ESP_ERR_NOT_FINISHED) && (c.halt_seen == 1) && (c.erase_seen == 0);
    }
    ESP_LOGI(TAG,
             "%s — mode=%s rc=%d halt=%d erase=%d events=%d ok=%d log=%s",
             MDG1_HIL_AUTOSTART_COMPLETE_MARKER, mode_name,
             (int)rc, c.halt_seen, c.erase_seen, c.events, (int)ok, log_path);
    if (ok && mode_byte == PHASE2_HIL_MODE_SHADOW) {
        /* Only shadow has a meaningful file to base64-dump; prod's CAN
         * traffic is captured via the TX-tee ESP_LOGI lines above and
         * via an external sniffer (Candlelight) when running on car. */
        dump_log_as_base64(log_path);
    }
    return ok ? ESP_OK : ESP_FAIL;
#endif
}
