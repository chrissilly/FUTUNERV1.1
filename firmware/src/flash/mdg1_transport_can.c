/*
 * mdg1_transport_can.c — production transport wrapping the project's
 * existing CAN+ISO-TP stack (can_manager + isotp-c). See header for the
 * lifecycle contract.
 *
 * Pattern lifted from dtc_feature.c::target_uds_request — same coordinator
 * acquire + can_manager_send_isotp + poll-receive shape. The difference:
 * we acquire ownership for the WHOLE orchestrator run (open→close), not
 * per-request, because the orchestrator emits ~6–10+ UDS exchanges in a
 * tight sequence and re-acquiring between each opens a race window where
 * the logger's polling or connection_manager's tester-present could
 * interleave.
 *
 * Hard rule: tester transmits on 0x7E0 only, listens on 0x7E8 only. The
 * isotp_link in can_manager.c is initialized with ECU_TX_ID=0x7E0 — that
 * is the single point of CAN-ID enforcement.
 */

#include "mdg1_transport_can.h"
#include "mdg1_flash_orchestrator_config.h"

#ifndef MDG1_FLASH_ORCHESTRATOR_HOST_BUILD

#include "can_manager.h"
#include "isotp_coordinator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG_CAN = "MDG1_TX_CAN";

typedef struct {
    bool initialized;
    bool coordinator_owned;
} can_ctx_t;

static can_ctx_t g_can = { 0 };

/* Tee the outbound UDS bytes to the boot log. Lets a host watching the
 * UART verify the orchestrator emitted the right bytes (e.g. SA seed
 * request = "27 11") without needing an external CAN sniffer. Format
 * matches the shadow transport's log so a grep across both is uniform. */
static void tee_uds_tx(const uint8_t *data, size_t len)
{
    /* Compact hex on one line, capped at MDG1_TRANSPORT_CAN_TEE_LOG_HEX_BYTES
     * bytes of hex (head of message — enough for SID + sub-function on
     * every flash-critical request). */
    char hex[MDG1_TRANSPORT_CAN_TEE_LOG_HEX_BYTES * 2 + 8];
    size_t cap = sizeof(hex) - 4;
    size_t lim = (len * 2 > cap) ? (cap / 2) : len;
    static const char hexd[] = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < lim; i++) {
        hex[o++] = hexd[(data[i] >> 4) & 0xF];
        hex[o++] = hexd[data[i] & 0xF];
    }
    if (lim < len) { hex[o++] = '.'; hex[o++] = '.'; hex[o++] = '.'; }
    hex[o] = '\0';
    ESP_LOGI(TAG_CAN, "TX 0x%03X len=%u %s",
             (unsigned)ECU_PHYSICAL_TX_ID, (unsigned)len, hex);
}

static esp_err_t can_send(void *ctx, const uint8_t *data, size_t len)
{
    can_ctx_t *cx = (can_ctx_t *)ctx;
    if (cx == NULL || !cx->initialized || !cx->coordinator_owned) {
        ESP_LOGE(TAG_CAN, "send while uninitialized/unarbitrated");
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > (size_t)UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Tee BEFORE handing to ISO-TP, so even ISO-TP failures still leave
     * a trace of what the orchestrator tried to emit. */
    tee_uds_tx(data, len);
    return can_manager_send_isotp(data, (uint16_t)len);
}

static esp_err_t can_recv(void *ctx, uint8_t *buf, size_t cap,
                          size_t *out_len, uint32_t timeout_ms)
{
    can_ctx_t *cx = (can_ctx_t *)ctx;
    if (cx == NULL || !cx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buf == NULL || out_len == NULL || cap == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0u;

    uint32_t start_ms =
        (uint32_t)((uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS);
    uint16_t cap16 = (cap > (size_t)UINT16_MAX) ? UINT16_MAX : (uint16_t)cap;

    for (;;) {
        uint16_t got = 0u;
        esp_err_t rc = can_manager_receive_isotp(buf, cap16, &got);
        if (rc == ESP_OK && got > 0u) {
            *out_len = (size_t)got;
            return ESP_OK;
        }
        uint32_t now_ms =
            (uint32_t)((uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS);
        if ((now_ms - start_ms) >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(MDG1_TRANSPORT_CAN_RECV_POLL_INTERVAL_MS));
    }
}

static esp_err_t can_flush(void *ctx)
{
    (void)ctx;
    /* No-op: isotp-c maintains its own segment state; can_manager_poll
     * naturally drains any queued frames. */
    return ESP_OK;
}

esp_err_t mdg1_transport_can_open(mdg1_uds_transport_t *out_iface)
{
    if (out_iface == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_iface, 0, sizeof(*out_iface));

    if (can_manager_get_state() != CAN_MGR_STATE_RUNNING) {
        ESP_LOGE(TAG_CAN,
                 "can_manager not RUNNING — Phase 2 transport refused");
        return ESP_ERR_INVALID_STATE;
    }

    if (!isotp_coordinator_request(ISOTP_OWNER_PHASE2_FLASH,
                                   MDG1_TRANSPORT_CAN_COORDINATOR_TIMEOUT_MS)) {
        ESP_LOGE(TAG_CAN,
                 "coordinator busy — could not acquire ISOTP_OWNER_PHASE2_FLASH "
                 "(current owner=%d)",
                 (int)isotp_coordinator_get_owner());
        return ESP_ERR_INVALID_STATE;
    }

    g_can.initialized       = true;
    g_can.coordinator_owned = true;

    out_iface->send_request  = can_send;
    out_iface->recv_response = can_recv;
    out_iface->flush         = can_flush;
    out_iface->ctx           = &g_can;

    ESP_LOGI(TAG_CAN, "production transport opened "
                      "(TX=0x%03X RX=0x%03X owner=PHASE2_FLASH)",
             (unsigned)ECU_PHYSICAL_TX_ID,
             (unsigned)ECU_PHYSICAL_RX_ID);
    return ESP_OK;
}

void mdg1_transport_can_close(mdg1_uds_transport_t *iface)
{
    if (iface == NULL) return;
    if (g_can.coordinator_owned) {
        isotp_coordinator_release(ISOTP_OWNER_PHASE2_FLASH);
        g_can.coordinator_owned = false;
    }
    g_can.initialized = false;
    memset(iface, 0, sizeof(*iface));
    ESP_LOGI(TAG_CAN, "production transport closed");
}

#else  /* MDG1_FLASH_ORCHESTRATOR_HOST_BUILD — host build excludes this TU */
typedef int mdg1_transport_can_host_build_excluded_t;
#endif
