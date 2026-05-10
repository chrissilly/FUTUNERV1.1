# Claude Code — Prompt 9 (UI catchup sweep)

> Paste-ready prompt for the firmware Claude Code session. The agent
> should run this from a clean `feat/prompt-9-ui-catchup` branch.
>
> This prompt has zero firmware logic changes. Every WS command being
> wired already exists; the dongle is already broadcasting events the
> UI silently drops today (real bug — `handleMsg` only handles
> `event === 'can_frame'`, the SBF orchestrator's
> `apply_started`/`apply_progress`/`apply_completed` payloads land
> nowhere). The work is restructuring the existing single-page UI,
> exposing the Prompt 1–5 command surface as user-friendly controls,
> adding an event bus + tiny shared store, and standing up the first
> UI eval gate.
>
> The frozen-module rule still applies (reflexive at this point), and
> `firmware/test/verify_frozen.sh` still runs as part of the eval —
> not because UI work could touch a frozen `.c` file, but because
> every prompt's eval re-checks the cross-prompt regression baseline.

---

## Paste this into Claude Code

```
Prompt 9 — UI catchup sweep. Restructure the single-page UI, expose
the Prompt 1–5 WS command surface as user-friendly controls, add an
event-bus shim + tiny shared store, and stand up the first UI eval
gate. ZERO firmware logic changes — UI consumes the existing command
registry, nothing else.

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md
- ~/esp/obd/FUTV1.1/docs/SESSION_HANDOFF.md
- ~/esp/obd/FUTV1.1/docs/MISSION_SPEC.md §1.3 (UI architecture)
- ~/esp/obd/FUTV1.1/firmware/futuner_control_panel.html  (BYTE-IDENTICAL to ui/control_panel.html — collapse to one source per Q1)
- ~/esp/obd/FUTV1.1/ui/control_panel.html                (canonical going forward)
- ~/esp/obd/FUTV1.1/firmware/src/commands/commands.c     (COMMAND_REGISTRY — the surface to bind against)
- ~/esp/obd/FUTV1.1/firmware/src/commands/wot_log_commands.c
- ~/esp/obd/FUTV1.1/firmware/src/commands/dtc_commands.c
- ~/esp/obd/FUTV1.1/firmware/src/commands/vin_pair_commands.c
- ~/esp/obd/FUTV1.1/firmware/src/commands/sbf_commands.c
- ~/esp/obd/FUTV1.1/firmware/src/sbf/sbf_orchestrator.c   (event payload shapes — apply_started/apply_progress/apply_completed/apply_failed/unload)
- ~/esp/obd/FUTV1.1/firmware/src/feature_manager/feature_manager.h
- ~/esp/obd/FUTV1.1/firmware/test/sbf/eval.sh             (template for grading harness; UI eval mirrors the structure)
- ~/esp/obd/FUTV1.1/ui/_reference/SCORPION_BUNDLE_NOTES.md (visual reference only — do NOT migrate themes)

ABSOLUTE rules carry through:
- No edits to firmware/src/scal/, firmware/src/bdef/, firmware/src/ecu_write/.
  verify_frozen.sh runs from inside the new ui eval; modifying a frozen
  file fails the harness.
- No firmware C logic changes. UI ONLY consumes the existing command
  registry. If you find yourself reaching for a new firmware command
  to make a UI button work, stop and surface it as a follow-up — this
  prompt does not modify firmware/src/.
- No cloud changes.
- CAN ID 0x7E0 only (not relevant for UI work but stated for safety).
- No magic numbers. CSS values that aren't already CSS variables go
  into the existing :root token block, named, with a comment. JS
  numeric constants (timeouts, intervals, slot counts) go into a
  named const block at the top of ui/control_panel.js with a comment
  per constant — same "Proposed default — needs Sean's approval
  before lock" annotation pattern as the firmware config headers.

Pre-decided choices (answering the six open questions in the skeleton
plus a sub-clarification on event-direction testing):

Q1 — UI SOURCE OF TRUTH: single canonical file at ui/control_panel.html.
  - Today firmware/futuner_control_panel.html and ui/control_panel.html
    are byte-identical (1596 lines, ~71 KB). That duplication is a
    drift hazard waiting to happen. Eliminate it.
  - Canonicalize at ui/. Split CSS and JS out of the inline blocks
    into ui/control_panel.css and ui/control_panel.js.
  - Add tools/bundle_ui.py: a tiny script (~80 lines) that concatenates
    ui/control_panel.{html,css,js} into a single self-contained
    firmware/futuner_control_panel.html (CSS inlined into <style>, JS
    inlined into <script>, no external refs). The firmware partition
    needs the single-file form because there is no separate static
    asset server on the dongle.
  - Wire firmware/build.sh to invoke tools/bundle_ui.py before idf.py
    build. Bundle script is idempotent and fast (<1 s).

Q2 — WS RUNTIME: vanilla JS + a ~30-line event-bus shim.
  - No npm. No build step beyond the bundle script. The embedded
    binary stays small.
  - Add a wsEvents bus to ui/control_panel.js:
        const wsEvents = (() => {
          const subs = {}; // event_name -> [handler, ...]
          return {
            on(name, fn)  { (subs[name] ||= []).push(fn); },
            off(name, fn) { subs[name] = (subs[name]||[]).filter(f => f !== fn); },
            emit(name, msg) { (subs[name]||[]).forEach(f => f(msg)); }
          };
        })();
  - Fix the silent-event-drop bug in handleMsg(): after the existing
    `if (msg.event === 'can_frame')` line, route every other
    `msg.event` value through wsEvents.emit(msg.event, msg). The
    can_frame fast-path stays as-is (it's a hot stream).
  - Reconnection logic stays as-is (setTimeout(wsConnect, 3000) on
    onclose). Move the magic 3000 into a named WS_RECONNECT_MS
    constant per the no-magic-numbers rule.

Q3 — STATE MANAGEMENT: hybrid. Per-feature state stays per-feature
  (existing pattern). One tiny shared store ONLY for cross-cutting
  state.
  - appState shape:
        const appState = {
          license: { paid: null, revoked: false, reason: '', vin: '' },
          activeFeature: 'idle'  // mirrors feature_manager_active() name
        };
  - Two consumers only: (a) the persistent header (license lock icon
    + active-feature label), (b) the feature-swap-confirmation modal
    (reads activeFeature when user clicks a button for a different
    feature).
  - Subscribers via tiny `subscribeAppState(fn)` registry.
  - Anything else — WOT queue counts, DTC table rows, SBF stage
    selector — lives in its own panel's local state. Do NOT promote
    those to the store.

Q4 — EVAL SHAPE: three-layer, no headless browser.
  - Layer A — HTML structure check (grep-based, runs in eval.sh):
      * Required top-level panel IDs present (panel-dashboard,
        panel-sniffer, panel-diag, panel-tuning, panel-logconfig,
        panel-files, panel-system, panel-livetune, panel-wot,
        panel-vinpair).
      * Header has license + active-feature indicator nodes
        (#licenseLock, #activeFeatureLabel).
      * Feature-swap modal node present (#swapConfirmModal).
      * For every command in firmware/src/commands/commands.c
        COMMAND_REGISTRY, at least one wsSend() call site referencing
        that command appears in ui/control_panel.js. The eval extracts
        registry names via a one-line awk over commands.c and greps
        ui/control_panel.js. (Excludes legacy CAN sniffer admin-only
        commands by name list — see EVAL_COMMAND_EXEMPTIONS in the
        eval harness.)
      * Every event the firmware can emit (apply_started,
        apply_progress, apply_completed, apply_failed, unload, plus
        future feature events) has a wsEvents.on(...) registration in
        ui/control_panel.js. Same registry-coverage style — but the
        event names come from grepping the firmware src for
        emit_event("{\"event\":\"...\"") literals. The eval extracts
        the set, asserts every member has a JS handler.
  - Layer B — JS syntax check:
      * `node --check ui/control_panel.js` exits 0.
      * `node --check ui/test/mock_dongle.py` is not applicable;
        instead `python3 -m py_compile ui/test/mock_dongle.py` exits 0.
  - Layer C — WS round-trip with fixture-driven event sequencer:
      * ui/test/mock_dongle.py — asyncio + websockets. Loads
        ui/test/mock_dongle_responses.json. For each incoming command,
        looks up the canned response shape and sends it. Also exposes
        a "fire scripted event sequence" admin port:
            POST /admin/fire {"sequence":"apply_progress_3_then_complete"}
        Triggers the loaded sequence (each step is {event, payload,
        delay_ms}).
      * ui/test/mock_dongle_responses.json — fixture file. Required
        keys: every command in the firmware COMMAND_REGISTRY → canned
        success response shape (matching the real handler's response
        contract). Plus a `sequences` block defining named scripted
        event sequences (apply_progress_3_then_complete,
        apply_failed_unload, license_revoked_then_paid, etc.).
        The eval greps that every COMMAND_REGISTRY entry has a fixture
        key, AND that every required scenario sequence is present.
      * ui/test/test_round_trip.py — Python websockets client. For
        every command in the registry: sends, awaits response, asserts
        the response matches the fixture shape (top-level keys + types
        only — value-level matching is too brittle). For every
        required event sequence: requests via the admin port, captures
        emitted events with timestamps, asserts ordering and timing.

Q4 SUB-CLARIFICATION (Sean): event direction MUST be tested. The mock
  dongle's scripted-sequence engine is the load-bearing piece —
  without it, the silent-event-drop bug from Q2 could regress. Every
  feature emitting events ships a fixture sequence that the round-trip
  test exercises.

Q5 — LAYOUT: mobile-first responsive single layout.
  - All new touch targets ≥44×44 px (iOS HIG minimum).
  - Ethanol % uses a slider (`<input type="range">`) NOT a number
    input. Touch-friendly. Number input next to it for precision.
  - Stage selector uses 3 large stage cards (Stage 1/2/3) that are
    tappable, not a `<select>`. Cards expand to full-width on phones,
    inline-flex on desktop.
  - Feature-swap modal is full-screen on phones (<480 px viewport),
    centered card on desktop.
  - One layout. No `if (isMobile) { … }` branches in JS. CSS media
    queries handle everything.
  - Existing tabs, gauges, knock indicators, sniffer, file browser
    keep their current layout — mobile retrofit only the new
    panels and the cross-cutting header.

Q6 — THEME: keep orange/black. NO migration.
  - Current `--accent: #ff6600` stays. Do not change any existing
    color value.
  - Every new color introduced this prompt MUST go through the
    existing CSS variable system. New tokens go into the :root block
    next to the existing ones, named (e.g., --warn-bg, --modal-overlay).
    Inline hex literals in new CSS rules fail eval.
  - Add `--theme-name: 'orange';` to :root as a forward-compatibility
    marker. Future re-skin is a single one-line edit; no UI logic
    depends on the value.
  - The ScorpionEFI reference bundle (ui/_reference/scorpion_compiled/)
    is visual reference for layout ideas only. It is NOT a theme
    target.

Q7 — UI SURFACE TO LAND (per-feature panel breakdown):

  Header (cross-cutting, rendered on every tab):
    - Existing: brand, conn-dot, conn-label, auth.
    - ADD: #licenseLock — shows 🔒 if appState.license.paid !== true,
      🔓 if paid. Hover/tap opens a tooltip with revoke reason and
      cached VIN.
    - ADD: #activeFeatureLabel — text like "Active: live_tune" or
      "Active: idle". Driven by appState.activeFeature, updated by
      polling cmd_get_status (which already returns active-feature
      name) and on every successful command response.

  Dashboard tab — keep existing gauges; ADD a "WOT Logger" card.
    - Status row: queued logs count, total bytes queued, last upload
      result (success/failed/pending), last upload timestamp.
    - Buttons: "Start WOT Logging" → wsSend({command:'wot_log_start'});
      "Stop" → wsSend({command:'wot_log_stop'}); "Force Upload Now"
      → reserved (no firmware command yet — show a tooltip "feature
      coming in a follow-up").
    - License-refusal banner if a start/stop returns success:false
      with a license-related error.

  Diagnostics tab — migrate from legacy get_errors/clear_errors to
  Prompt-3 dtc_read/dtc_clear.
    - Keep the existing "Read DTCs" / "Clear DTCs" buttons and node IDs.
      Rebind them: readDtc() now sends {command:'dtc_read'},
      clearDtc() now sends {command:'dtc_clear'}.
    - DTC table renders: code (P-string), description (from response),
      status flags chip (active/pending/confirmed).
    - Per-row "Clear" button is OUT of scope — Prompt 3's dtc_clear
      is bulk-only. Ship bulk "Clear All" only.
    - Last-cleared timestamp display.
    - Keep the legacy get_errors/clear_errors buttons under a
      separate "System Error Log" card on the System tab — they read
      a different log (firmware error history, not ECU DTCs).

  Live Tune tab — NEW tab, between "XDF Tuning" and "Log Config".
    - Stage selector (3 cards: Stage 1, Stage 2, Stage 3).
    - Ethanol % slider (0–100) + paired number input.
    - "Apply" button → wsSend({command:'live_tune_start',
      params:{stage, ethanol_pct}}).
    - "Update" button (visible only while ACTIVE) →
      wsSend({command:'live_tune_set', params:{...}}).
    - "Stop" button → wsSend({command:'live_tune_stop'}).
    - Progress bar — driven by apply_started → apply_progress
      (maps_done/maps_total) → apply_completed (elapsed_ms shown
      after).
    - State badge: idle / loading / applying / active / error.
    - Refusal banner if response.success === false (license,
      arbitration, or load failure). Banner stays until user clicks
      acknowledge or starts again.
    - Polls live_tune_status every 2 seconds while tab is active.

  System tab — ADD a VIN Pairing + License card.
    - "Pair VIN now" button → wsSend({command:'vin_pair_now'}).
    - "Set Auth Token" — admin-only, wrapped behind the existing
      auth-status === 'authed' gate. Input field + button →
      wsSend({command:'set_auth_token', params:{token}}).
    - License panel: shows paid/unpaid + revoke reason if any. Driven
      by license_status command, polled on tab-active and after every
      vin_pair_now.

  Cross-cutting feature-swap-confirmation modal:
    - Triggered when user clicks a feature-start button while
      appState.activeFeature !== 'idle' AND !== the feature being
      requested.
    - Modal text: "Stop {activeFeature} to start {requestedFeature}?".
    - Buttons: Cancel / Stop and Start.
    - On confirm, send the start command directly (the
      feature_manager arbitrates on the firmware side; we don't issue
      an explicit stop first — preempt-swap is the manager's job).
    - On preempt response, toast "Switched from X to Y".

Q8 — REQUIRED TEST SCENARIOS (must literally appear in
ui/test/test_round_trip.py so the eval scenario-grep matches):
  - test_command_registry_coverage      — every COMMAND_REGISTRY
                                          entry has a fixture entry
                                          AND a wsSend binding in JS.
  - test_event_handler_coverage         — every emit_event(...) string
                                          in firmware/src has a
                                          wsEvents.on(...) binding.
  - test_apply_progress_sequence        — mock fires apply_started →
                                          3× apply_progress →
                                          apply_completed; client
                                          asserts ordered receipt.
  - test_apply_failed_path              — mock fires apply_failed;
                                          client asserts state badge
                                          → error and banner shows.
  - test_license_unpaid_refusal         — mock returns
                                          {success:false,
                                          error:"license unpaid"} for
                                          live_tune_start; client
                                          asserts refusal banner.
  - test_feature_swap_modal_appears     — sequential commands
                                          (wot_log_start then
                                          live_tune_start) with
                                          activeFeature seeded;
                                          modal node becomes visible.
  - test_dtc_read_renders_table         — mock returns DTC array of 3
                                          codes; client asserts table
                                          rows of count 3 with code
                                          + description text.
  - test_html_structure_intact          — required panel IDs present;
                                          JS syntax check passes.
  - test_css_tokens_used                — every new color in
                                          ui/control_panel.css
                                          references a CSS variable;
                                          no inline hex literals
                                          (the eval scans new CSS
                                          rules and excludes the
                                          existing :root block).

Q9 — FORBIDDEN LIST FOR THIS PROMPT:
  - Any modification to firmware/src/* (frozen modules + every other
    feature's .c/.h) — bytes must match prior baseline.
  - Any modification to firmware/src/CMakeLists.txt — UI work doesn't
    add or remove C sources.
  - Any modification to firmware/test/* — five prior eval gates run
    untouched as part of the cross-prompt regression check.
  - Cloud server modifications (cloud/*) — none.
  - Any new firmware WS command. If a button can't be wired against an
    existing command, surface it as a Prompt-9 follow-up; do not add
    new firmware code.
  - Reskinning. The orange/black theme stays. No --accent value
    change, no new font, no new color value outside the existing
    token system.
  - Adding any npm dependency. Vanilla JS only. Python test deps
    (websockets, asyncio) are stdlib + a single pip install
    documented in ui/test/README.md.

Mocking pattern (UI host eval):

The eval runs three layers in sequence:

  Layer A — static checks (bash + grep + awk + node --check +
  python3 -m py_compile). No process spawning beyond the syntax
  checkers. Fast — <2 s.

  Layer B — mock-dongle round-trip:
    1. Eval spawns mock_dongle.py on a high port (default 47821 —
       named in the eval, not a magic number).
    2. Eval spawns test_round_trip.py against ws://127.0.0.1:<port>.
    3. test_round_trip.py exercises every command + every required
       event sequence.
    4. test_round_trip.py exits 0 on full-pass; non-zero with a
       summary line per failure.
    5. Eval greps the test output for "RESULT: PASS".

The mock dongle is NOT a firmware substitute. It is a fixture-driven
shim that lets the UI's WS handlers and event subscribers be
exercised without a real ESP32-S3 in the loop. Real firmware testing
remains on-car (manual, per-feature).

Wiring (small focused changes outside ui/):
  - firmware/build.sh — invoke `python3 ../tools/bundle_ui.py
    --in ../ui/control_panel.html ../ui/control_panel.css
    ../ui/control_panel.js --out ./futuner_control_panel.html`
    BEFORE the existing idf.py build line.
  - tools/bundle_ui.py — NEW; concatenates the three sources into the
    single-file firmware copy. Must produce byte-identical output for
    identical inputs (deterministic). No timestamps, no random IDs.
  - .gitignore — add ui/test/__pycache__/, ui/test/*.pyc.
  - firmware/futuner_control_panel.html — STAYS at the same path on
    disk (the firmware partition expects it there) but its content is
    now generated from ui/. The agent should commit the regenerated
    bundle once at the end so the firmware build still works without
    requiring developers to run the bundle script before every build
    — but going forward, build.sh refreshes it automatically.

Acceptance criteria — verify ALL before declaring done:
  - firmware/test/ui/eval.sh exits 0 (full eval, no skip flags).
  - firmware/test/verify_frozen.sh exits 0 (six frozen files unchanged).
  - firmware/build.sh exits 0 (the bundle step runs cleanly; idf.py
    build succeeds against the regenerated firmware copy).
  - All FIVE prior eval gates still PASS regression-clean
    (feature_manager, wot_logger, dtc, vin_pairing, sbf). UI work
    must not regress any of them — and CAN'T, since UI work doesn't
    touch firmware/src/, but the eval runs them anyway as a safety
    net.
  - All nine required test scenarios literally present in
    ui/test/test_round_trip.py.
  - Bundle script is deterministic: running it twice produces
    byte-identical firmware/futuner_control_panel.html.
  - cloud/tests passes (no cloud changes, but regression check).

Hard rules carry through (CLAUDE.md):
  - CAN ID 0x7E0 only. (Stated reflexively — UI doesn't talk CAN.)
  - No magic numbers. New JS constants in a named const block at the
    top of ui/control_panel.js with `Proposed default — needs Sean's
    approval before lock.` annotation. New CSS values in :root with
    same annotation.
  - ON/OFF discipline through feature_manager. UI surfaces the state;
    arbitration stays on the firmware side.
  - Mandatory: append to ~/esp/obd/status-2026-05-06.md and
    ~/esp/obd/file-update-2026-05-06.md (today's logs at workspace
    root, not in the project folder).

When done:
1. cd ~/esp/obd/FUTV1.1
2. firmware/test/ui/eval.sh
3. If RESULT: PASS — re-run the five prior gates AND verify_frozen.sh,
   confirm regression-clean, print full eval output and the proposed
   defaults table from ui/control_panel.js + new :root CSS variables.
   Hand back.
4. If RESULT: FAIL — fix and re-run. Do not declare done with any
   FAIL.

Ask clarifying questions BEFORE writing code if anything in the
event-bus shim, the mock-dongle scripted-sequence engine, the bundle
script's determinism contract, or the registry-coverage grep is
unclear. The bundle determinism is the easiest place to silently
regress: a stray build-time timestamp in the generated HTML defeats
the "byte-identical given identical inputs" check. If your design
finds itself wanting to inline a build timestamp or version string,
stop and ask — the answer is almost certainly "put it in a runtime
WS response, not the bundled HTML."

Proceed.
```

---

## Notes for Sean

Three things in this prompt are scope expansions beyond the
skeleton's original framing:

The **JS/CSS extraction from the inline HTML** (Q1 + Q4 sub-decision)
is necessary to make Layer-B JS syntax checking work. `node --check`
on a `.html` file with embedded `<script>` blocks needs a script
extraction step that's brittle. Splitting the files cleanly and
having `tools/bundle_ui.py` reassemble them at build time gives us
both lintable sources AND the single-file form the firmware
partition expects. The extra deliverable is the bundle script
(`tools/bundle_ui.py`, ~80 lines) — small, auditable, and
deterministic by design.

The **silent-event-drop bug fix** (Q2) is technically a bug in the
existing `handleMsg()` logic that predates Prompt 5. The SBF
orchestrator has been broadcasting `apply_started` /
`apply_progress` / `apply_completed` since Prompt 5 landed; the UI
has been silently dropping every one of those events because
`handleMsg()` only special-cases `event === 'can_frame'`. This
prompt fixes it as part of the bus work, which is the right place —
the bus shim IS the fix. No separate one-line follow-up commit.

The **mock-dongle scripted-sequence engine** (Q4 sub-clarification
from Sean) is the load-bearing piece of Layer C. Without
fixture-driven event sequences, the eval verifies request/response
shape but cannot catch a regression of the silent-event-drop bug.
The sequencer is ~40 lines on top of the response-fixture machinery
and pays for itself on the first feature that adds a new event
type — which is Prompt 6 (BLE pairing). After Prompt 9, every
feature prompt's UI work includes a fixture-sequence entry alongside
its command bindings; the eval catches drift automatically.

The agent's likely pre-flight clarification questions: (a) bundle
script determinism — should it sort the JS const block alphabetically
to stabilize diffs, or preserve author order? Recommend preserve
author order; alpha-sort hides intent. (b) Whether the migration of
the legacy `get_errors`/`clear_errors` Diagnostics buttons happens in
this prompt or as a follow-up. Recommend in this prompt — it's a
pure rebind of two button handlers, ~10 lines of JS, and leaving the
old commands wired to "Read DTCs" buttons that don't actually read
DTCs is a documented hostile-dogfood today. (c) Whether the WOT
"Force Upload Now" button gets a dummy handler with a tooltip or is
omitted entirely. Recommend omitted entirely; ghost UI controls that
don't work are worse than absent ones. The skeleton listed it but
the firmware doesn't expose it, so out-of-scope per Q9.

After Prompt 9 ships, Prompts 6/7/8 each include their own UI surface
as part of feature scope (BLE pairing UI in Prompt 6, ethanol
constraint visualization in Prompt 7, transport-status indicator in
Prompt 8). No more standalone UI sweeps — the catchup is the only
one.
