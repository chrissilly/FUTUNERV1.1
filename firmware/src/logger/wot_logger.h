#ifndef WOT_LOGGER_H
#define WOT_LOGGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * wot_logger — lifecycle / feature_manager glue for the WOT logging
 * feature. Owns no detection or upload logic itself; that lives in
 * wot_recorder and wot_uploader.
 *
 * Boot flow:
 *   wot_logger_init()
 *     ↳ initializes the recorder + uploader sub-modules
 *     ↳ registers itself with feature_manager as FEATURE_WOT_LOGGING
 *
 * Runtime flow (driven by feature_manager arbitration):
 *   feature_manager_request_start(FEATURE_WOT_LOGGING)
 *     ↳ wot_logger_start()  arms recorder + starts uploader
 *   feature_manager_request_stop(FEATURE_WOT_LOGGING)
 *     ↳ wot_logger_stop()   disarms recorder + stops uploader
 *
 * Per the project ON/OFF rule, no work happens unless start() has
 * been called. Init does NOT auto-start; init only wires up
 * registrations.
 */

/*
 * Initialize the WOT logger module. Wires up the recorder and
 * uploader sub-modules and registers the feature with feature_manager.
 * Must be called exactly once at boot, AFTER feature_manager_init()
 * and AFTER logger_manager_init().
 */
esp_err_t wot_logger_init(void);

/*
 * Idempotent. Hands the WOT logger feature descriptor to
 * feature_manager_register(). Called from wot_logger_init(); also
 * exposed for test scaffolding that wants to use a fresh
 * feature_manager instance without re-running the full init path.
 */
esp_err_t wot_logger_register_with_feature_manager(void);

/*
 * Feature start/stop hooks invoked by feature_manager. Do NOT call
 * directly — go through feature_manager_request_start/stop so
 * arbitration with other features is respected.
 *
 * start() arms the recorder and kicks the uploader's retry loop.
 * stop() disarms the recorder, flushes any in-progress recording,
 * and stops the uploader. Idempotent on each side.
 */
esp_err_t wot_logger_start(void);
esp_err_t wot_logger_stop(void);

/*
 * True if the WOT logger has been started and not yet stopped.
 * Used by feature_manager during preempt-swap to know when stop()
 * has fully completed.
 */
bool      wot_logger_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* WOT_LOGGER_H */
