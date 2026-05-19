# FUTUNER Phase 1 — HIL Validation Handoff

> Pickup doc for `rabbit@sillyrabbitmotorsport`'s Claude account. No prior session context required. This file is self-contained: it explains the validation scope, the setup the host machine needs, and the paste-ready prompt the Claude agent should execute.

---

## Context

You are picking up Phase 1 hardware-loop validation of the FUTUNER aftermarket ECU tuning dongle against a dev RS7 (or any keyed-on MDG1 vehicle the dongle has been paired with). Phase 1 features have already been built, eval-gated, and shadow-validated by the primary development session. Your job is to exercise the dongle's Phase 1 surfaces against real silicon, triangulating WebSocket commands, real-time Candlelight wire capture, and live UI observation to prove the three streams agree.

This is **read-only validation**. You do not modify any firmware C code, you do not enable Phase 2, and you do not commit anything to git. The push freeze is active workspace-wide.

---

## Host machine prerequisites

The host running Claude Code needs:

- **OS**: macOS, Linux, or Windows-with-WSL2-Ubuntu. Avoid native Windows — the project's path conventions and tooling assume Unix.
- **ESP-IDF**: version matching `firmware/sdkconfig` or `CMakeLists.txt` pin.
- **Python 3** with: `gs_usb`, `pyserial`, `sshpass`, `requests`, `websockets`.
- **Candlelight USB-CAN** with appropriate driver/udev rules. On WSL2, attach via `usbipd-win`.
- **Browser** (Chrome or Firefox) for the UI observation stream.
- **Network access** to the FUTUNER cloud server (for dashboard verification) and to the dongle's local subnet (after STA pairing).

Run `tools/srm doctor` first — it catches missing deps before any car-side work.

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

- `firmware/test/hil_phase1/continuous_<ts>.candump` — full-session wire capture
- `firmware/test/hil_phase1/anomaly_watch_<ts>.log` — any frames on non-allow-listed CAN IDs
- `firmware/test/hil_phase1/ui_state/phase_<N>_<feature>_<when>.json` — UI snapshots per phase
- `firmware/test/hil_phase1/ws_session_<ts>.jsonl` — dongle WebSocket command/response log
- Today's `status-YYYY-MM-DD.md` and `file-update-YYYY-MM-DD.md` at the workspace root, with the chip report appended

---

## What "pass" means

All three streams agree per phase: WS log + UI display + Candlelight wire capture all consistent. No frames on disallowed CAN IDs throughout. No gateway lockout. Battery above 13 V at start and end. 8 prior eval gates green before AND after.

Three-stream agreement is the actual contract. The dongle's own claims via WS are not enough — Candlelight is the independent wire witness, the UI is the independent display witness, and the WS log is the control-plane evidence. If any one disagrees with the other two, that phase fails.

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

- ~/esp/obd/CLAUDE.md   (workspace router; PUSH FREEZE active)
- ~/esp/obd/FUTV1.1/CLAUDE.md   (hard rules: CAN ID 0x7E0/0x7E8
                                  only, ON/OFF discipline, no magic
                                  numbers, mandatory status logs)
- ~/esp/obd/FUTV1.1/docs/MISSION_SPEC.md   (Phase 1 + Phase 2 spec)
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_VALIDATE_DONGLE.md
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_FLASH_ONLY.md   (dongle flash
                                  procedure)
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_BUILD_SRM_CLI.md   (the
                                  tools/srm CLI consolidated all
                                  validation phases)
- ~/esp/obd/FUTV1.1/tools/srm/
- ~/esp/obd/FUTV1.1/tools/can_sniff.py --help
- ~/esp/obd/FUTV1.1/firmware/src/commands/commands.c

Handoff state:
- Phase 1 features all shipped: feature_manager, wot_logger, dtc,
  vin_pairing, sbf live tune, ui, ethanol BLE bridge, ethanol
  live-update constraints + rev limiter safety.
- All 8 prior Phase 1 eval gates green at HEAD; Phase 2
  orchestrator built but OFF by default.
- Dongle MAC, ECU box code (4K0907557G__0003), VIN on file in
  secrets/ + variant manifest.

==========================================================
Absolute rules (carry through every step)
==========================================================

- CAN ID 0x7E0 (request) / 0x7E8 (response) ONLY. Any frame on
  any other ID outside the normal boot-up window — STOP and
  surface. C8 J533 gateway lockout pattern (persistent timeouts,
  NRC 0x10, NRC 0x12) — STOP, key off, wait 10+ minutes, retry.
- Phase 2 stays OFF. Build with FUTUNER_PHASE2_ENABLED=0.
  Do NOT invoke tools/srm capture or tools/srm flash with
  Phase 2 enabled.
- Battery > 13.0 V required before any wire activity. Verify
  before AND after each phase.
- No firmware C changes. No cloud server changes. No commits.
  Push freeze is active workspace-wide.
- Mandatory progress logs: append to
  ~/esp/obd/status-YYYY-MM-DD.md and
  ~/esp/obd/file-update-YYYY-MM-DD.md.
- Proprietary IP: nothing leaves the local filesystem except via
  the existing FUTUNER cloud server.

==========================================================
STEP 1 — Build and flash the validation firmware
==========================================================

You are responsible for flashing the dongle. Don't ask the owner
to do it manually — that's why tools/srm flash exists.

  1. tools/srm doctor
       - Verify env (ESP-IDF, pyserial, gs_usb, sshpass, network
         reach to cloud server). Any FAIL → halt, surface.

  2. tools/srm flash
       - Build args: FUTUNER_PHASE2_ENABLED=0 (Phase 1 only; this
         is the default but pass it explicitly so the build is
         deterministic across machines)
       - Source: HEAD of FUTV1.1/
       - Target: dongle on the OBD-II port via USB-Serial (or
         OTA if pre-paired and reachable; tools/srm flash decides)
       - Print the git rev hash and build hash to status log
         before flashing
       - Watch the serial boot log post-flash; expect:
           "FUTUNER vX.Y.Z (Phase 1 build, Phase 2 disabled)"
           "feature_manager initialized"
           "(no Phase 2 banner)"
         If you see a Phase 2 banner, abort the validation —
         wrong build flashed.

  3. Verify dongle is reachable post-flash:
       - tools/srm status
       - Should show: firmware build hash matches HEAD, Phase 2
         disabled, VIN pairing state (paired/unpaired)
       - If running build doesn't match HEAD's expected hash,
         tools/srm flash didn't take — halt and surface.

==========================================================
STEP 2 — Pre-validation regression baseline
==========================================================

  Before touching the car, prove no regressions:

  tools/srm validate --eval-gates-only

  Must print all 8 gates as PASS:
    feature_manager / wot_logger / dtc / vin_pairing / sbf / ui
    / mdg1_payload / mdg1_flash_orchestrator

  Any FAIL → halt, surface. Don't proceed to hardware until
  the codebase regression-baseline is clean.

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
  tools/can_sniff.py --filter 0x7E0 0x7E8 \
      --timestamp \
      > firmware/test/hil_phase1/continuous_<session_ts>.candump &
  SNIFF_PID=$!

  - Don't filter to anything narrower; we want to see everything
    on the diagnostic IDs throughout the session
  - Also start a parallel UNFILTERED sniff at low priority that
    flags any frame on a non-allow-listed ID:
      tools/can_sniff.py --watch-for-anomalies \
        --allow 0x7E0,0x7E8,<vehicle-bus-IDs-from-baseline> \
        > firmware/test/hil_phase1/anomaly_watch_<session_ts>.log &
    ANOMALY_PID=$!
    If this writer prints anything during a phase — STOP that
    phase, surface to owner. Possible gateway probe, tool
    contention, or unexpected dongle behavior.

STREAM B — UI browser observation:
  - UI must be open in a browser tab on the same network device
  - For each phase, capture the UI state as evidence:
      * Via WS state-stream subscription (preferred — structured
        JSON, easy to diff against expected)
      * Or via screenshot if the WS state isn't capturable
        (fallback)
  - Per-phase: snapshot before phase starts, snapshot during,
    snapshot after. Save to
    firmware/test/hil_phase1/ui_state/phase_<N>_<feature>_<when>.json
  - The UI's displayed values for each phase MUST agree with the
    wire-level activity Candlelight captured AND the WS command
    responses. Three-way agreement is the pass criterion.

STREAM C — Dongle WS command/response log:
  - All WS commands you send + all responses received get logged to
    firmware/test/hil_phase1/ws_session_<session_ts>.jsonl
  - This is your control-plane evidence: what you asked the dongle
    to do, what it said back

==========================================================
STEP 5 — Per-phase validation
==========================================================

For EACH phase below: tag the continuous candump with a phase
marker (echo "# PHASE N START <ts>" >> the candump file, do the
phase, echo "# PHASE N END <ts>"). This lets the post-session
analyzer slice the continuous capture per-phase.

PHASE 1 — Passive baseline
  - Already running (continuous sniff started in Step 4).
  - Just verify: at least 1 frame seen in continuous capture
    in the first 5 seconds. Confirms wiring + key state.
  - Pass: frames seen, no anomaly-watch hits.

PHASE 2 — VIN pairing (only if dongle is unpaired or owner
authorizes factory-reset)
  - If already paired: skip. Note in report.
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
  - tools/srm validate --phase feature_manager
  - Manual layer: start feature A via WS, verify UI shows A
    running, attempt to start feature B, verify warning + clean
    stop of A before B starts, UI reflects each transition, wire
    shows no overlapping UDS traffic from A and B
  - Pass: WS + UI + wire all agree on state transitions; no
    "both active" state ever appears

PHASE 4 — DTC read/clear
  - tools/srm validate --phase dtc
  - Wire check: read uses 0x19 only, clear uses 0x14 only
  - Round-trip: read → display in UI → clear (with owner
    confirmation if real DTCs present) → re-read empty
  - Pass: wire matches expected services exactly, UI shows DTC
    list pre-clear and empty post-clear, WS log matches

PHASE 5 — WOT logger
  - tools/srm validate --phase wot_logger
  - Synthetic trigger only unless owner authorizes engine-on
    real WOT pull
  - Verify: 60s hard cap enforced, log gzipped locally (~3-4KB),
    uploaded to cloud, local file deleted on cloud confirm
  - Three-stream agreement: WS shows log lifecycle, UI shows
    "logging" indicator + "uploaded" + "purged", wire shows
    expected UDS reads (RPM, throttle, boost, AFR, etc.)
  - Cloud verification: log appears in dashboard, VIN-associated,
    all expected parameters present

PHASE 6 — SBF live tune + ethanol constraints
  - tools/srm validate --phase sbf
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
  - tools/srm validate --phase ui
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

  1. Kill continuous sniff: kill $SNIFF_PID $ANOMALY_PID
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

==========================================================
Acceptance criteria
==========================================================

- Correct firmware flashed: HEAD git rev, Phase 2 disabled,
  build hash matches HEAD
- All 8 prior eval gates PASS pre-hardware
- Phases 1–8: each PASS or explicitly N/A (with reason)
- Three-stream agreement at every phase: WS log + UI state +
  wire capture all consistent
- No frames on disallowed CAN IDs throughout session
  (continuous capture is the witness)
- No anomaly-watch hits
- No NRC 0x10/0x12 lockout pattern
- Battery > 13.0 V at start AND end
- Wire captures archived: continuous + per-phase markers + anomaly
  watch
- UI state snapshots archived per phase
- WS session log archived
- No commits, no Phase 2 activation, no firmware C changes

==========================================================
Forbidden
==========================================================

- Building with FUTUNER_PHASE2_ENABLED=1
- Invoking tools/srm capture or tools/srm flash --phase2
- Any UDS service that writes flash (0x34, 0x36, 0x37)
- Clearing real DTCs without owner confirmation
- Any commit, push, or branch creation
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
  Build hash:                        <hash>
  Pre-flight (env + 8 gates):        PASS/FAIL
  Continuous sniff started:          PASS/FAIL
  Anomaly watch hits:                <count> (zero is the goal)
  Phase 1 — passive baseline:        PASS/FAIL
  Phase 2 — VIN pairing:             PASS/FAIL/SKIP-already-paired
  Phase 3 — feature manager:         PASS/FAIL
  Phase 4 — DTC read/clear:          PASS/FAIL
  Phase 5 — WOT logger:              PASS/FAIL
  Phase 6 — SBF + ethanol constraints: PASS/FAIL  (5/5 sub-checks)
  Phase 7 — UI live gauges:          PASS/FAIL
  Phase 8 — ethanol BLE / fallback:  PASS/FAIL/N-A
  Three-stream agreement:            PASS/FAIL  (per phase)
  CAN ID hygiene:                    only 0x7E0/0x7E8 + baseline
  Gateway lockout events:            <count> (zero is the goal)
  Battery start / end:               <V> / <V>
  Anomalies:                         <list or "none">

  Output files:
    firmware/test/hil_phase1/continuous_<ts>.candump
    firmware/test/hil_phase1/anomaly_watch_<ts>.log
    firmware/test/hil_phase1/ui_state/phase_<N>_<feature>_<when>.json
    firmware/test/hil_phase1/ws_session_<ts>.jsonl
    (one per phase)

Append the chip report block to today's
status-YYYY-MM-DD.md under "## Phase 1 HIL validation" and a
per-file delta block to file-update-YYYY-MM-DD.md (only the
captures + logs — no secrets, no code changes).

Hand back. Don't commit.

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
- Push freeze is active workspace-wide; the agent is instructed not to commit. If you see it staging or committing, that's a bug and worth interrupting.
