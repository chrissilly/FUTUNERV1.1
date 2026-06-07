# FUTUNER Phase 1 — HIL Validation Handoff

> Paste-ready prompt for executing Phase 1 hardware-in-loop validation against a real keyed-on MDG1 vehicle (dev RS7 or another paired car). Self-contained — no prior session context needed.

---

## Context

You are picking up Phase 1 hardware-loop validation of the FUTUNER aftermarket ECU tuning dongle. Phase 1 features have already been built, eval-gated, and shadow-validated. Your job is to exercise the dongle's Phase 1 surfaces against real silicon, triangulating WebSocket commands, real-time Candlelight wire capture, and live UI observation to prove the three streams agree.

This is **read-only validation against the ECU**. You do not modify any firmware C code, you do not enable Phase 2, and you do not commit anything as part of the validation run itself.

---

## Host machine prerequisites

The host running Claude Code needs:

- **OS**: macOS or Linux. Avoid native Windows — the project's path conventions and tooling assume Unix.
- **ESP-IDF**: version matching `firmware/sdkconfig` or `CMakeLists.txt` pin.
- **Python 3** with: `gs_usb`, `pyserial`, `requests`, `websockets`.
- **Candlelight USB-CAN** with appropriate driver/udev rules. On Linux, gs_usb kernel module + SocketCAN preferred.
- **Browser** (Chrome or Firefox) for the UI observation stream.
- **Network access** to the FUTUNER cloud server (for dashboard verification) and to the dongle's local subnet (after STA pairing).

---

## Hardware preconditions (owner stages these; Claude verifies each)

- Dev RS7 keyed ON, engine OFF, in a safe location (not on street; not blocking traffic).
- Dongle plugged into OBD-II port via Y-splitter.
- Candlelight USB-CAN on the other Y-splitter leg, USB to host (passive sniffing only — never emits).
- Phone or laptop on same network as dongle (post-pairing).
- Battery > 13.0 V (verify via dongle voltage read).
- Ethanol sensor: optional. Validate Phase 8 differently if absent.
- Browser open to the dongle's UI URL after pairing completes.

---

## Files this validation will produce

- `firmware/test/hil_phase1/captures/continuous_<ts>.candump` — full-session wire capture
- `firmware/test/hil_phase1/captures/anomaly_watch_<ts>.log` — any frames on non-allow-listed CAN IDs
- `firmware/test/hil_phase1/ui_state/phase_<N>_<feature>_<when>.json` — UI snapshots per phase
- `firmware/test/hil_phase1/logs/ws_session_<ts>.jsonl` — dongle WebSocket command/response log
- Today's `status-YYYY-MM-DD.md` and `file-update-YYYY-MM-DD.md` at the workspace root, with the chip report appended

---

## What "pass" means

All three streams agree per phase: WS log + UI display + Candlelight wire capture all consistent. No frames on disallowed CAN IDs throughout. No gateway lockout. Battery above 13 V at start and end. 10 host eval checks green before AND after (9 eval.sh gates + wifi_manager host_test_runner binary).

Three-stream agreement is the actual contract. The dongle's own claims via WS are not enough — Candlelight is the independent wire witness, the UI is the independent display witness, and the WS log is the control-plane evidence. If any one disagrees with the other two, that phase fails.

---

## Prerequisites (one-time, before dispatch)

Run these once on the host machine before starting the validation prompt:

```bash
cd ~/esp/obd/FUTV1.1
mkdir -p firmware/test/hil_phase1/{ui_state,captures,logs}
chmod +x firmware/build.sh firmware/flash.sh firmware/monitor.sh
```

---

## Known open issues that affect this validation

- **P-28 (open):** WOT logger feature 1 fails to register at boot because no logger profile is loaded before `wot_logger_init()` runs. **Phase 5 of this validation is EXPECTED-FAIL until P-28 closes.** Do not interpret Phase 5 failure as a regression; cross-reference `docs/PHASE_2_PREREQUISITES.md` P-28 root-cause section and skip Phase 5 with `N/A — blocked on P-28` in the chip report.
- **P-33 (open, low):** `firmware/test/wifi_manager/eval.sh` requires bash 4+ for `declare -A`. macOS stock bash 3.2 fails the wrapper but the underlying `host_test_runner` binary passes all assertions. Either install bash via brew (`brew install bash`) or invoke the test binary directly: `./firmware/test/wifi_manager/host_test_runner`.

---

## Validation prompt (paste this into Claude Code on the host machine)

```
Phase 1 hardware-loop validation against the dev RS7 (or any
keyed-on MDG1 vehicle with dongle paired). This is a handoff —
you have no prior session context. Bootstrap from the docs, flash
the correct firmware, then triangulate validation across three
streams in parallel: WS commands + Candlelight wire capture + UI
browser observation. All three streams must agree for each phase
to pass.

==========================================================
Context bootstrap (read in this order)
==========================================================

- ~/esp/obd/CLAUDE.md   (workspace router)
- ~/esp/obd/FUTV1.1/CLAUDE.md   (hard rules: CAN ID 0x7E0/0x7E8
                                  only, ON/OFF discipline, no magic
                                  numbers, mandatory status logs)
- ~/esp/obd/FUTV1.1/docs/MISSION_SPEC.md   (Phase 1 + Phase 2 spec)
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_VALIDATE_DONGLE.md
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_FLASH_ONLY.md   (dongle flash
                                  procedure)
- ~/esp/obd/FUTV1.1/docs/PHASE_2_PREREQUISITES.md   (P-items, including
                                  P-28 which blocks Phase 5 of this
                                  validation)
- ~/sniffer/can_tail.py (canonical wire witness — Sean directive
  2026-05-19; legacy tooling background captured in P-52).
- ~/esp/obd/FUTV1.1/tools/ws_driver.py --help
- ~/esp/obd/FUTV1.1/firmware/src/commands/commands.c

Handoff state:
- Phase 1 features all shipped: feature_manager, wot_logger, dtc,
  vin_pairing, sbf live tune, ui, ethanol BLE bridge, ethanol
  live-update constraints + rev limiter safety, wifi_manager (mode
  intent control).
- All 10 host eval checks green at HEAD (9 eval.sh + wifi_manager binary); Phase 2 orchestrator built
  but OFF by default (FUTUNER_PHASE2_ENABLED=0).
- Dongle MAC, ECU box code (4K0907557G__0003), VIN on file in
  secrets/ + variant manifest.
- main is the integration branch. PRs welcome but not required for
  small fixes; commit directly with a descriptive message.

==========================================================
Absolute rules (carry through every step)
==========================================================

- CAN ID 0x7E0 (request) / 0x7E8 (response) ONLY. Any frame on
  any other ID outside the normal boot-up window — STOP and
  surface. C8 J533 gateway lockout pattern (persistent timeouts,
  NRC 0x10, NRC 0x12) — STOP, key off, wait 10+ minutes, retry.
- Phase 2 stays OFF. Build with FUTUNER_PHASE2_ENABLED=0
  (this is the default).
- Battery > 13.0 V required before any wire activity. Verify
  before AND after each phase.
- No firmware C changes as part of the validation run itself.
  (The session that closes P-28 is separate from this one.)
- Mandatory progress logs: append to
  ~/esp/obd/status-YYYY-MM-DD.md and
  ~/esp/obd/file-update-YYYY-MM-DD.md.
- Proprietary IP: nothing leaves the local filesystem except via
  the existing FUTUNER cloud server.

==========================================================
STEP 1 — Build and flash the validation firmware
==========================================================

You are responsible for flashing the dongle.

  1. Sanity-check the toolchain by doing a clean host build first:
       cd ~/esp/obd/FUTV1.1
       bash firmware/build.sh
     Watch for any compile errors. If anything fails, halt and
     surface — don't try to push past a broken build.

  2. Flash the dongle:
       bash firmware/flash.sh
     (FUTUNER_PHASE2_ENABLED defaults to 0 in
      firmware/src/config/futuner_config.h. If you have any doubt,
      verify with:
        grep -nE '^#define FUTUNER_PHASE2_ENABLED' \
            firmware/src/config/futuner_config.h
      Expect: `#define FUTUNER_PHASE2_ENABLED 0`. Overriding requires
      `idf.py build -DFUTUNER_PHASE2_ENABLED=1` — flash.sh does NOT
      take a -D flag.)
     - Print the git rev hash before flashing:
         git rev-parse HEAD > /tmp/flash_rev.txt
       and append to today's status log.
     - Watch the serial boot log post-flash via:
         bash firmware/monitor.sh
       Expect:
         "FUTUNER vX.Y.Z (Phase 1 build, Phase 2 disabled)"
         "feature_manager initialized"
         "(no Phase 2 banner)"
       If you see a Phase 2 banner, abort the validation —
       wrong build flashed.

  3. Verify dongle is reachable post-flash (dongle is in AP mode at
     192.168.10.1 — join its AP SSID first if you haven't paired it
     to STA yet):
       python3 tools/ws_driver.py --host 192.168.10.1 \
           --script get_status --script list_commands
       — `get_status` should print firmware build info, VIN pairing
         state, and currently registered features.
       — `list_commands` enumerates the WS command surface so you can
         confirm the build matches expectations.
       — If `wot_log_start` does NOT appear in `list_commands` output
         (or `get_status` lists no WOT_LOGGING feature), that's P-28
         showing itself. Note it; Phase 5 will be marked
         expected-fail.

==========================================================
STEP 2 — Pre-validation regression baseline
==========================================================

  Before touching the car, prove no regressions by running all
  10 host eval checks (9 eval.sh gates + wifi_manager binary):

    cd ~/esp/obd/FUTV1.1
    for g in firmware/test/feature_manager/eval.sh \
             firmware/test/wot_logger/eval.sh \
             firmware/test/dtc/eval.sh \
             firmware/test/vin_pairing/eval.sh \
             firmware/test/sbf/eval.sh \
             firmware/test/ui/eval.sh \
             firmware/test/mdg1_payload/eval.sh \
             firmware/test/can_capture/eval.sh \
             firmware/test/mdg1_flash_orchestrator/eval.sh; do
      echo "=== $g ==="
      SKIP_IDF_BUILD=1 bash "$g" || echo "FAIL: $g"
    done

    # wifi_manager eval.sh requires bash 4+ (P-33). On stock macOS
    # bash 3.2, bypass the wrapper and run the host_test_runner binary
    # directly — it is the authoritative check.
    echo "=== firmware/test/wifi_manager/host_test_runner ==="
    ./firmware/test/wifi_manager/host_test_runner \
        || echo "FAIL: wifi_manager/host_test_runner"

  Must print all 10 checks as PASS. Any FAIL → halt, surface. Don't
  proceed to hardware until the codebase regression-baseline is clean.

==========================================================
STEP 3 — Hardware preconditions
==========================================================

Owner stages these; you verify each:
- Dev RS7 keyed ON, engine OFF, in a safe location
- Dongle plugged into OBD-II via Y-splitter
- Candlelight USB-CAN on the other Y-splitter leg, attached to
  this Mac via USB (passive only — never emits)
- Phone or laptop on same network as dongle (post-pairing)
- Battery > 13.0 V (verify via dongle voltage read)
- Ethanol sensor: optional. Validate Phase 8 differently if absent.
- Browser open to the dongle's UI URL (after pairing completes)
  on the same network device

Print: "Stage hardware per the list above. Press Enter when
done." Wait for owner. Verify each precondition via
sensors/OBD reads before proceeding to Step 4.

==========================================================
STEP 4 — Start continuous monitoring (three streams)
==========================================================

Before any phase runs, start ALL THREE monitoring streams in
parallel. They run for the entire validation session.

STREAM A — Candlelight continuous sniff:
  TS=$(date +%Y%m%d_%H%M%S)
  CAP=firmware/test/hil_phase1/captures/continuous_${TS}.candump
  ~/sniffer/can_tail.py 500000 "$CAP" &
  SNIFF_PID=$!

  - can_tail.py captures EVERYTHING on the bus to the named outfile
    (default ./candump.log; second arg overrides). No --filter, no
    --allow, no --timestamp flag — the candump log format already
    includes the timestamp prefix `(relative_seconds.us)`.
  - Stop the sniff with SIGINT: `kill -INT $SNIFF_PID`. The signal
    handler in can_tail.py runs notifier.stop() + bus.shutdown()
    cleanly.
  - Anomaly detection is a POST-CAPTURE grep — the candump file
    contains every frame; the analyzer surfaces non-allowlisted IDs:
      grep -vE 'can0 (7E0|7E8|<vehicle-bus-IDs-from-baseline>)#' \
        "$CAP" > firmware/test/hil_phase1/captures/anomaly_watch_${TS}.log
    If this writer has any content after a phase — STOP, surface to
    owner. Possible gateway probe, tool contention, or unexpected
    dongle behavior.
  - Sustained-use note: P-52 documents a macOS gs_usb wedge after
    ~30 min. If frame growth stalls mid-session, SIGINT the sniff
    and restart fresh (`mv` the candump to a per-window file first
    so the new sniff lands in a clean log).

STREAM B — UI browser observation:
  - UI must be open in a browser tab on the same network device
  - For each phase, capture the UI state as evidence by polling
    the dongle's status snapshot commands and tee'ing them to disk:
      python3 tools/ws_driver.py --host <dongle-ip> \
          --script get_status \
          --script license_status \
          --script wifi_status \
          --script live_tune_status \
          > firmware/test/hil_phase1/ui_state/phase_${N}_${FEATURE}_${WHEN}.json
    Repeat the snapshot before-phase / during-phase / after-phase.
    ws_driver.py has no built-in subscription mode; it streams sends
    + responses to stdout per-command, which is what you redirect.
    Fallback: browser screenshot if a snapshot command isn't covering
    the UI surface you need.
  - Per-phase: snapshot before phase starts, snapshot during,
    snapshot after. Save to
    firmware/test/hil_phase1/ui_state/phase_<N>_<feature>_<when>.json
  - The UI's displayed values for each phase MUST agree with the
    wire-level activity Candlelight captured AND the WS command
    responses. Three-way agreement is the pass criterion.

STREAM C — Dongle WS command/response log:
  - All WS commands you send + all responses received get logged to
    firmware/test/hil_phase1/logs/ws_session_${TS}.jsonl
  - ws_driver.py streams every send + response to stdout in the
    format `# WS send: ...` / `< <reply>`. Redirect stdout to the
    session log:
      python3 tools/ws_driver.py --host <dongle-ip> --script <cmd> \
          >> firmware/test/hil_phase1/logs/ws_session_${TS}.jsonl 2>&1
    (ws_driver.py has no --log flag; redirection is the supported
    pattern. Use `tee -a` if you want stdout + file simultaneously.)
  - This is your control-plane evidence: what you asked the dongle
    to do, what it said back.

==========================================================
STEP 5 — Per-phase validation
==========================================================

For EACH phase below: tag the continuous candump with a phase
marker (echo "# PHASE N START <ts>" >> the candump file, do the
phase, echo "# PHASE N END <ts>"). This lets the post-session
analyzer slice the continuous capture per-phase.

Each per-phase eval-gate invocation uses the corresponding host
test as a sanity layer before exercising the wire:

PHASE 1 — Two-phase baseline (passive 0-frame + active >=2-frame)

  Two-phase because the VAG MLB/MQB diagnostic CAN trunk is SILENT
  until provoked — the C8 J533 gateway does NOT broadcast on the
  diagnostic bus unprovoked, so a "passive baseline" that requires
  visible traffic would NO-GO a perfectly healthy car. The prior
  single-phase ">100 frames in 5s passive" assumption was empirically
  invalidated on 2026-05-19: 0 frames in a 5s strict sniff, but 64
  frames over the next 15s after a single session-provoking command.
  The new contract verifies BOTH conditions: bus is quiet on its
  own (catches renegade broadcasters / leftover sessions) AND
  responds when prodded (catches dead bus / wrong bittiming / bad
  splitter leg).

  PHASE 1a — Passive snapshot (assert quiet bus)
    CAP_1A=firmware/test/hil_phase1/captures/baseline_passive_${TS}.candump
    ~/sniffer/can_tail.py 500000 "$CAP_1A" &
    PID_1A=$!
    sleep 2
    kill -INT $PID_1A; wait $PID_1A 2>/dev/null
    FRAMES_1A=$(grep -cE '^\(' "$CAP_1A" 2>/dev/null)
    ASSERT: $FRAMES_1A == 0. Any frames here indicate a renegade
    broadcaster, leftover diagnostic session from a prior tool, or
    ghost CAN host on the bus. HALT and surface to owner.
    (Anomaly subset: same as Stream A post-capture grep above —
     run it against $CAP_1A to confirm no non-allowlisted IDs.)

  PHASE 1b — Active snapshot (assert keepalive engages)
    Single session-provoking WS command, then sniff for 3 seconds.
    Provoke via dtc_read (TesterPresent isn't in the WS command
    surface; dtc_read opens a UDS session and yields the same
    keepalive witness on 0x7E0/0x7E8):
      CAP_1B=firmware/test/hil_phase1/captures/baseline_active_${TS}.candump
      ~/sniffer/can_tail.py 500000 "$CAP_1B" &
      PID_1B=$!
      sleep 0.5  # let the sniffer subscribe before traffic starts
      python3 tools/ws_driver.py --host <dongle-ip> --script dtc_read
      sleep 3
      kill -INT $PID_1B; wait $PID_1B 2>/dev/null
      FRAMES_1B=$(grep -cE '^\(' "$CAP_1B")
    ASSERT: $FRAMES_1B >= 2 (one 19 02 dtc-read request + one 59 02
    positive response minimum; expect additional 3E 00 / 7E 00 keepalive
    pairs at ~4 Hz over 3s if the dongle holds the session open).
    Three-stream witness verified: WS-driven request reflected in
    wire capture.
  - Pass: 1a $FRAMES_1A == 0, 1b $FRAMES_1B >= 2, no anomaly hits
    (post-capture grep clean for both).

PHASE 2 — VIN pairing (only if dongle is unpaired or owner
authorizes factory-reset)
  - Host sanity gate:
      SKIP_IDF_BUILD=1 bash firmware/test/vin_pairing/eval.sh
  - If already paired: skip on-car phase. Note in report.
  - If unpaired: walk the AP→STA→server flow per MISSION_SPEC §1.1
    * Verify dongle's AP SSID appears
    * Phone joins AP, enters STA creds
    * Dongle reboots to STA, joins network
    * Dongle reads VIN via 0x09 0x02 (verify wire shows this read)
    * Dongle phones home, receives token (verify in WS log)
    * Server DB has the new entry (query cloud server)
    * Lock test: present token with different VIN — must reject
  - Three-stream agreement: WS shows pairing complete + UI shows
    "paired to VIN <X>" + wire showed exactly one VIN read.

PHASE 3 — Feature manager arbitration
  - Host sanity gate:
      SKIP_IDF_BUILD=1 bash firmware/test/feature_manager/eval.sh
  - Manual layer: start feature A via WS (e.g., live_tune_start —
    wot_log_start would fail per P-28). live_tune_start takes
    params {stage, ethanol_pct} per firmware/src/commands/sbf_commands.c;
    use a known-safe test SBF stage:
      python3 tools/ws_driver.py --host <dongle-ip> --script \
        'live_tune_start {"stage":1,"ethanol_pct":0}'
    Verify UI shows A running, attempt to start feature B, verify
    warning + clean stop of A before B starts, UI reflects each
    transition, wire shows no overlapping UDS traffic from A and B.
  - Pass: WS + UI + wire all agree on state transitions; no
    "both active" state ever appears.

PHASE 4 — DTC read/clear
  - Host sanity gate:
      SKIP_IDF_BUILD=1 bash firmware/test/dtc/eval.sh
  - Wire check: read uses 0x19 only, clear uses 0x14 only
  - Round-trip: read → display in UI → clear (with owner
    confirmation if real DTCs present) → re-read empty
  - Pass: wire matches expected services exactly, UI shows DTC
    list pre-clear and empty post-clear, WS log matches

PHASE 5 — WOT logger  ⚠️ EXPECTED-FAIL per P-28
  - DO NOT treat failure here as a regression. P-28 documents
    the known boot-init-order issue that prevents feature_id=1
    (FEATURE_WOT_LOGGING) from registering. Until P-28 closes,
    wot_log_start will return
    {"error":"feature id 1 is not registered","active_feature":"none"}.
  - Action: mark Phase 5 as "N/A — blocked on P-28" in the chip
    report. Note the rc=258 boot log line if visible (confirms
    the same root cause). Don't waste cycles trying to exercise
    the rest of the WOT lifecycle.
  - Host sanity gate (still useful — confirms the host harness
    side is sound even if the firmware side regresses):
      SKIP_IDF_BUILD=1 bash firmware/test/wot_logger/eval.sh
    Must still pass at the host level — that's testing the
    recorder/uploader unit logic in isolation, not the
    feature-manager registration path.

PHASE 6 — SBF live tune + ethanol constraints
  - Host sanity gate:
      SKIP_IDF_BUILD=1 bash firmware/test/sbf/eval.sh
  - Load a known test SBF (stage_one.sbf), select E50
  - Verify RAM updates complete in 1.5–2 s window (timestamp
    from WS, confirmed against wire capture)
  - Constraint suite (5 checks per MISSION_SPEC §1.5):
    * <3% change → no update fires
    * >3% sustained <60s → no update
    * >3% sustained >60s → update fires; rev limiter drops to
      4000 RPM (verify via ECU read, no actual engine rev)
    * 30s post-update stabilization → rev limiter restored
    * Synthetic WOT during pending update → update locked
  - Three-stream agreement across all five checks

PHASE 7 — UI / WebSocket live gauges
  - Host sanity gate:
      SKIP_IDF_BUILD=1 bash firmware/test/ui/eval.sh
  - UI already open from Step 4. Verify each gauge populates
    with live ECU data:
      RPM, boost, AFR, ignition timing, coolant temp, IAT,
      throttle position, load
  - Update rate ~10 Hz nominal; no stuck values
  - Fault section shows current DTCs (should be empty post-Phase 4)
  - Wire confirms: dongle is actively reading these PIDs via UDS

PHASE 8 — Ethanol BLE bridge OR manual fallback
  - If sensor present: verify BLE link active, cycle the sensor
    (owner momentarily power-cycles it), verify UI falls back to
    manual within timeout, then auto-reconnects
  - If sensor absent: validate manual-input path. Set manual
    ethanol value via UI, confirm tune updates per Phase 6
    constraints

==========================================================
STEP 6 — Session teardown and analysis
==========================================================

  1. Kill continuous sniffs: kill $SNIFF_PID $ANOMALY_PID
  2. Parse the continuous candump:
       - Count frames per CAN ID — must be ONLY 0x7E0/0x7E8 plus
         baseline-allowed vehicle bus IDs
       - Find any anomaly-watch entries
       - Verify per-phase markers are present and well-ordered
  3. Compare WS log timestamps to wire capture timestamps to
     confirm the dongle did what the WS log claims (e.g., if WS
     said "DTC read at T=X", wire should show a 0x19 request at
     T=X±100ms)
  4. Verify per-phase UI snapshots agree with WS responses
  5. Verify battery voltage at end > 13.0 V
  6. Re-run the 10 host eval checks from Step 2 to confirm no
     regression after the on-car session.

==========================================================
Acceptance criteria
==========================================================

- Correct firmware flashed: HEAD git rev, Phase 2 disabled
- All 10 host eval checks PASS pre-hardware AND post-hardware (9 eval.sh + wifi_manager binary)
- Phases 1–8: each PASS, or N/A (with reason). Phase 5 is
  expected N/A per P-28.
- Three-stream agreement at every phase that runs: WS log + UI
  state + wire capture all consistent
- No frames on disallowed CAN IDs throughout session
  (continuous capture is the witness)
- No anomaly-watch hits
- No NRC 0x10/0x12 lockout pattern
- Battery > 13.0 V at start AND end
- Wire captures archived: continuous + per-phase markers + anomaly
  watch
- UI state snapshots archived per phase
- WS session log archived

==========================================================
Forbidden
==========================================================

- Building with FUTUNER_PHASE2_ENABLED=1
- Any UDS service that writes flash (0x34, 0x36, 0x37)
- Clearing real DTCs without owner confirmation
- Sending any captured log, .bin, or wire trace to a remote
  service that isn't the existing FUTUNER cloud server
- Killing the continuous sniff before the validation completes
  (it's the evidence record)

==========================================================
When done
==========================================================

Print:

  Phase 1 HIL validation — YYYY-MM-DD HH:MM
  ==========================================
  Firmware flashed:                  HEAD <git-rev>, Phase 2 OFF
  Pre-flight (env + 10 checks):      PASS/FAIL
  Continuous sniff started:          PASS/FAIL
  Anomaly watch hits:                <count> (zero is the goal)
  Phase 1 — passive baseline:        PASS/FAIL
  Phase 2 — VIN pairing:             PASS/FAIL/SKIP-already-paired
  Phase 3 — feature manager:         PASS/FAIL
  Phase 4 — DTC read/clear:          PASS/FAIL
  Phase 5 — WOT logger:              N/A — blocked on P-28
  Phase 6 — SBF + ethanol constraints: PASS/FAIL  (5/5 sub-checks)
  Phase 7 — UI live gauges:          PASS/FAIL
  Phase 8 — ethanol BLE / fallback:  PASS/FAIL/N-A
  Three-stream agreement:            PASS/FAIL  (per phase)
  CAN ID hygiene:                    only 0x7E0/0x7E8 + baseline
  Gateway lockout events:            <count> (zero is the goal)
  Battery start / end:               <V> / <V>
  Anomalies:                         <list or "none">
  Post-hardware 9-gate sweep:        PASS/FAIL

  Output files:
    firmware/test/hil_phase1/captures/continuous_<ts>.candump
    firmware/test/hil_phase1/captures/anomaly_watch_<ts>.log
    firmware/test/hil_phase1/ui_state/phase_<N>_<feature>_<when>.json
    firmware/test/hil_phase1/logs/ws_session_<ts>.jsonl

Append the chip report block to today's
status-YYYY-MM-DD.md under "## Phase 1 HIL validation" and a
per-file delta block to file-update-YYYY-MM-DD.md (only the
captures + logs — no secrets, no code changes).

Hand back.

Ask the owner BEFORE you proceed if anything is unclear,
particularly: (a) factory-reset for Phase 2 VIN pairing test or
skip if already paired; (b) engine-running WOT authorized for
Phase 5 (default NO — synthetic only); (c) clearing real DTCs in
Phase 4 (default NO — surface first, owner decides).

Recommended defaults if owner unreachable: skip Phase 2 if
already paired (note in report); synthetic WOT only; surface
real DTCs without clearing.

Proceed.
```

---

## Notes for whoever is at the keyboard (not for the Claude agent)

- The validation is **read-only against the ECU**. No flash writes, no Phase 2 activation, no destructive UDS. The dongle reads ECU data, exercises Phase 1 features, and verifies behavior. Nothing changes on the car.
- Expected duration: 1–3 hours depending on whether VIN pairing needs to be re-walked and whether the ethanol sensor is in scope.
- If anything goes sideways (gateway lockout, dongle bricks, unexpected wire traffic), the agent will halt and surface — don't expect it to push through. Halting is the correct behavior.
- The output files all land under `firmware/test/hil_phase1/`. After the session, those can be shared back to the primary development session for review.
- **Phase 5 (WOT logger) is expected to fail until P-28 closes.** That's a documented known issue, not a regression introduced by the validation run.
- Treat `main` as the integration branch; commits are welcome but not required for the validation run itself. The validator-agent's job is to exercise + report, not to merge fixes.
