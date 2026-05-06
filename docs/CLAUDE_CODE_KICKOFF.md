# Claude Code — kickoff prompts

> Two prompts. Run them in order. Each is scoped tight enough that Claude Code can finish, test, and stop without scope creep.

---

## Prompt 1 — Build the feature manager (state arbiter)

> **Why this is first:** The project rules require that every user-visible feature default to OFF, run only on explicit start, and stop cleanly before another starts. That arbiter does not exist yet. Every later feature plugs into it, so it has to land first. Until then, "ON/OFF discipline" is a comment, not enforcement.

**Paste this into Claude Code:**

```
Read CLAUDE.md and docs/MISSION_SPEC.md sections on feature ON/OFF
discipline before starting. Then read firmware/src/commands/command_handler.h
and firmware/src/commands/command_handler.c to understand the existing
command dispatch layer. The feature manager sits ABOVE command_handler:
when a "start_logging" command arrives, command_handler asks
feature_manager whether it can start; feature_manager arbitrates.

Build a new module at firmware/src/feature_manager/
  - feature_manager.h
  - feature_manager.c
  - CMakeLists.txt (registers the component to the build)

Required API (propose adjustments if the shape is wrong, but do not silently
deviate):

    typedef enum {
        FEATURE_NONE = 0,
        FEATURE_WOT_LOGGING,
        FEATURE_LIVE_TUNE,
        FEATURE_PHASE2_FLASH,
        FEATURE_DTC_CLEAR,
        FEATURE_BLE_PAIRING,
        FEATURE_VIN_PAIRING,
        FEATURE_COUNT
    } feature_id_t;

    typedef struct {
        feature_id_t id;
        const char  *name;
        esp_err_t  (*start)(void);
        esp_err_t  (*stop)(void);
        bool       (*is_running)(void);
    } feature_descriptor_t;

    esp_err_t feature_manager_init(void);
    esp_err_t feature_manager_register(const feature_descriptor_t *desc);
    esp_err_t feature_manager_request_start(feature_id_t id, char *err_out, size_t err_len);
    esp_err_t feature_manager_request_stop(feature_id_t id);
    feature_id_t feature_manager_active(void);
    const char *feature_manager_active_name(void);

Behavior requirements:

1. At boot, no feature is active. feature_manager_active() returns FEATURE_NONE.
2. feature_manager_request_start(X):
   a. If active == FEATURE_NONE: call X.start(); on success, set active = X.
   b. If active == X: return ESP_OK (idempotent).
   c. If active == Y, Y != X: log a warning, call Y.stop(), wait until
      Y.is_running() returns false (with a configurable timeout — read
      from config, no integer literal here), then call X.start().
      If Y.stop() fails or times out, do NOT start X; return error and
      populate err_out.
3. feature_manager_request_stop(X):
   a. If active == X: call X.stop(); on success, set active = FEATURE_NONE.
   b. Else: return ESP_OK (no-op).
4. All transitions are protected by a mutex. No two threads can race a
   start/stop swap.
5. Every state transition emits a log line (ESP_LOGI) with a stable tag
   so the dev car serial log shows the arbitration history clearly.

Acceptance criteria — claude code MUST verify these before declaring done:

- The module compiles cleanly under the existing firmware build
  (`cd firmware && ./build.sh` returns 0).
- A unit test at firmware/test/test_feature_manager.c covers:
  * Register a feature, start it, verify active.
  * Request stop, verify inactive.
  * Register two features. Start A. Request start B. Verify A.stop()
    was called, then B.start(), then active == B.
  * Idempotent: request start of already-active feature returns OK
    without re-calling start().
  * Force a feature whose stop() returns error. Request another
    feature. Verify the swap fails and active stays on the failed-stop
    feature.
- No magic numbers. Timeouts come from a config header
  firmware/src/config/feature_manager_config.h with sensible defaults
  (propose values, mark them clearly as needing approval).

Do NOT in this task:
- Modify command_handler.c yet. (Wiring commands to the feature manager
  is Prompt 3.)
- Touch any existing feature module (logger, scal, flash, etc.).
- Refactor the misnamed firmware/src/state_machine/ directory. Note in
  the PR description that it should be renamed (e.g., to "uds_session/")
  but do not do it now.

When done:
- Update status-YYYY-MM-DD.md at workspace root with what was built.
- Update file-update-YYYY-MM-DD.md with each file written/edited and why.
- Print a one-paragraph summary of behavior + the proposed default values
  in feature_manager_config.h that need Sean's approval.
```

---

## Prompt 2 — Wire WOT logger as the first feature on the manager

> **Why this is second:** Smallest end-to-end feature you can ship that proves the feature manager works. Gauges already stream at 12.4 Hz on the dev car. The missing pieces are the throttle trigger, the 60 s hard cap, gzip + queue, and the upload-then-delete loop. Done correctly, this is the template every future feature follows.

**Paste this into Claude Code AFTER Prompt 1 is merged:**

```
Read CLAUDE.md, docs/MISSION_SPEC.md §4.3, and the just-merged
firmware/src/feature_manager/. Read firmware/src/logger/wot_logger.{c,h}
to see what's already there.

Goal: make WOT logging a first-class feature on the feature manager,
end-to-end on the dev car.

Tasks:
1. Adapt firmware/src/logger/wot_logger.c to expose start/stop/is_running
   and register itself as FEATURE_WOT_LOGGING with feature_manager
   during logger init. No work happens unless start() is called.
2. Implement the WOT trigger: when start() is active AND throttle position
   crosses a configurable threshold (read from config, no magic numbers),
   begin recording. Stop recording at 60s hard cap (configurable) OR when
   throttle returns below threshold for a configurable cooldown OR when
   stop() is called.
3. Compress finished log with gzip in-memory. Target ~3-4 KB per log per
   spec.
4. Queue logs on flash (existing filesystem module). On Wi-Fi available,
   POST to /api/v1/telemetry/log. On 200 response, delete local copy.
   On non-2xx, retain for retry.
5. Wire the WS/serial commands "wot_log_start" and "wot_log_stop" to
   feature_manager_request_start(FEATURE_WOT_LOGGING) and ..._stop(...).
   This is the FIRST place command_handler talks to feature_manager.
   Use it as the template for every future feature wiring.

Acceptance criteria — claude code MUST verify these before declaring done:

- Build compiles clean.
- Unit test at firmware/test/test_wot_logger.c covers:
  * start when no other feature active → WOT logger active
  * start when another feature active → arbitrated correctly via
    feature_manager (use a mock feature)
  * trigger crosses threshold → recording begins
  * 60s elapsed → recording auto-ends, log queued
  * upload success → local copy deleted
  * upload 5xx → local copy retained, retry on next attempt
- Manual test note in PR description: how to verify on the dev car —
  exact serial commands to send, expected websocket events, expected
  log output.

Do NOT:
- Add any other feature wiring. WOT logger only.
- Touch live tune, flash, or DTC paths.
- Change the gauge streaming code (it already works at 12.4 Hz).

When done: update status + file-update logs at workspace root with what
was built, what defaults were chosen, and what needs Sean's approval.
```

---

## What comes after these two

Once Prompts 1 and 2 are merged and verified on the dev car, the rest of the Phase 1 features each follow the same pattern (one Claude Code prompt per feature, scope ≈ Prompt 2):

- **Prompt 3** — DTC read/clear (stubs already exist in `commands/dtc_commands.c`; smallest second feature).
- **Prompt 4** — VIN pairing flow (Wi-Fi AP→STA, VIN read via UDS PID 0x0902, license token fetch + cache).
- **Prompt 5** — SBF live tune orchestrator (the commercial headline; biggest single feature).
- **Prompt 6** — BLE ethanol sensor bridge.
- **Prompt 7** — Ethanol constraint + rev-limiter window logic.
- **Prompt 8** — Transport abstraction layer (CAN ↔ Ethernet behind one interface).

Each prompt should follow the structure of Prompt 2: read CLAUDE.md and the relevant MISSION_SPEC section, register with feature_manager, expose start/stop/is_running, define acceptance criteria, list explicit non-goals, finish with status + file-update log entries.

Phase 2 (full binary flash) gets its own dedicated prompt sequence after Phase 1 is fully verified on the dev car. That work is separate because the failure mode is "brick the customer's ECU" and warrants its own checklist, real-bench validation, and pre-flash gate review before any code lands in the main branch.
