# Claude Code — Prompt 5 (SBF Live Tune Orchestrator)

> Paste-ready prompt for the firmware Claude Code session. The agent
> should run this from a clean `feat/prompt-5-sbf-live-tune` branch.
>
> This is the highest-stakes prompt yet because of the frozen-module
> rule. Any modification to scal/bdef/ecu_write fails the eval and
> cannot ship. The orchestrator is **new code that calls the frozen
> modules' public API** — it does not modify them, even by a comment.

---

## Paste this into Claude Code

```
Prompt 5 — SBF live tune orchestrator. New code that calls into the
FROZEN scal/bdef/ecu_write modules and DOES NOT modify them.

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md
- ~/esp/obd/FUTV1.1/docs/MISSION_SPEC.md §4.2
- ~/esp/obd/FUTV1.1/docs/SCALE_ARCHITECTURE_PROPOSAL.md §5
- ~/esp/obd/FUTV1.1/firmware/src/FROZEN_MODULES.md             (BINDING)
- ~/esp/obd/FUTV1.1/firmware/src/scal/scal_file.h              (PUBLIC API ONLY)
- ~/esp/obd/FUTV1.1/firmware/src/bdef/bdef_file.h              (PUBLIC API ONLY)
- ~/esp/obd/FUTV1.1/firmware/src/ecu_write/ecu_write.h         (PUBLIC API ONLY)
- ~/esp/obd/FUTV1.1/firmware/src/feature_manager/feature_manager.h
- ~/esp/obd/FUTV1.1/firmware/src/license/license.h             (Prompt 4)
- ~/esp/obd/FUTV1.1/firmware/src/state_machine/connection_manager.h  (for get_vin)
- ~/esp/obd/FUTV1.1/firmware/test/wot_logger/eval.sh           (template)
- ~/esp/obd/FUTV1.1/sbf/stage1_patched.sbf.json                (real SBF example)

ABSOLUTE rule: scal_file.c, scal_file.h, bdef_file.c, bdef_file.h,
ecu_write.c, ecu_write.h are FROZEN per FROZEN_MODULES.md. You read
their .h files for the API. You DO NOT open, edit, or even look at
their .c files. You DO NOT add #defines that "would clean up" their
internals. You wrap them. firmware/test/verify_frozen.sh runs as
part of your eval and any modification fails the harness.

Pre-decided choices (answering the eight ambiguities flagged in your
pre-flight survey):

Q1 — MODULE SHAPE: Five new files under firmware/src/sbf/. The
orchestrator wraps the frozen modules; it does not reimplement
parsing.
  - sbf_orchestrator.{c,h}   (~300 lines)
      Feature lifecycle, register w/ feature_manager, state machine,
      worker task, queue. The "owner" of the live-tune state.
  - sbf_loader.{c,h}         (~250 lines)
      Reads SBF JSON from /storage/sbf/, hands a parsed struct to the
      applier. Internally calls scal_file_parse() / bdef_file_parse()
      from the frozen modules.
  - sbf_applier.{c,h}        (~250 lines)
      Selects stage + ethanol variant from a loaded SBF, calls
      scal_file_apply() / bdef_file_apply() / ecu_write_*() to write
      to ECU RAM. Measures and logs apply time.
  - sbf_downloader.{c,h}     (~200 lines)
      Pulls SBF from cloud /api/v1/device/calibration (existing
      endpoint) using the license-module auth token. Tick-driven
      retry, same shape as wot_uploader.
  - sbf_commands.{c,h}       (~150 lines)
      WS/serial command handlers: live_tune_start, live_tune_set,
      live_tune_stop, live_tune_status.

Q2 — LICENSE GATE WIRING: bundled. This prompt wires
license_can_run_feature() into TWO places:
  - sbf_orchestrator.start() — refuses with ESP_ERR_INVALID_STATE if
    license_can_run_feature(FEATURE_LIVE_TUNE, ecu_vin) returns false.
  - wot_uploader.c — refuses to upload if
    license_can_run_feature(FEATURE_WOT_LOGGING, ecu_vin) is false
    (the deferred-from-Prompt-4 follow-up). One-line addition; this
    promotes the license gate from dead code to load-bearing for both
    features in a single coherent commit.

Q3 — ETHANOL INTERACTION (scope boundary):
  - Manual ethanol input via WS command set_ethanol_percent — IN scope.
  - BLE ethanol sensor input — DEFERRED to Prompt 6.
  - Ethanol hysteresis / dwell / WOT lockout / rev-limit window — ALL
    DEFERRED to Prompt 7. Do NOT implement any of §4.5's safety logic
    in this prompt. Live-tune apply during driving is on the operator
    in v1; Prompt 7 adds the rails.

Q4 — APPLY TIMING: task-driven, not synchronous. start() pushes an
apply request to a worker queue and returns immediately. The worker
task drains the queue, calls into the applier, emits progress events
via WebSocket. apply_completed event when done with elapsed_ms in
the payload. This way the WS thread is never blocked for 1.5–2 s.
Use the test-controllable clock pattern from wot_recorder so the
host test can fast-forward.

Q5 — STAGE SWITCHING API: single live_tune_set(stage, ethanol_pct).
  - live_tune_start({stage, ethanol_pct}) → enters active state,
    triggers initial apply.
  - live_tune_set({stage, ethanol_pct}) → only valid while active;
    queues a re-apply with new params.
  - live_tune_stop() → drains queue, releases SBF, exits active.
  - live_tune_status() → returns {active, current_stage, current_eth,
    last_apply_ms, last_apply_elapsed_ms, sbf_filename}.

Q6 — PHASE 2 BASE BINARY CHECK: DEFERRED. v1 relies on the license
gate alone. The orchestrator does NOT verify the ECU has the live-tune
patches. Add a TODO comment in sbf_applier.c with a clear note:

    /* TODO Phase 2 sentinel check — verify the ECU's currently-running
     * binary contains the live-tune patches before applying. Sentinel
     * address/value is per-variant and lives in the variant manifest.
     * Until manifest schema lands, dev-car operator discipline is the
     * only safeguard. Track as Phase 2 prerequisite P-XX in
     * docs/PHASE_2_PREREQUISITES.md.
     */

Add a corresponding entry to docs/PHASE_2_PREREQUISITES.md as a new
P-NN item titled "ECU sentinel check before live-tune apply" so the
TODO doesn't get lost.

Q7 — REQUIRED TEST SCENARIOS (must literally appear in
test_sbf_orchestrator.c so the eval scenario-grep matches):
  - test_start_refuses_when_unpaid       — license_can_run_feature
                                            returns false; start fails
                                            with ESP_ERR_INVALID_STATE.
  - test_start_paid_applies_in_budget    — paid + SBF on disk;
                                            elapsed_ms < 2000 via
                                            fast-forwardable clock.
  - test_set_ethanol_triggers_reapply    — while active, change
                                            ethanol; re-apply runs.
  - test_set_stage_triggers_reapply      — while active, change stage;
                                            re-apply runs.
  - test_malformed_sbf_returns_idle      — loader error; state goes
                                            back to IDLE.
  - test_swap_from_dtc                   — DTC active, request
                                            live_tune start;
                                            arbitration via
                                            feature_manager works.
  - test_apply_progress_events           — WS observer sees
                                            apply_started + apply_progress
                                            + apply_completed events
                                            with elapsed_ms.
  - test_unload_drains_queue             — pending re-apply queued;
                                            stop() drains without
                                            executing.

Q8 — FORBIDDEN LIST FOR THIS PROMPT:
  - Any modification to firmware/src/scal/, firmware/src/bdef/,
    firmware/src/ecu_write/ — bytes must match frozen_modules.sha256.
  - Any modification to firmware/src/feature_manager/, /dtc/, /vin_pairing/,
    /license/ — except the single-line license_can_run_feature gate
    addition to wot_uploader.c per Q2.
  - Any modification to wot_logger.c or wot_recorder.c — only
    wot_uploader.c gets the gate-wiring one-liner.
  - Cloud server modifications — none. The download path uses the
    existing /api/v1/device/calibration endpoint as-is.
  - Implementing any of MISSION_SPEC §4.5's safety logic.
  - Implementing the BLE ethanol bridge (Prompt 6).
  - Implementing variant manifest schema validation beyond a stub.

Mocking pattern (host tests):
The frozen modules' real .c files MUST NOT be linked into the host
test runner. Mock them at the public API boundary using function
pointers injected into sbf_loader / sbf_applier. The orchestrator's
own logic (state machine, worker task, gate check, queue) gets real
exercise; the parsing and writing get mocked. This is the same
pattern wot_logger used for HTTP / FS / wifi.

Mock list:
  - Real feature_manager (registered descriptors).
  - Real license module BUT with a test-controllable license state
    (paid/unpaid/revoked toggleable per test).
  - Mock scal_file_parse / scal_file_apply (function-pointer-injected).
  - Mock bdef_file_parse / bdef_file_apply (function-pointer-injected).
  - Mock ecu_write_* (function-pointer-injected).
  - Mock HTTP client for downloader tests (200 vs non-200).
  - Mock filesystem (in-memory, same shape as wot_uploader's mock).
  - Mock VIN source (same shape as Prompt 4).
  - Test-controllable clock (the wot_recorder pattern, reusable).
  - WebSocket event sink mock (capture emitted events for the
    progress-event test).

Wiring (small, focused changes outside the new sbf/ directory):
  - main.c: call sbf_orchestrator_init() during boot, after
    license_init() and dtc_feature_init().
  - firmware/src/commands/commands.c: add live_tune_start,
    live_tune_set, live_tune_stop, live_tune_status entries to
    COMMAND_REGISTRY pointing at sbf_commands handlers.
  - firmware/src/CMakeLists.txt: add the five sbf/ sources + the new
    sbf_commands.c.
  - firmware/src/logger/wot_uploader.c: add the
    license_can_run_feature(FEATURE_WOT_LOGGING, ecu_vin) gate at
    the start of the upload path. Q2 follow-up.

Acceptance criteria — verify ALL before declaring done:
  - firmware/test/sbf/eval.sh exits 0 (full eval, no SKIP_IDF_BUILD).
  - firmware/test/verify_frozen.sh exits 0 (six frozen files unchanged).
  - ./build.sh exits 0.
  - All FIVE prior eval gates still PASS regression-clean
    (vin_pairing, dtc, wot_logger, feature_manager, can_capture).
  - All eight required test scenarios literally present in
    test_sbf_orchestrator.c.
  - cloud/tests passes (no cloud changes, but regression check).

Hard rules carry through (CLAUDE.md):
  - CAN ID 0x7E0 only. Standard UDS services only.
  - No magic numbers. All constants in sbf_config.h with the
    "Proposed default — needs Sean's approval before lock" annotation.
  - ON/OFF discipline through feature_manager.
  - Mandatory: append to ~/esp/obd/status-2026-05-05.md and
    ~/esp/obd/file-update-2026-05-05.md.

When done:
1. cd ~/esp/obd/FUTV1.1
2. firmware/test/sbf/eval.sh
3. If RESULT: PASS — re-run the four other prior gates, confirm
   regression-clean, print full eval output and the proposed defaults
   table from sbf_config.h. Hand back.
4. If RESULT: FAIL — fix and re-run. Do not declare done with any
   FAIL.

Ask clarifying questions BEFORE writing code if anything in the
mocking pattern, the worker task design, the queue semantics, or the
license-gate-wiring scope is unclear. The frozen-module boundary is
the most error-prone part of this prompt — if your design finds itself
wanting to "improve" anything inside scal/bdef/ecu_write, stop and
ask. The wrapping is correct; modifying is regression risk.

Proceed.
```

---

## Notes for Sean

Two things in this prompt are scope expansions beyond my original draft:

The **bundled wot_uploader gate-wiring** (Q2 answered as `(a)`) is the deferred 1-line follow-up from Prompt 4 promoted to in-scope here. The agent's reasoning was correct: the license module is touched by this prompt anyway, the gate is one line, and bundling it means the license_can_run_feature() function gets two real callers in one commit instead of staying dead code waiting for a separate trivial commit.

The **Phase 2 sentinel check** is explicitly deferred and tracked as a future Phase 2 prerequisite. The risk is that a paid customer with a stock-flashed ECU could attempt a live-tune apply, get "success", and see no behavior change. For dev-car-only testing this is fine; for customer rollout it becomes a P-XX prerequisite alongside Phase 2 itself.

The agent's eight pre-flight questions were all good. Q4 in particular (synchronous vs task-driven apply) is a real design call — the answer (task-driven worker) keeps the WS thread responsive during the 1.5–2 s apply window, which matters because the customer's UI will show "applying..." and they'll hit the stop button if it freezes.
