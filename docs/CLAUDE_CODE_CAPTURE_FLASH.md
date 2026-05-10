# Claude Code — Capture MagicMotorsport Flash UDS Reference

> Captures wire-level CAN traffic during MagicMotorsport flashing
> the dev RS7. Re-flashes the **same binary that's already on the
> ECU** (safe — exercises full UDS pipeline without changing ECU
> behavior). Captures 5 cycles back-to-back so security-access seed
> rotation is visible. Output lands in
> `firmware/test/can_capture/fixtures/magicmotorsport/`.

---

## Hardware (you set up before pasting)

- Candlelight USB-CAN plugged into your Mac via USB.
- Candlelight connected to the OBD-II port via Y-splitter.
- Dev RS7: key in ON position, engine OFF.
- MagicMotorsport tool open in its window (separate app), ECU
  connected, ready to start a flash. Tool should be set to flash
  the SAME binary currently on the ECU (re-flash identical content).

That's all the physical setup. Paste below; the agent handles
everything else.

---

## Paste this into Claude Code

```
Capture wire-level CAN traffic during 5 MagicMotorsport flash
cycles. Re-flashing identical content (safe). Output:
firmware/test/can_capture/fixtures/magicmotorsport/.

I will click "flash" in MagicMotorsport when you tell me to and
press Enter when each flash finishes. Everything else is your job.
Don't write code, don't commit, don't reflash anything yourself.

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md
- ~/esp/obd/FUTV1.1/tools/can_sniff.py --help
- ~/esp/obd/FUTV1.1/docs/PHASE_2_PREREQUISITES.md (P-01)

Hard rules:
- CAN ID 0x7E0 only / 0x7E8 only. Any frame on any other ID
  outside the boot-up window — STOP, surface to me.
- If the bus shows persistent timeouts / NRC 0x10 / NRC 0x12 —
  J533 lockout pattern. STOP. Cycle off → 10+ min → on.

==========================================================
PHASE 1 — Pre-flight (silent; PASS/FAIL line per check)
==========================================================

  cd ~/esp/obd/FUTV1.1
  python3 -c "import gs_usb"                              (must succeed)
  tools/can_sniff.py --help                               (exit 0)
  mkdir -p firmware/test/can_capture/fixtures/magicmotorsport

  Baseline sniff — 5 s, no filter:
    tools/can_sniff.py > /tmp/baseline.log &
    sleep 5
    kill the sniffer
  Parse /tmp/baseline.log: must contain at least one frame on the
  bus (any ID). If empty → STOP, "Candlelight not seeing CAN
  traffic; check Y-splitter and key position."

  Detect any unexpected IDs in the baseline (anything other than
  0x7DF, 0x7E0, 0x7E8, plus normal vehicle bus chatter on other
  IDs which is fine — we only flag if we see something on
  0x710 or 0x7E1–0x7E7 which would suggest an active diagnostic
  scan from another tool).

If pre-flight fails, STOP and tell me which check.

==========================================================
PHASE 2 — Capture loop (5 cycles)
==========================================================

For cycle N from 1 to 5:

  Print: "READY for cycle $N of 5. Click START in MagicMotorsport
          to begin the flash. I'll start capturing now. Press
          Enter when MagicMotorsport reports the flash is
          complete."

  Start sniffer in background, filtering 0x7E0 + 0x7E8:
    tools/can_sniff.py --filter 0x7E0 0x7E8 \
        > firmware/test/can_capture/fixtures/magicmotorsport/flash_run_${N}_4K0907557G_0003.candump &

  Wait for me to press Enter.

  Stop the sniffer (kill the background process cleanly).

  Parse the captured log:
    - Count frames on 0x7E0
    - Count frames on 0x7E8
    - List unique UDS service IDs seen on 0x7E0 (request bytes
      after ISO-TP header)
    - Note any negative responses (0x7F service code) and their
      NRCs

  Write a per-cycle notes file:
    firmware/test/can_capture/fixtures/magicmotorsport/flash_run_${N}_4K0907557G_0003.notes.md
  Contents:
    - Cycle N timestamp
    - Total frames captured
    - 0x7E0 / 0x7E8 frame counts
    - Unique services observed
    - Any NRCs (with explanation)
    - File hash (sha256 of the candump file)

  Print: "Cycle $N captured. Frames: <N0x7E0> on 0x7E0,
          <N0x7E8> on 0x7E8. Continuing to cycle $((N+1))."

==========================================================
PHASE 3 — Cross-cycle analysis
==========================================================

After all 5 cycles, parse all 5 candumps together:

  - Compare cycle 1's security access seed (UDS service 0x27)
    against cycles 2..5. Seeds MUST differ between sessions
    (rotation is intentional).
  - Compare TransferData (0x36) byte counts between cycles.
    Should be similar (within ±1%) since same binary.
  - Confirm every cycle ends with ECU reset (0x11) + clean
    return-to-default-session.

Write a cross-cycle summary at:
  firmware/test/can_capture/fixtures/magicmotorsport/SUMMARY.md

==========================================================
PHASE 4 — Report
==========================================================

Print:

  MagicMotorsport capture — YYYY-MM-DD HH:MM
  ===========================================
  Pre-flight:        PASS
  Cycle 1:           PASS — N0x7E0 reqs, N0x7E8 resps, services <list>
  Cycle 2:           PASS — ...
  Cycle 3:           PASS — ...
  Cycle 4:           PASS — ...
  Cycle 5:           PASS — ...
  Seed rotation:     CONFIRMED (5 distinct seeds across cycles)
  Bytes-transferred: <range>, mean <X> bytes per cycle
  
  Anomalies:
   - <anything that surprised you across cycles>

  Output files:
    firmware/test/can_capture/fixtures/magicmotorsport/flash_run_1..5_4K0907557G_0003.candump
    firmware/test/can_capture/fixtures/magicmotorsport/flash_run_1..5_4K0907557G_0003.notes.md
    firmware/test/can_capture/fixtures/magicmotorsport/SUMMARY.md

Append the report block to ~/esp/obd/status-2026-05-07.md (today's
log; create if missing) under a new "## P-01 capture session"
heading.

Hand back. Don't commit.

Proceed.
```

---

## What the agent does vs. what you do

| Step | Who |
|---|---|
| Hardware setup (Candlelight, Y-splitter, key on) | You (before paste) |
| MagicMotorsport configured to flash same binary | You (before paste) |
| `python3` deps check, `can_sniff.py` invocation, log files, parsing | Agent |
| "Click flash" prompt timing | Agent prompts; you click in MM |
| Per-cycle file naming, hashing, notes | Agent |
| Cross-cycle seed rotation analysis | Agent |
| Final report + status-log append | Agent |

The only times you act during execution: click "flash" in
MagicMotorsport when the agent says "READY for cycle N", and press
Enter in the Claude Code window when each flash finishes. Five
clicks + five Enters total.

If anything looks wrong (gateway lockout pattern, dongle on the bus
when it shouldn't be, NRC 0x33 from the ECU rejecting MM's
authentication), the agent halts and surfaces. Copy the chat output
back here.
