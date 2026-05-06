#ifndef WOT_RECORDER_H
#define WOT_RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * wot_recorder — WOT detection state machine + CSV/gzip pipeline.
 *
 * The recorder is purely sample-driven: a producer (logger_manager
 * on target, the test harness on host) pushes one sample row at a
 * time via wot_recorder_feed_sample(). The recorder decides — based
 * on the throttle channel and the configured threshold/cooldown —
 * when to begin recording, when to end, and when to hand a finished
 * recording to its on-finish callback.
 *
 * It owns no task. It owns no clock. The caller injects:
 *   - a clock function (wot_recorder_clock_fn_t) that returns the
 *     current monotonic time in milliseconds. On target this is
 *     xTaskGetTickCount()*portTICK_PERIOD_MS; in tests it is a
 *     fast-forwardable fixture.
 *   - an on-finish callback (wot_recorder_on_finish_fn_t) invoked
 *     when a recording completes. The callback receives a pointer
 *     to the gzipped output buffer and its length; the buffer is
 *     owned by the recorder and is valid only for the duration of
 *     the callback. The callback typically writes the bytes to the
 *     uploader queue.
 *
 * Threading: the recorder is single-producer. All public functions
 * must be called from the same task that owns the producer
 * callback (the existing logger_manager poll callback path on
 * target). No internal locking.
 */

/* Index of the throttle variable inside the sample row, or
 * WOT_RECORDER_TRIGGER_VAR_NONE if the active logger profile does
 * not contain WOT_TRIGGER_VARIABLE_NAME. */
#define WOT_RECORDER_TRIGGER_VAR_NONE  ((uint8_t)0xFF)

/* Monotonic clock function pointer. Returns ms since some fixed
 * epoch. Only the delta matters; absolute origin is unused. */
typedef uint32_t (*wot_recorder_clock_fn_t)(void);

/* On-finish callback. The recorder hands `gzip_buf` of length
 * `gzip_len` to the caller. The buffer is owned by the recorder
 * and must be considered invalid once the callback returns. */
typedef void (*wot_recorder_on_finish_fn_t)(const uint8_t *gzip_buf,
                                            size_t          gzip_len,
                                            void           *user_ctx);

typedef struct {
    /* Required */
    wot_recorder_clock_fn_t       clock_now_ms;
    wot_recorder_on_finish_fn_t   on_finish;
    void                         *user_ctx;

    /* Index of the throttle variable inside fed sample rows, or
     * WOT_RECORDER_TRIGGER_VAR_NONE to disable triggering until
     * the active profile is set up. */
    uint8_t                       trigger_var_index;

    /* Number of variables in each fed sample row. Must be
     * <= WOT_RECORDER_MAX_VARS_PER_SAMPLE. */
    uint8_t                       variables_per_sample;

    /* Optional names array (variables_per_sample entries). Used to
     * build the CSV header. May be NULL — in which case generic
     * "v0,v1,..." names are used. */
    const char * const           *variable_names;
} wot_recorder_config_t;

esp_err_t wot_recorder_init(const wot_recorder_config_t *cfg);

void      wot_recorder_deinit(void);

/* Arm/disarm the recorder. Disarmed → no triggering, fed samples
 * are dropped. Arming is idempotent. Disarming flushes any in-
 * progress recording (the on-finish callback is invoked if there
 * is buffered data). */
esp_err_t wot_recorder_arm(void);
esp_err_t wot_recorder_disarm(void);

bool      wot_recorder_is_armed(void);

/* True if a recording is currently in progress (between trigger
 * and end). */
bool      wot_recorder_is_recording(void);

/* Feed one sample row. `values` length must match
 * cfg.variables_per_sample. Returns ESP_OK on accept, or an error
 * if the recorder is uninitialized / disarmed. */
esp_err_t wot_recorder_feed_sample(const float *values);

#ifdef __cplusplus
}
#endif

#endif /* WOT_RECORDER_H */
