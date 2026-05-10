# Claude Code — Bench Session Prompt (KOEO + Candlelight verification)

> Paste this into a Claude Code session running on your Mac with:
> (a) the dongle plugged into USB-C for flash + serial,
> (b) the Candlelight USB-CAN adapter plugged in alongside the
>     dongle (Y-splitter on the OBD-II port per
>     `docs/BENCH_CAN_TOOLKIT.md`),
> (c) the dev car key in the ON position with the engine NOT running
>     (KOEO — key-on, engine-off).
>
> The agent flashes, provisions, then walks every Phase 1 feature on
> the live dongle while the Candlelight sniffs the CAN bus
> independently. Every phase that issues a UDS command gets a
> two-sided check: WS reports success AND the Candlelight saw the
> expected request/response frames on the bus.
>
> Different from the earlier prompts: this one does NOT write code.
> It RUNS the existing tools (`tools/bench_push.py`,
> `tools/can_sniff.py`) plus an ad-hoc Python WS client against a
> real dongle wired to a real ECU. KOEO mode means we validate
> protocol correctness, not engine response — the engine is off, so
> "did you feel it?" doesn't apply. We rely on the bus capture
> instead.

---

## Paste this into Claude Code

```
Bench session — KOEO + Candlelight verification. Flash the dongle,
provision it, then validate every Phase 1 feature on the live
hardware while the Candlelight sniffs the CAN bus in parallel. Pause
and ask me ONLY when something can't be automated. Do NOT write new
code. Do NOT commit anything.

Operating mode (binding):
- Key ON, engine OFF (KOEO). The ECU is powered up on the CAN bus.
  We validate UDS request/response correctness; we do NOT validate
  engine response (no engine running).
- Candlelight USB-CAN is plugged in alongside the dongle via
  Y-splitter on the OBD-II port. tools/can_sniff.py talks to it.
- Every phase that issues a UDS command gets a TWO-SIDED check:
  WS response success AND CAN frames captured on 0x7E0 (request)
  and 0x7E8 (response).

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md                              (project rules)
- ~/esp/obd/FUTV1.1/docs/SESSION_HANDOFF.md                (background)
- ~/esp/obd/FUTV1.1/docs/upload2server.md                  (what bench_push.py automates)
- ~/esp/obd/FUTV1.1/docs/BENCH_CAN_TOOLKIT.md              (Candlelight setup)
- ~/esp/obd/FUTV1.1/tools/bench_push.py --help
- ~/esp/obd/FUTV1.1/tools/can_sniff.py --help              (--filter accepts hex IDs)
- ~/esp/obd/FUTV1.1/firmware/src/commands/commands.c       (WS command surface)
- ~/esp/obd/FUTV1.1/firmware/src/commands/dtc_commands.c   (UDS 0x19/0x14 frame shapes)
- ~/esp/obd/FUTV1.1/firmware/src/sbf/sbf_orchestrator.c    (event payload shapes)

ABSOLUTE rules carry through:
- CAN ID 0x7E0 only (request) / 0x7E8 only (response). Any other ID
  appearing on the bus during dongle activity is a red flag — STOP
  and surface.
- No firmware C changes. No source edits except the status /
  file-update logs.
- No new commits.
- Stop and ask me before any irreversible action.
- If anything looks wrong (ECU not responding, gateway lockout
  symptoms — persistent timeouts, NRC 0x10/0x12, unexpected IDs on
  the bus) STOP, surface the symptom, wait for me. Do NOT retry.

Pre-flight (do these silently and report a single PASS/FAIL line per
check):
1. cd ~/esp/obd/FUTV1.1
2. firmware/test/verify_frozen.sh                  (must PASS)
3. git status --porcelain                          (must be empty)
4. tools/bench_push.py --help                      (must exit 0)
5. tools/can_sniff.py --help                       (must exit 0)
6. ls /dev/cu.usbmodem*                            (must show one path — dongle)
7. python3 -c "import gs_usb"                      (Candlelight library;
                                                    must succeed — if not,
                                                    pip install gs_usb)
8. echo "$ADMIN_API_KEY" | wc -c                   (must be > 1)

If any pre-flight fails, STOP and tell me which one. Do not proceed.

Phase 0 — CAN baseline (verify Candlelight sees the dongle on the
bus before any feature work):
  - Start tools/can_sniff.py in the background, capturing to
    /tmp/futuner_bench_p0.log for 5 seconds, no filter.
  - During the capture, send {"command":"dtc_read"} from a Python WS
    client to the dongle.
  - Stop the capture.
  - PASS if the log contains at least one frame on 0x7E0 AND at
    least one on 0x7E8. FAIL if the bus was silent (means either
    Candlelight isn't seeing the wire, or the dongle isn't
    transmitting).
  - If FAIL, STOP. Do not continue. Tell me whether you saw any
    traffic at all (if NO frames whatsoever, Candlelight isn't
    wired correctly; if frames on other IDs, the gateway may not
    be allowing 0x7E0).

If Phase 0 passes, ask me one question:

  "Pre-flight + CAN baseline green. Dongle on $PORT. Candlelight
   seeing 0x7E0/0x7E8 traffic. About to flash + provision. The
   dongle's MAC will be enrolled on the cloud, an SBF will be
   uploaded, the device will be marked paid, and the dongle will
   be flashed. Proceed? (y/n)"

If I say y, run:
  tools/bench_push.py --all --port $PORT \
      --sbf-file sbf/stage1_patched.sbf --paid 1

Stream the output. If bench_push.py exits non-zero, STOP and surface
the error.

Once provisioning completes, capture the dongle's STA IP from the
bench_push summary (or via {"command":"wifi_status"} on WS). All
subsequent WS commands target ws://<sta_ip>/ws.

For every validation phase below, the pattern is:

  1. Start tools/can_sniff.py --filter 0x7E0 0x7E8 in a background
     subprocess, capturing to /tmp/futuner_bench_p<N>.log.
  2. Send the WS command(s) for the phase.
  3. Stop the sniffer.
  4. Parse the captured log: count frames on 0x7E0 and 0x7E8;
     extract UDS service IDs (request byte 1, response byte 1 with
     0x40 OR'd off).
  5. PASS only if BOTH the WS response shape was correct AND the
     captured frames match the expected service IDs for that phase.

Validation phases (six). After each phase, print a single-line PASS
/ FAIL / SKIP chip with a 2–3 line detail block.

Phase 1 — DTC read+clear. UDS 0x19 (read) and 0x14 (clear).
  - Sniff start (filter 0x7E0 0x7E8).
  - WS: {"command":"dtc_read"}. Capture response.
  - WS: {"command":"dtc_clear"}. Capture response.
  - Wait 2 s. WS: {"command":"dtc_read"}. Capture response.
  - Sniff stop.
  - WS-side PASS: all three responses success:true with shapes
    matching dtc_commands.c.
  - CAN-side PASS: the log contains at least one request with
    service byte 0x19 (read DTCs) AND at least one with 0x14 (clear
    DTCs); positive responses 0x59 and 0x54 on 0x7E8.
  - Combined PASS if both. Report DTC count before/after.

Phase 2 — VIN pair refresh + license. UDS 0x22 (read VIN PID 0x0902).
  - Sniff start.
  - WS: {"command":"vin_pair_now"}. Capture response.
  - WS: {"command":"license_status"}. Capture response.
  - Sniff stop.
  - WS-side PASS: vin_pair_now returns success:true with message
    "VIN paired"; license_status returns paid:true.
  - CAN-side PASS: log contains a 0x22 request on 0x7E0 (VIN read);
    positive 0x62 response on 0x7E8 carrying VIN bytes.
  - Combined PASS if both. Report VIN, boxcode, paid state.

Phase 3 — Live tune Stage 1 at E0 (KOEO; no human-loop confirmation
of engine response — engine isn't running).

  ASK ME ONCE before this phase:

    "About to apply live tune Stage 1, ethanol 0% (KOEO — engine
     stays off). The dongle will issue many UDS write-data
     commands to the ECU; the apply should complete inside 2 s.
     Candlelight will independently verify the writes hit the bus.
     Proceed? (y/n)"

  Wait for y. Then:
  - Sniff start.
  - Spin up a separate WS subscriber that captures all incoming
    events (apply_started, apply_progress, apply_completed,
    apply_failed, unload).
  - WS: {"command":"live_tune_start","params":{"stage":1,"ethanol_pct":0}}.
  - Wait up to 5 s for apply_completed event. Capture elapsed_ms,
    maps_applied, total_bytes_written.
  - If apply_failed instead — STOP, surface failure_reason, wait.
  - Sniff stop.
  - WS-side PASS: apply_completed received within 2000 ms.
  - CAN-side PASS: log contains many 0x7E0 frames with service byte
    0x2E (write data by identifier) OR 0x3D (write memory by
    address) — depends on which UDS service the orchestrator uses;
    confirm the corresponding positive response (0x6E or 0x7D) on
    0x7E8 for each.
  - Combined PASS if both. Report elapsed_ms, frame count on 0x7E0,
    frame count on 0x7E8.

Phase 4 — Live tune ethanol switch (KOEO).
  - Sniff start.
  - WS: {"command":"live_tune_set","params":{"stage":1,"ethanol_pct":50}}.
  - Wait for apply_completed. Capture elapsed_ms.
  - Sniff stop.
  - WS-side PASS: elapsed_ms < 2000.
  - CAN-side PASS: same shape as Phase 3 (write requests + positive
    responses).
  - Combined PASS if both.

Phase 5 — Live tune stop + state cleanup.
  - WS: {"command":"live_tune_stop"}. Expect success:true and
    unload event.
  - WS: {"command":"live_tune_status"}. Expect state IDLE.
  - PASS if both. (No CAN traffic expected during stop; the
    orchestrator drains its queue but doesn't necessarily issue UDS
    commands. Don't fail on a quiet bus here.)

Phase 6 — Feature manager arbitration (pure WS; no CAN-side check
needed — the manager is purely on-dongle).
  - WS: {"command":"wot_log_start"}. Expect success:true.
  - WS: {"command":"live_tune_start","params":{"stage":1,"ethanol_pct":0}}.
    Expect success:true (preempt swap).
  - WS: {"command":"live_tune_status"}. Expect state ACTIVE.
  - WS: {"command":"live_tune_stop"}.
  - PASS if all four succeed.

Phase 7 — WOT logger arm + disarm (KOEO; no actual capture possible).
  Note: at KOEO with no throttle input, the recorder stays in IDLE
  state — it ARMS but never transitions to RECORDING because wdkba
  (throttle position) never crosses the WOT threshold. We're
  validating that the start/stop commands work and the recorder
  arms cleanly. No log file will be produced; that's correct
  behavior for KOEO and not a failure.
  - WS: {"command":"wot_log_start"}. Expect success:true.
  - Wait 5 s.
  - WS: {"command":"wot_log_stop"}. Expect success:true.
  - PASS if both succeed AND no apply_failed / error events arrived
    during the 5 s window.
  - SKIP human-loop pulls — engine is off.

End of validation. Print the final report:

  Bench session — KOEO + Candlelight — YYYY-MM-DD HH:MM
  =====================================================
  Pre-flight + CAN baseline:  PASS / FAIL
  Flash + provision:          PASS / FAIL
  Phase 1 DTC:                PASS / FAIL — N read, M after clear,
                                            CAN: <frame counts>
  Phase 2 VIN/Lic:            PASS / FAIL — VIN <V>, boxcode <B>,
                                            paid:true, CAN: <frame counts>
  Phase 3 LiveTune:           PASS / FAIL — Nms, maps:M, bytes:B,
                                            CAN: writes:W, ACKs:A
  Phase 4 Eth shift:          PASS / FAIL — Nms, CAN: writes:W
  Phase 5 Stop:               PASS / FAIL — clean idle
  Phase 6 Arbitrate:          PASS / FAIL — preempt clean
  Phase 7 WOT arm/disarm:     PASS / FAIL — protocol-only (KOEO)

  Anomalies / observations:
   - <agent's notes from anything that surprised>

Append the report block to ~/esp/obd/status-2026-05-06.md under a
new ## Bench validation session (KOEO) — 2026-05-06 heading. Append
a one-line entry to ~/esp/obd/file-update-2026-05-06.md ("status log
updated with bench session report"). Do not commit.

Important notes for the agent:
- The CAN sniffer captures are diagnostic. If a phase's WS side
  PASSes but the CAN side has unexpected/missing frames, that's the
  most valuable kind of signal — flag it prominently in the report
  even if you mark the phase combined-FAIL. The two-sided check is
  the whole point.
- If the bus shows traffic on any ID OTHER than 0x7DF, 0x7E0,
  0x7E8 during a dongle command, STOP. Other IDs (especially in
  the 0x710 / 0x7E1–0x7E7 range) suggest a gateway scan or
  misaddressed request — exactly the failure mode CLAUDE.md's hard
  rule #1 warns about.
- All human-loop questions are simple y/n. Do not ask open-ended
  questions; if you need clarification, frame it as multiple-choice
  with your recommended answer first.
- If the dongle stops responding to WS commands at any phase, STOP,
  print "dongle unresponsive at phase N," capture the most recent
  10 s of CAN traffic to /tmp/futuner_bench_unresponsive.log, and
  wait for me.

Proceed.
```

---

## Notes for Sean

Two things flipped from the engine-running version:

The **"did you feel it?" questions are gone** — engine is off, so
behavioral confirmation doesn't apply. They're replaced with
**Candlelight CAN-frame verification**. For every phase that issues
UDS commands, the agent runs `tools/can_sniff.py` in the background
and asserts the right service IDs hit the bus. This is actually
*stronger* than "did you feel it" because it catches a class of
bugs ("the dongle thinks it sent the command but didn't") that
engine-feel can't.

The **WOT phase collapses to arm/disarm** — engine off means no
throttle, so the recorder arms but never transitions to RECORDING.
That's correct behavior, not a failure. The phase confirms the
start/stop WS commands work. To validate the actual capture path,
you'd need a separate session with engine running on a closed road
or track.

Three things that got added because Candlelight is on the bus:

**Phase 0 — CAN baseline.** Before any feature work, the agent
sniffs for 5 seconds while issuing one DTC read, and confirms
Candlelight saw frames on 0x7E0 and 0x7E8. If the bus is silent,
something's wrong before the real validation even starts (Y-splitter
loose, gas adapter not seated, dongle not transmitting). Catches
the dumbest failure modes first.

**Per-phase frame logs at `/tmp/futuner_bench_p<N>.log`.** Each
phase's CAN capture lands in a separate file. After the session you
have a complete record of every frame that crossed the bus,
correlated to the phase that produced it. Useful for post-mortem if
something behaves weirdly.

**Forbidden-ID detection.** If the agent sees traffic on any ID
other than 0x7DF / 0x7E0 / 0x7E8 during a dongle command, it stops.
That's the gateway-scan red flag CLAUDE.md's hard rule #1 warns
about — better to catch it once than wait through a 10-minute J533
lockout because something probed the wrong address.

The Candlelight is plugged in once, captures everything in
parallel, and you don't have to interact with it during the
session. The agent runs `can_sniff.py` as a subprocess and parses
the logs after each phase.

End-of-session report block lands in your status log automatically.
Scan it, file follow-ups for any FAIL or any "WS pass + CAN weird"
combination, and you'll have a clear picture of what graduated to
"validated on car KOEO" and what needs more work before engine-on
testing.
