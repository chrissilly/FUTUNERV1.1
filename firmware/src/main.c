#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "can/can_manager.h"
#include "nvs/nvs_manager.h"
#include "nvs/ecu_info.h"
#include "config/futuner_config.h"
#include "state_machine/connection_manager.h"
#include "wifi/wifi_ap.h"
#include "websocket/ws_server.h"
#include "commands/command_handler.h"
#include "error/error_tracker.h"
#include "filesystem/fs_manager.h"
#include "logger/logger_profile.h"
#include "logger/wot_logger.h"
#include "ecu_write/ecu_write.h"
#include "flex_fuel/flex_fuel.h"
#include "commands/serial_console.h"
#include "feature_manager/feature_manager.h"
#include "dtc/dtc.h"

static const char *TAG = "MAIN";

/* WIFI_AP_IP is defined in wifi/wifi_ap.h (included above). C1 fix:
 * password removed from source — loaded from NVS at runtime. */

#define CAN_TASK_STACK_SIZE 8192
#define CAN_TASK_PRIORITY   5
#define CAN_TASK_CORE       1

/* P-66: cadence at which can_task pumps wot_logger_tick() (the WOT
 * uploader's retry driver). 1 Hz matches the "≈1 Hz is plenty"
 * contract in wot_uploader.h; the uploader self-rate-limits to
 * WOT_UPLOAD_RETRY_INTERVAL_MS internally, so this only needs to be
 * frequent enough not to add latency to that interval. */
#define MAIN_WOT_TICK_INTERVAL_MS 1000

/**
 * CAN + connection manager task.
 * Runs on a dedicated task with its own stack so the blocking CAN
 * transmits and deep isotp/connection_manager call chains don't
 * starve the WiFi/LWIP tasks or overflow the main task stack.
 */
static void can_task(void *arg) {
    ESP_LOGI(TAG, "CAN task started on core %d", xPortGetCoreID());

    /* P-72: pin logger_profile_apply() to this task. Any subsequent
     * caller from another context (e.g. a WS command handler) gets
     * rejected at the door instead of corrupting shared
     * logger_manager state in a clear/add race. */
    logger_profile_set_owner_task(xTaskGetCurrentTaskHandle());

    connection_manager_start_connection();

    uint32_t last_log_ticks = xTaskGetTickCount();
    uint32_t last_wot_tick_ticks = xTaskGetTickCount();
    int log_counter = 0;

    while (1) {
        can_manager_poll();
        connection_manager_update();
        ecu_write_poll();
        flex_fuel_update();

        uint32_t now_ticks = xTaskGetTickCount();

        /* P-66: pump the WOT uploader's retry tick at ~1 Hz so queued
         * logs actually upload. wot_logger_tick() no-ops when the
         * uploader isn't running. */
        if ((now_ticks - last_wot_tick_ticks) >= pdMS_TO_TICKS(MAIN_WOT_TICK_INTERVAL_MS)) {
            wot_logger_tick();
            last_wot_tick_ticks = now_ticks;
        }

        if ((now_ticks - last_log_ticks) >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "Heartbeat: System running... count=%d", log_counter++);
            last_log_ticks = now_ticks;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting FUTUNER v%s", FUTUNER_VERSION_STRING);

    esp_err_t err = error_tracker_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize error tracker");
        return;
    }

    err = nvs_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS manager");
        return;
    }

    err = wifi_ap_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi AP");
        return;
    }

    err = wifi_ap_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi AP");
        return;
    }

    err = ws_server_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket server");
        return;
    }

    /* Web server starts lazily when a WiFi client connects (wifi_ap.c event handler).
       No need to call ws_server_start() here - saves resources when no one is connected. */

    err = command_handler_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize command handler");
        return;
    }

    ws_server_set_message_handler(command_handler_process_message);

    err = fs_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize filesystem manager");
        return;
    }

    err = fs_manager_mount_partition(FS_PARTITION_STORAGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount storage partition");
        error_tracker_log(ERROR_CATEGORY_SYSTEM, ERROR_SEVERITY_ERROR,
                        "Failed to mount filesystem");
    } else {
        ESP_LOGI(TAG, "Filesystem mounted successfully");

        /* Initialize logger profile system (creates /cal/profiles/ dir) */
        err = logger_profile_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Logger profile init failed (non-fatal)");
        }

        err = flex_fuel_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Flex fuel init failed (non-fatal)");
        }
    }

    err = can_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize CAN manager");
        return;
    }

    err = can_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start CAN manager");
        return;
    }

    err = connection_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize connection manager");
        return;
    }

    /* Feature manager — central ON/OFF arbiter. Must be initialized
       before any feature_manager_register() call so wot_logger and
       future features can plug in. */
    err = feature_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize feature manager");
        return;
    }

    /* WOT logger — registers itself with feature_manager. Defaults
       to inactive; activated only via wot_log_start command. */
    err = wot_logger_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WOT logger init failed (non-fatal): rc=%d", (int)err);
    }

    /* DTC read/clear feature — registers itself with feature_manager.
       Defaults to inactive; activated on-demand via dtc_read /
       dtc_clear commands which arbitrate through feature_manager. */
    err = dtc_feature_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DTC feature init failed (non-fatal): rc=%d", (int)err);
    }

    /* License cache + VIN pairing — wires the cloud round-trip
       transports through esp_http_client and nvs_manager, then
       hands the FEATURE_VIN_PAIRING descriptor to feature_manager.
       Pair-or-refresh runs on-demand via the vin_pair_now command. */
    {
        extern esp_err_t main_init_license_and_vin_pairing(void);
        err = main_init_license_and_vin_pairing();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "license / vin_pairing init failed (non-fatal): rc=%d", (int)err);
        }
    }

    /* SBF live tune orchestrator — registers FEATURE_LIVE_TUNE with
       feature_manager, wires loader/applier/downloader behind their
       on-target adapters. Includes the wot_uploader license gate
       wiring (Prompt 4 follow-up). */
    {
        extern esp_err_t main_init_sbf_orchestrator(void);
        err = main_init_sbf_orchestrator();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sbf_orchestrator init failed (non-fatal): rc=%d", (int)err);
        }
    }

    /* Phase 2 full binary flash — wires the mbedtls-backed AES iface
       into mdg1_payload and (in a follow-up prompt) registers
       FEATURE_PHASE2_FLASH with feature_manager. Gated by
       FUTUNER_PHASE2_ENABLED in config/futuner_config.h; default 0
       in customer firmware so the orchestrator stays dormant.

       Override at build time via `idf.py build -DFUTUNER_PHASE2_ENABLED=1`
       to bring the orchestrator online for bench validation. */
#if FUTUNER_PHASE2_ENABLED
    {
        extern void mdg1_aes_mbedtls_register(void);
        mdg1_aes_mbedtls_register();
        ESP_LOGI(TAG, "Phase 2 flash: mbedtls AES iface registered (FUTUNER_PHASE2_ENABLED=1)");
    }
    /* HIL preflight NVS-armed autostart. Reads the armed flag, clears
     * it BEFORE running anything (one-shot, crash-safe), and either
     * (dry-run build) just logs the marker or (full build) runs the
     * shadow preflight + dumps the log over UART. Safe no-op when
     * not armed. See flash/phase2_hil_autostart.h. */
    {
        extern esp_err_t phase2_hil_autostart_run_if_armed(void);
#ifdef PHASE2_HIL_AUTOSTART_FORCE_ARM_THIS_BUILD
        /* Bench-only helper to validate the autostart pipeline without
         * needing a working command-input channel. Forces the armed
         * flag on this boot. NEVER ship this in customer firmware. */
        extern esp_err_t phase2_hil_autostart_arm(void);
        ESP_LOGW(TAG, "PHASE2_HIL_AUTOSTART_FORCE_ARM_THIS_BUILD set — "
                      "forcing armed flag (shadow mode) for one-shot test");
        phase2_hil_autostart_arm();
#endif
#ifdef PHASE2_HIL_AUTOSTART_FORCE_ARM_PROD_THIS_BUILD
        /* Bench-only helper, prod variant. Same caveats as the shadow
         * helper. Use ONE OR THE OTHER, not both — last write wins. */
        extern esp_err_t phase2_hil_autostart_arm_with_mode(int);
        ESP_LOGW(TAG, "PHASE2_HIL_AUTOSTART_FORCE_ARM_PROD_THIS_BUILD set — "
                      "forcing armed flag (PROD mode) — quiet bench expected");
        phase2_hil_autostart_arm_with_mode(2 /*PHASE2_HIL_MODE_PROD*/);
#endif
        phase2_hil_autostart_run_if_armed();
    }
#endif

    ESP_LOGI(TAG, "System initialized");
    ESP_LOGI(TAG, "Device Serial: 0x%012llX", wifi_ap_get_serial_number());
    /* C1 fix: do not log password in plaintext */
    ESP_LOGI(TAG, "WiFi AP: SSID=%s, IP=%s", wifi_ap_get_ssid(), WIFI_AP_IP);
    ESP_LOGI(TAG, "Web server: http://%s/ (starts on first WiFi client)", WIFI_AP_IP);

    /* Launch CAN + connection manager on its own task so app_main returns.
       This frees the main task stack and lets WiFi/LWIP run without contention. */
    BaseType_t ret = xTaskCreatePinnedToCore(
        can_task,
        "can_task",
        CAN_TASK_STACK_SIZE,
        NULL,
        CAN_TASK_PRIORITY,
        NULL,
        CAN_TASK_CORE
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create CAN task");
        return;
    }

    /* Start serial console (USB UART) for direct config commands */
    serial_console_start();

    ESP_LOGI(TAG, "CAN task launched, app_main exiting");
    /* app_main returns - FreeRTOS reclaims the main task stack */
}
