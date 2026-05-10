# Claude Code — Flash Dongle Only

> Focused mini-project: build the firmware, flash it to the dongle
> over USB, watch the boot log, confirm it booted cleanly. No cloud,
> no Wi-Fi, no provisioning, no validation phases. Just "did the
> dongle take the firmware and come up healthy."
>
> No env vars needed. No interactive questions. The agent auto-detects
> the USB serial port and runs the build + flash + boot-watch sequence.

---

## Hardware needed

- **Dongle** plugged into your Mac via USB-C.
- That's it. No car, no OBD-II port, no Candlelight, no Wi-Fi, no
  cloud server. Just the dongle on a desk, connected to your Mac.

You can do this anywhere — at your desk, on the kitchen table,
sitting in a coffee shop with the dongle in your bag. As long as the
USB cable is connected.

---

## Paste this into Claude Code

```
Flash the dongle. Build firmware, flash it over USB, watch the boot
log for ~15 s, confirm it came up healthy. No cloud, no Wi-Fi, no
provisioning. Don't write code, don't commit, don't validate
anything beyond "did boot succeed."

Auto-detect: USB serial port at /dev/cu.usbmodem*. If exactly one is
present, use it. If zero or more than one, STOP and tell me.

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md
- ~/esp/obd/FUTV1.1/firmware/build.sh
- ~/esp/obd/FUTV1.1/firmware/flash.sh
- ~/esp/obd/FUTV1.1/firmware/monitor.sh

==========================================================
PHASE 1 — Pre-flight (silent; PASS/FAIL line per check)
==========================================================

  cd ~/esp/obd/FUTV1.1
  firmware/test/verify_frozen.sh                  (must PASS)
  test -f ~/esp/esp-idf/export.sh                 (ESP-IDF installed)
  ls /dev/cu.usbmodem*                            (exactly one path)
  python3 -c "import serial"                      (pyserial present;
                                                   if not, install:
                                                   python3 -m pip
                                                   install --user
                                                   pyserial)

If any pre-flight fails, STOP and print which.

Save the detected port:
  PORT=$(ls /dev/cu.usbmodem* | head -1)

==========================================================
PHASE 2 — Build firmware
==========================================================

  cd ~/esp/obd/FUTV1.1/firmware
  source ~/esp/esp-idf/export.sh
  ./build.sh

Stream output. PASS if exit code 0 AND "Project build complete"
appears in stdout. FAIL otherwise — capture last 50 lines and STOP.

==========================================================
PHASE 3 — Flash
==========================================================

  ./flash.sh -p $PORT

Stream output. PASS if exit code 0 AND "Hard resetting via RTS pin"
appears. FAIL otherwise — capture last 50 lines and STOP.

==========================================================
PHASE 4 — Watch boot for 15 s
==========================================================

Open $PORT at 115200 baud via pyserial. Read for 15 s. Capture all
output to /tmp/futuner_first_boot.log.

Look for these key lines (in this order):
  - "ESP-ROM:" or "rst:0x..." (boot ROM started)
  - "I (FEATURE_MGR)" — feature_manager initialized
  - "I (LICENSE)" — license module initialized
  - "I (VIN_PAIR)" — vin_pairing module initialized
  - "I (SBF)" — sbf orchestrator initialized
  - "I (WIFI)" — wifi started (likely AP mode on first boot)
  - The dongle's MAC: line "device MAC: AA:BB:CC:DD:EE:FF" or similar

Don't fail if lines are missing — different builds may have different
init order. Just capture what's seen.

==========================================================
PHASE 5 — Report
==========================================================

Print:

  Flash — YYYY-MM-DD HH:MM
  =========================
  Pre-flight:    PASS — port=$PORT, IDF present
  Build:         PASS — Project build complete
  Flash:         PASS — Hard resetting via RTS pin
  Boot:          PASS / PARTIAL / FAIL
                 (PASS if all 5 init lines seen,
                  PARTIAL if some seen,
                  FAIL if no init lines at all)
  
  Init lines seen:
    [list each one observed]
  
  MAC: AA:BB:CC:DD:EE:FF (if scraped)
  
  Anomalies:
   - <anything weird in the boot log: panics, asserts, crashes,
      unexpected resets, etc.>

Save the full boot log at /tmp/futuner_first_boot.log for later.

Hand back. Don't commit.

Proceed.
```

---

## What you do

1. Plug the dongle into your Mac via USB-C.
2. Open Claude Code in Terminal (any directory; the prompt cd's into
   the repo itself).
3. Paste the prompt body (everything inside the code fence).
4. Watch the output. Should take 2–4 minutes total (build is the
   slow part).

If Phase 1 fails on `ls /dev/cu.usbmodem*`, the dongle isn't being
recognized over USB. Check the cable (some USB-C cables are charge-
only; you need a data cable). Try a different USB port.

If Phase 2 fails on `Project build complete`, the firmware doesn't
compile. Copy the last 50 lines of build output back here and we'll
diagnose.

If Phase 3 fails on `Hard resetting via RTS pin`, esptool didn't
connect to the dongle's bootloader. Usually a button-hold issue
(some boards need BOOT held while you plug in). Or wrong baud.

If Phase 4 returns FAIL or PARTIAL, the dongle is running but not
booting cleanly — copy `/tmp/futuner_first_boot.log` back here.
That's the most useful diagnostic for "the silicon's alive but
something's wrong with the firmware."

If Phase 5 prints PASS for all four phases, **the dongle is flashed
and booting healthy.** That's the proof point this mini-project is
trying to establish. From here you can decide what to add next —
cloud connectivity, provisioning, validation — knowing the
firmware-to-silicon path itself works.

---

## What this prompt explicitly does NOT do

- No cloud anything (no rsync, no enrollment, no license, no SBF).
- No Wi-Fi anything (dongle defaults to AP mode after flash; that's
  fine, we're not exercising STA).
- No CAN traffic (no Candlelight, no UDS).
- No WS commands (the WS server starts on the dongle but we don't
  connect to it).
- No git changes, no commits.

Pure silicon-up validation. Once this passes, you have a known-good
flashed dongle. Everything else builds on that.
