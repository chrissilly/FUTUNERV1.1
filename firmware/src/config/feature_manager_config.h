#ifndef FEATURE_MANAGER_CONFIG_H
#define FEATURE_MANAGER_CONFIG_H

/*
 * feature_manager_config.h — central tunables for the feature manager.
 *
 * Per FUTV1.1/CLAUDE.md "no magic numbers" rule, every numeric constant the
 * feature manager uses lives in this header. feature_manager.c MUST NOT
 * contain integer literals (other than 0/1) for behavioral values.
 *
 * All defaults below are PROPOSED and need approval from Sean before lock.
 */

/*
 * Maximum time the manager will wait for a preempted feature's is_running()
 * to return false after stop() is called. Sized to match MISSION_SPEC §4.2's
 * 1.5–2 s live-tune update budget; a feature that takes longer to wind down
 * is signaling it cannot be preempted cleanly.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define FEATURE_MGR_STOP_TIMEOUT_MS        2000

/*
 * Polling interval for is_running() inside the stop-and-wait loop.
 * 25 ms × 80 polls covers the 2 s budget without burning CPU.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define FEATURE_MGR_STOP_POLL_INTERVAL_MS  25

/*
 * Maximum time any caller will block waiting for the manager mutex.
 * Bound prevents an unrelated stuck task from freezing every UI command;
 * a longer wait than this is a deadlock smell that should be logged loudly.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define FEATURE_MGR_MUTEX_TIMEOUT_MS       1000

/*
 * Static registry size. Sized off FEATURE_COUNT so registration can never
 * outrun the enum space; updating the enum is the only way to add a slot.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define FEATURE_MGR_MAX_REGISTERED         FEATURE_COUNT

#endif /* FEATURE_MANAGER_CONFIG_H */
