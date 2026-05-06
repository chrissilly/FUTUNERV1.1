#ifndef SBF_ORCHESTRATOR_H
#define SBF_ORCHESTRATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "sbf_config.h"
#include "sbf_loader.h"
#include "sbf_applier.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sbf_orchestrator — feature lifecycle + state machine + worker
 * queue for live tune.
 *
 * State machine (visible to status command):
 *   IDLE      → no SBF loaded, no apply pending.
 *   LOADING   → SBF being downloaded or read from cache.
 *   APPLYING  → worker is iterating the applier.
 *   ACTIVE    → applied; idle waiting for set/stop.
 *   ERROR     → apply or load failed; stop() returns to IDLE.
 *
 * The orchestrator owns a small request queue. live_tune_start
 * enqueues a "load+apply" request; live_tune_set enqueues a
 * "re-apply with new params" request. The worker drains the queue
 * one entry at a time, emitting WS events on transitions.
 *
 * Sean's Q4 design: task-driven, NOT synchronous. start() returns
 * immediately after enqueueing. The worker (real FreeRTOS task on
 * target; explicit drain function on host) does the actual apply.
 */

/* Operational states, surfaced via live_tune_status. */
typedef enum {
    SBF_STATE_IDLE = 0,
    SBF_STATE_LOADING,
    SBF_STATE_APPLYING,
    SBF_STATE_ACTIVE,
    SBF_STATE_ERROR,
} sbf_state_t;

/* Snapshot returned by sbf_orchestrator_status / live_tune_status. */
typedef struct {
    sbf_state_t state;
    uint8_t     current_stage;          /* 0 if no SBF loaded */
    uint8_t     current_ethanol_pct;    /* last requested */
    uint32_t    last_apply_ms;          /* clock_now_ms at last successful apply */
    uint32_t    last_apply_elapsed_ms;  /* elapsed across last apply */
    char        sbf_filename[64];       /* /storage/sbf/stage<N>.sbf or empty */
    char        last_error[SBF_LAST_ERROR_MAX]; /* last failure reason; empty on success */
} sbf_status_snapshot_t;

/* WS event sink — emits a JSON string. The orchestrator's worker
 * calls this on every state transition the UI cares about
 * (apply_started / apply_progress / apply_completed / apply_failed
 * / unload). On target the sink wraps ws_server_broadcast_text;
 * the host test captures events into a buffer for assertions. */
typedef void (*sbf_event_sink_fn_t)(const char *json, void *user_ctx);

/* VIN source — used at start() to gate license_can_run_feature on
 * the current ECU VIN. Returns NULL or empty string when no VIN is
 * known (license module's contract handles that gracefully). */
typedef const char *(*sbf_vin_source_fn_t)(void);

/* Monotonic ms clock. Test-controllable (per Sean's Q4 directive
 * about reusing the wot_recorder pattern). */
typedef uint32_t (*sbf_clock_fn_t)(void);

/* Boxcode source — used by the applier to look up mid_byte +
 * address_offset via sbf_variants_lookup. */
typedef const char *(*sbf_boxcode_source_fn_t)(void);

typedef struct {
    /* Loader iface — wraps frozen scal/bdef on target; mocked in tests. */
    sbf_loader_iface_t       loader;

    /* Apply-time deps — write fn + clock + progress. The orchestrator
     * owns the progress callback and adapts it into the deps. */
    sbf_apply_write_fn_t     ecu_write;
    void                    *ecu_write_ctx;

    /* WS event sink. */
    sbf_event_sink_fn_t      event_sink;
    void                    *event_sink_ctx;

    /* Sources. */
    sbf_vin_source_fn_t      vin_source;
    sbf_boxcode_source_fn_t  boxcode_source;
    sbf_clock_fn_t           clock_now_ms;
} sbf_orchestrator_config_t;

esp_err_t sbf_orchestrator_init(const sbf_orchestrator_config_t *cfg);
void      sbf_orchestrator_deinit(void);

/* Idempotent. Hands the FEATURE_LIVE_TUNE descriptor to
 * feature_manager_register(). Called from sbf_orchestrator_init();
 * also exposed for test scaffolding. */
esp_err_t sbf_orchestrator_register_with_feature_manager(void);

/* feature_manager descriptor accessors. */
bool sbf_orchestrator_is_running(void);

/* ------------------------------------------------------------------ */
/* Public live-tune ops (called from sbf_commands.c)                    */
/* ------------------------------------------------------------------ */

esp_err_t sbf_orchestrator_live_tune_start(uint8_t stage,
                                           uint8_t ethanol_pct,
                                           char   *err_out,
                                           size_t  err_cap);

esp_err_t sbf_orchestrator_live_tune_set(uint8_t stage,
                                         uint8_t ethanol_pct,
                                         char   *err_out,
                                         size_t  err_cap);

esp_err_t sbf_orchestrator_live_tune_stop(char   *err_out,
                                          size_t  err_cap);

void      sbf_orchestrator_live_tune_status(sbf_status_snapshot_t *out);

/* ------------------------------------------------------------------ */
/* Worker drive                                                         */
/* ------------------------------------------------------------------ */

/*
 * Drains one queued request synchronously. On target this is called
 * from the worker FreeRTOS task in a loop. On host the test calls
 * it directly to advance state without spinning a real task.
 *
 * Returns ESP_OK on processed-one or queue-empty; an esp_err_t code
 * on apply failure (the orchestrator transitions to ERROR and emits
 * the failed event regardless).
 */
esp_err_t sbf_orchestrator_drain_one(void);

/* Test-only: returns the queue depth (number of pending requests). */
size_t sbf_orchestrator_queue_depth(void);

#ifdef __cplusplus
}
#endif

#endif /* SBF_ORCHESTRATOR_H */
