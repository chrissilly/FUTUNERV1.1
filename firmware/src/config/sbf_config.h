#ifndef SBF_CONFIG_H
#define SBF_CONFIG_H

/*
 * sbf_config.h — central tunables for the SBF live-tune orchestrator.
 *
 * Per FUTV1.1/CLAUDE.md "no magic numbers" rule, every numeric and
 * string constant the sbf_orchestrator / sbf_loader / sbf_applier /
 * sbf_downloader / sbf_commands modules use lives here.
 *
 * All defaults below are PROPOSED and need approval from Sean before lock.
 */

/* ------------------------------------------------------------------ */
/* Filesystem layout                                                    */
/* ------------------------------------------------------------------ */

/*
 * Directory on the storage partition where downloaded SBF files are
 * cached. Files inside are named "stage<N>.sbf" where N is the
 * customer-requested stage 1/2/3 per MISSION_SPEC §4.2.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_CACHE_DIR_PATH                  "/storage/sbf"

/*
 * Filename template for cached stage SBFs. Used with snprintf to
 * build "/storage/sbf/stage1.sbf" etc.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_CACHE_FILENAME_TEMPLATE         "%s/stage%u.sbf"

/*
 * Maximum length of a built path string (cache dir + filename).
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_PATH_MAX                        128

/*
 * mkdir() mode bits used to create the cache directory on first
 * write. Mirrors WOT_QUEUE_DIR_MODE.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_CACHE_DIR_MODE                  0777

/* ------------------------------------------------------------------ */
/* Cloud endpoint                                                      */
/* ------------------------------------------------------------------ */

/*
 * Path component appended to the configured cloud host when fetching
 * the assigned SBF. Per the existing cloud server in cloud/src/main.py:
 *   GET /api/v1/device/calibration   →   streams the binary SBF
 *
 * v1 uses the existing endpoint as-is; multi-stage rotation is a
 * follow-on prompt.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_DOWNLOAD_PATH                   "/api/v1/device/calibration"

/*
 * Default cloud host. Reuses the same default as license/wot_uploader.
 * Runtime override via the same NVS key license_config.h declares
 * (LICENSE_NVS_CLOUD_HOST_KEY); we don't redefine the key here to
 * avoid drift.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_DEFAULT_HOST                    "https://api.sillyrabbitmotorsport.com"

/*
 * HTTP request timeout (ms) for the SBF download. Larger than the
 * license/WOT timeouts because an SBF may be 30–100 KB; 30 s covers
 * a slow cellular hotspot end-to-end.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_DOWNLOAD_HTTP_TIMEOUT_MS        30000

/*
 * HTTP status code lower/upper bounds that count as success.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_HTTP_OK_MIN                     200
#define SBF_HTTP_OK_MAX                     299

/*
 * Maximum SBF payload accepted from the cloud. SBFs from the existing
 * sbf/ samples are 30–40 KB; 256 KB is comfortable headroom.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_DOWNLOAD_BUF_MAX                262144

/*
 * Maximum length of a built download URL (host + path).
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_URL_MAX                         256

/* ------------------------------------------------------------------ */
/* Apply timing budgets                                                 */
/* ------------------------------------------------------------------ */

/*
 * Hard cap on a single apply pass duration (ms). Per MISSION_SPEC
 * §4.2 the live-tune update should complete in 1.5–2 s. The
 * orchestrator emits apply_failed if elapsed time exceeds this.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_APPLY_HARD_CAP_MS               2500

/*
 * Per-write timeout (ms). Each ecu_write_data call is async; the
 * worker waits this long for the callback before declaring the write
 * stuck. With ~50 byte writes at 500 kbps + ECU response time, 250 ms
 * is generous.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_PER_WRITE_TIMEOUT_MS            250

/*
 * Progress events are emitted every N maps processed so the WS UI
 * doesn't get spammed with one event per cell. 10 is roughly two
 * progress ticks across a typical 21-map stage 1 SBF.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_PROGRESS_EVENT_EVERY_N_MAPS     10

/* ------------------------------------------------------------------ */
/* Worker queue                                                         */
/* ------------------------------------------------------------------ */

/*
 * Maximum apply requests buffered in the orchestrator's worker queue.
 * Set/reapply requests collapse onto the latest entry — the queue
 * never grows beyond this depth. 4 covers UI-driven rapid-fire stage/
 * ethanol changes without dropping the most recent request.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_WORKER_QUEUE_DEPTH              4

/*
 * Worker task stack size (bytes). Apply loop uses a few KB of stack
 * for blend_engine scratch buffers + ecu_write call frames.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_WORKER_TASK_STACK_BYTES         8192

/*
 * Worker task FreeRTOS priority. Higher than gauge logger (5) so an
 * apply preempts a polling cycle, lower than CAN driver (10) so the
 * driver stays responsive.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_WORKER_TASK_PRIORITY            6

/*
 * Worker task pinned core. Pinning to core 0 keeps it off the WiFi/
 * LWIP-heavy core 1.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_WORKER_TASK_CORE                0

/* ------------------------------------------------------------------ */
/* Runtime parameters                                                   */
/* ------------------------------------------------------------------ */

/*
 * Allowed ethanol percentage range. SBFs encode blends via the SCAL
 * blend_map's 9-point x-axis (0..100% in steps); inputs outside this
 * are clamped or rejected by the orchestrator.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_ETHANOL_MIN_PCT                 0
#define SBF_ETHANOL_MAX_PCT                 100

/*
 * Allowed stage range. SBF Builder produces stage 1 / 2 / 3 per
 * MISSION_SPEC §4.2.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_STAGE_MIN                       1
#define SBF_STAGE_MAX                       3

/* ------------------------------------------------------------------ */
/* Buffer sizing for handlers / err strings                             */
/* ------------------------------------------------------------------ */

/*
 * Maximum length of an err_out / reason buffer used by the
 * orchestrator's public functions when surfacing failures.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_ERR_BUF_MAX                     192

/*
 * Maximum length of a WebSocket event JSON payload emitted by the
 * worker.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_EVENT_JSON_MAX                  256

/*
 * Maximum length of the active SBF filename string returned in
 * status snapshots.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_FILENAME_MAX                    64

/*
 * Maximum length of the orchestrator's last-error scratch + the
 * applier's failure_reason. 128 covers the typical "rc=NN at map
 * MMM" string with comfortable headroom.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_LAST_ERROR_MAX                  128

/* ------------------------------------------------------------------ */
/* Blend map shape (SCAL flex map convention)                           */
/* ------------------------------------------------------------------ */

/*
 * Number of points on the blend map's x-axis (ethanol % grid).
 * Per SCAL format: 9 uint16 values in [0, 65535] mapped to
 * percentages.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_BLEND_MAP_POINTS                9

/*
 * Resolution of the blend map's z-axis interpolation factors. Each
 * factor is a 16-bit value (Q0.16) scaled so 0 = pure gasoline and
 * 65535 = pure ethanol.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_BLEND_FACTOR_FULL_SCALE         65535

/* ------------------------------------------------------------------ */
/* Variant lookup table                                                 */
/* ------------------------------------------------------------------ */

/*
 * Maximum number of boxcodes in the sbf_variants lookup table.
 * Today we seed with the dev car's 4K0907557G__0003. Expand as new
 * boxcodes onboard.
 *
 * TODO: Phase A migrates this lookup into the per-variant manifest
 * defined in docs/SCALE_ARCHITECTURE_PROPOSAL.md §2.2 (specifically
 * memory_map.write_mid_byte and memory_map.write_offset). Tracked
 * as P-11 in docs/PHASE_2_PREREQUISITES.md. Until then, dev-car
 * operator discipline is the only safeguard against a stale row.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define SBF_VARIANTS_TABLE_MAX              16

#endif /* SBF_CONFIG_H */
