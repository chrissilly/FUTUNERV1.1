#ifndef FUTUNER_CONFIG_H
#define FUTUNER_CONFIG_H

#define FUTUNER_VERSION_MAJOR 2
#define FUTUNER_VERSION_MINOR 0
#define FUTUNER_VERSION_PATCH 0

#define FUTUNER_VERSION_STRING "2.0.0-dev"

#define FUTUNER_NVS_NAMESPACE "futuner"

#define VIN_MAX_LENGTH 17
#define SOFTWARE_VERSION_MAX_LENGTH 32
#define HARDWARE_VERSION_MAX_LENGTH 32
#define BUILD_ID_MAX_LENGTH 64

/*
 * Phase 2 (full binary flash) build gate. Off by default in customer
 * firmware — the per-section flash orchestrator, the AES wire path,
 * and any code that references them must be guarded by:
 *
 *     #if FUTUNER_PHASE2_ENABLED
 *
 * Override at build time with `idf.py build -DFUTUNER_PHASE2_ENABLED=1`
 * (or edit this header for a one-off bench build). Tracked alongside
 * P-08 in docs/PHASE_2_PREREQUISITES.md.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#ifndef FUTUNER_PHASE2_ENABLED
/* Phase 1 customer-firmware build — default 0 per the file's own comment
 * above. Phase 2 bench builds should pass `idf.py build
 * -DFUTUNER_PHASE2_ENABLED=1` explicitly rather than relying on this
 * default. Restored 2026-05-17 (PC). */
#define FUTUNER_PHASE2_ENABLED 0
#endif

/*
 * Phase 3 (live tuning ecosystem) build gate. Off by default in
 * customer firmware — the SBF/FBF apply path, the ethanol constraint
 * engine, the rev-limiter RAM toggle, the 9 map-switch UI slots, the
 * pre-apply safety gate, and any code that references them must be
 * guarded by:
 *
 *     #if FUTUNER_PHASE3_ENABLED
 *
 * Override at build time with `idf.py build -DFUTUNER_PHASE3_ENABLED=1`
 * (or edit this header for a one-off bench build). Tracked alongside
 * P3-11 in docs/PHASE_3_PREREQUISITES.md.
 *
 * Phase 2 (destructive 8 MB binary flash) and Phase 3 (non-destructive
 * RAM-write live tuning) are independent gates. Both default off in
 * customer firmware. Either can be turned on without the other (Phase
 * 2 enables the destructive flash path; Phase 3 enables the live-tune
 * surface). Phase 1 (logging, DTC, VIN pair) is always-on and is not
 * gated by either flag.
 *
 * Default 0 per Phase 3 silo directive 2026-05-22. Flipping is a
 * separate owner-signed action.
 */
#ifndef FUTUNER_PHASE3_ENABLED
#define FUTUNER_PHASE3_ENABLED 0
#endif

#endif // FUTUNER_CONFIG_H
