# Claude Code — Validate Dongle (in-car, KOEO)

> Paste this into Claude Code on Sean's Mac. Dongle is plugged into the
> car's OBD-II port, Candlelight USB-CAN plugged in alongside, key on /
> engine off. Dongle is already flashed and provisioned. The agent
> connects to the dongle's WebSocket, runs every Phase 1 command in
> sequence, captures CAN frames in parallel via the Candlelight, and
> reports what worked.
>
> **Set two env vars before pasting** (these replace the interactive
> prompts, so the agent runs hands-off):
>
> ```
> export DONGLE_HOST='192.168.10.1'   # or the STA IP if on Wi-Fi
> export ADMIN_API_KEY='your-admin-key'
> ```

---

## Paste this into Claude Code

```
Validate the dongle. It's plugged into the car (OBD-II), Candlelight
USB-CAN is plugged in alongside, key on, engine off. Dongle is
already flashed and provisioned. Run every Phase 1 WS command in
sequence, capture matching CAN frames via the Candlelight, report
what worked.

Inputs from the shell (DO NOT ASK ME):
- DONGLE_HOST env var = dongle's IP. WS URL is ws://$DONGLE_HOST/ws.
- ADMIN_API_KEY env var = needed only if a phase's failure mode
  requires hitting the cloud admin endpoint (it shouldn't for
  validation). If ADMIN_API_KEY is unset, continue anyway.

If DONGLE_HOST is unset OR the WS URL is unreachable in 5 s — STOP,
print "DONGLE_HOST unset or WS unreachable; export DONGLE_HOST and
re-run", do nothing else. Do not ask me interactively.

Don't write code, don't commit anything, don't reflash, don't
reprovision.

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md
- ~/esp/obd/FUTV1.1/firmware/src/commands/commands.c
- ~/esp/obd/FUTV1.1/firmware/src/sbf/sbf_orchestrator.c (event shapes)
- ~/esp/obd/FUTV1.1/tools/can_sniff.py --help

Hard rules:
- CAN ID 0x7E0 only (request) / 0x7E8 only (response). Any frame on
  any other ID during a dongle command — STOP, surface to me.
- If the dongle stops responding at any phase — STOP, surface,
  capture last 10 s of CAN to /tmp/futuner_unresponsive.log, wait.
- If the ECU returns NRC 0x10 / 0x12 or persistent timeouts on
  0x7E0 — that's a J533 lockout pattern. STOP. Tell me. Do not
  retry. Cycle off → 10+ min → on is the only fix.

Pre-flight (silent, single PASS/FAIL line per check):
1. echo $DONGLE_HOST                 (must be non-empty)
2. python3 -c "import websockets"    (WS client lib present)
3. python3 -c "import gs_usb"        (Candlelight lib present)
4. tools/can_sniff.py --help         (must exit 0)
5. Open ws://$DONGLE_HOST/ws and send {"command":"get_status"};
   expect a JSON response within 5 s.

If any pre-flight fails: print which one and STOP.

If pre-flight passes, run all seven phases without stopping for
confirmation. For each phase: start tools/can_sniff.py
--filter 0x7E0 0x7E8 in the background capturing to
/tmp/futuner_p<N>.log, run the WS command(s), stop the sniffer,
parse the log for expected service IDs.

Combined PASS only if WS response is correct AND CAN saw the
expected request/response shape.

  P1 — DTC read+clear (UDS 0x19, 0x14)
    WS: dtc_read              expect success:true, dtcs[]
    WS: dtc_clear             expect success:true
    WS: dtc_read              expect success:true (post-clear)
    CAN: 0x19 + 0x14 services on 0x7E0; 0x59 + 0x54 on 0x7E8

  P2 — VIN pair + license (UDS 0x22 PID 0x0902)
    WS: vin_pair_now          expect success:true, "VIN paired"
    WS: license_status        expect paid:true, vin populated
    CAN: 0x22 on 0x7E0; 0x62 on 0x7E8 carrying VIN

  P3 — Live tune apply at stage 1, ethanol 0 (KOEO)
    Subscribe to events first (apply_started/progress/completed/failed/unload)
    WS: live_tune_start {stage:1, ethanol_pct:0}
    Wait up to 5 s for apply_completed
    Capture elapsed_ms, maps_applied, total_bytes_written
    PASS if elapsed_ms < 2000
    CAN: many writes on 0x7E0 (service 0x2E or 0x3D), ACKs on 0x7E8

  P4 — Ethanol shift to 50%
    WS: live_tune_set {stage:1, ethanol_pct:50}
    Wait for apply_completed
    PASS if elapsed_ms < 2000
    CAN: same shape as P3

  P5 — Stop and unload
    WS: live_tune_stop        expect success:true, unload event
    WS: live_tune_status      expect state IDLE

  P6 — Feature manager arbitration
    WS: wot_log_start         expect success:true
    WS: live_tune_start {stage:1, ethanol_pct:0}    expect success:true (preempt)
    WS: live_tune_status      expect ACTIVE
    WS: live_tune_stop

  P7 — WOT logger arm/disarm (no capture possible KOEO — protocol only)
    WS: wot_log_start         expect success:true
    Wait 5 s
    WS: wot_log_stop          expect success:true

After all 7, print:

  Validation — YYYY-MM-DD HH:MM
  ===============================
  Pre-flight:    PASS
  P1 DTC:        PASS / FAIL — N read, M after clear, CAN: <counts>
  P2 VIN/Lic:    PASS / FAIL — VIN <V>, paid:<true|false>, CAN: <counts>
  P3 LiveTune:   PASS / FAIL — Nms, maps:M, bytes:B, CAN: writes:W
  P4 Eth shift:  PASS / FAIL — Nms, CAN: writes:W
  P5 Stop:       PASS / FAIL
  P6 Arbitrate:  PASS / FAIL — preempt clean
  P7 WOT arm:    PASS / FAIL

  Anomalies:
   - <anything that surprised you>

Append the report block to ~/esp/obd/status-2026-05-07.md (today's
log; create if missing) under a new "## Validation" heading.

Hand back. Don't commit.

Proceed.
```

---

## Notes

The prompt is hands-off after paste — it pulls everything it needs
from the env vars you set in the same shell before pasting, runs all
seven phases, and prints a report. The only times it stops asking
nothing are when something is genuinely wrong (dongle unresponsive,
gateway-lockout pattern, weird CAN ID on the bus). In those cases it
halts and surfaces; you copy the chat output back here and we figure
out next move.

If the dongle's at AP-mode IP (192.168.10.1) and not yet on STA
Wi-Fi, P2's `vin_pair_now` will fail because there's no internet
route to the cloud. Either move the dongle to STA first (load the
AP page, enter Wi-Fi creds), or accept that P2 will FAIL with a
clear "no STA route" error — the rest of the validation still runs.

P3 and P4 fire UDS write commands at the ECU's RAM; KOEO is the
safe configuration for that (engine off when rev limit toggles
mid-apply). Don't run with engine started — that's the safety case
MISSION_SPEC §4.5 calls out.
