# Claude Code — Full sequence: flash + provision + validate

> One paste. The agent builds firmware, flashes the dongle, enrolls
> it on the cloud, sets its auth token, joins it to your Wi-Fi,
> pairs the VIN, marks paid, uploads the SBF, then validates every
> Phase 1 feature. KOEO mode (key on, engine off). Candlelight USB-CAN
> plugged in alongside.
>
> No interactive questions. No "did you feel it?" The agent halts
> only if something genuinely breaks.

---

## Step 0 — set these env vars in your Terminal first (one-time)

```
export ADMIN_API_KEY='your-admin-api-key'
export STA_SSID='your-home-or-hotspot-ssid'
export STA_PASS='your-wifi-password'
```

Optional override (only if the dongle's AP IP isn't the default):

```
export AP_HOST='192.168.4.1'   # default, override only if customized
```

That's it. Now paste the prompt below.

---

## Paste this into Claude Code

```
Full sequence — flash dongle, provision it on cloud + WS, validate
every Phase 1 feature. KOEO. No interactive pauses. Don't write code,
don't commit, don't ask me anything unless something is genuinely
wrong.

Inputs from shell (read at start, don't ask me):
- ADMIN_API_KEY (cloud admin auth)
- STA_SSID, STA_PASS (Wi-Fi credentials for the dongle's STA mode)
- AP_HOST (defaults to 192.168.4.1; the dongle's AP-mode IP)

If any required env var is unset → STOP, print which one is missing
("export <NAME> and re-run"), do nothing else.

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md
- ~/esp/obd/FUTV1.1/docs/upload2server.md
- ~/esp/obd/FUTV1.1/firmware/src/commands/commands.c
- ~/esp/obd/FUTV1.1/firmware/build.sh
- ~/esp/obd/FUTV1.1/firmware/flash.sh
- ~/esp/obd/FUTV1.1/tools/can_sniff.py --help

Hard rules:
- CAN ID 0x7E0 only / 0x7E8 only.
- If the ECU returns NRC 0x10 / 0x12 or persistent 0x7E0 timeouts —
  J533 lockout pattern. STOP, tell me. Cycle off → 10+ min → on.
- If anything looks wrong, STOP and surface. Do not retry blind.

==========================================================
PHASE A — Pre-flight (silent; PASS/FAIL line per check)
==========================================================

  cd ~/esp/obd/FUTV1.1
  firmware/test/verify_frozen.sh                        (must PASS)
  ls /dev/cu.usbmodem*                                  (exactly one)
  echo $ADMIN_API_KEY | wc -c                           (>1)
  echo $STA_SSID | wc -c                                (>1)
  python3 -c "import gs_usb"                            (must succeed)
  curl -fsS https://sillyrabbitmotorsport.com/fut/health (must 200)

If any fails, STOP and print which one.

==========================================================
PHASE B — Build + flash firmware
==========================================================

  PORT=$(ls /dev/cu.usbmodem* | head -1)
  cd ~/esp/obd/FUTV1.1/firmware
  source ~/esp/esp-idf/export.sh   (or detect equivalent activation)
  ./build.sh                       (expect "Project build complete")
  ./flash.sh -p $PORT              (expect "Hard resetting via RTS pin")

Then tail the serial port for ≤30 s using pyserial; capture the line:
  I (NVS) device MAC: AA:BB:CC:DD:EE:FF
Save that MAC.

If MAC scrape fails, STOP. Tell me.

==========================================================
PHASE C — Cloud-side enrollment (one curl chain)
==========================================================

  curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
       -H "Content-Type: application/json" \
       -d "{\"mac\":\"$MAC\"}" \
       https://sillyrabbitmotorsport.com/fut/admin/devices

Capture auth_token from response. If response is 409
(already enrolled), GET /admin/devices and reuse the existing
token for that MAC.

==========================================================
PHASE D — Push token + Wi-Fi via dongle's AP-mode WS
==========================================================

The dongle is at ws://$AP_HOST/ws on first boot.

  WS: unlock                 (admin password — empty by default; if
                              the dongle is password-locked I'll know)
  WS: set_auth_token {token}
  WS: wifi_connect {ssid:$STA_SSID, password:$STA_PASS}

Poll wifi_status every 2 s for up to 30 s; capture sta_ip when
state is connected. From here on, WS URL is ws://$STA_IP/ws.

If STA never connects, STOP. Tell me.

==========================================================
PHASE E — VIN pair + license paid + SBF assign (cloud + WS)
==========================================================

  WS (at $STA_IP): vin_pair_now
       (expect success:true, "VIN paired")

  curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
       -H "Content-Type: application/json" \
       -d '{"paid":1}' \
       https://sillyrabbitmotorsport.com/fut/admin/devices/$MAC/license

  curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
       -F "file=@$HOME/esp/obd/FUTV1.1/sbf/stage1_patched.sbf" \
       "https://sillyrabbitmotorsport.com/fut/admin/calibrations/stage1_patched.sbf?boxcode=4K0907557G__0003"

  curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
       "https://sillyrabbitmotorsport.com/fut/admin/devices/$MAC/assign_calibration?filename=stage1_patched.sbf"

  WS: vin_pair_now    (refresh license cache; expect paid:true now)
  WS: license_status  (verify paid:true, vin populated)

==========================================================
PHASE F — Validate Phase 1 features (7 sub-phases)
==========================================================

For each sub-phase, capture CAN frames in parallel:
  tools/can_sniff.py --filter 0x7E0 0x7E8 → /tmp/futuner_p<N>.log
Run WS commands, stop sniffer, parse log for expected service IDs.
PASS only if WS response correct AND CAN saw expected frames.

  P1 — DTC (UDS 0x19, 0x14)
       WS: dtc_read, dtc_clear, dtc_read
       CAN: 0x19 + 0x14 on 0x7E0; 0x59 + 0x54 on 0x7E8

  P2 — VIN read (UDS 0x22 PID 0x0902) — already exercised in Phase E
       Re-run license_status; mark PASS if paid still true

  P3 — Live tune apply, stage 1, ethanol 0
       Subscribe events (apply_started/progress/completed/failed/unload)
       WS: live_tune_start {stage:1, ethanol_pct:0}
       Wait ≤5 s for apply_completed
       PASS if elapsed_ms < 2000
       CAN: many writes (0x2E or 0x3D) on 0x7E0; ACKs on 0x7E8

  P4 — Ethanol shift to 50%
       WS: live_tune_set {stage:1, ethanol_pct:50}
       Wait for apply_completed; PASS if elapsed_ms < 2000

  P5 — Stop and unload
       WS: live_tune_stop, live_tune_status (expect IDLE)

  P6 — Feature manager arbitration
       WS: wot_log_start, live_tune_start, live_tune_status (ACTIVE),
           live_tune_stop

  P7 — WOT logger arm/disarm (KOEO; protocol-only, no real capture)
       WS: wot_log_start, wait 5 s, wot_log_stop

==========================================================
PHASE G — Report
==========================================================

Print:

  Validation — YYYY-MM-DD HH:MM
  ===============================
  Phase A pre-flight:  PASS
  Phase B flash:       PASS — build.sh OK, MAC=$MAC
  Phase C cloud:       PASS — token saved
  Phase D Wi-Fi:       PASS — STA at $STA_IP
  Phase E VIN/Lic/SBF: PASS — VIN $VIN, paid:true, SBF assigned
  P1 DTC:              PASS / FAIL — N read, M after clear
  P2 VIN/Lic:          PASS / FAIL
  P3 LiveTune:         PASS / FAIL — Nms, maps:M
  P4 Eth shift:        PASS / FAIL — Nms
  P5 Stop:             PASS / FAIL
  P6 Arbitrate:        PASS / FAIL
  P7 WOT arm:          PASS / FAIL

  Anomalies:
   - <anything that surprised you>

Append the report to ~/esp/obd/status-2026-05-07.md (today's log;
create if missing) under "## Validation".

Hand back. Don't commit.

Proceed.
```

---

## What you do, in order

1. Open Terminal on your Mac.
2. Run the four `export` lines from Step 0 above (with your real
   admin key, SSID, and password).
3. Open Claude Code in that same Terminal.
4. Paste the prompt body (everything inside the code fence).
5. Walk away. Come back when it's done.

The agent will halt and tell you only if something genuinely breaks
(can't reach the cloud, dongle stops responding, ECU lockout pattern,
weird CAN traffic). Otherwise it runs all of A through G hands-off
and the report lands in today's status log.

If anything in the report says FAIL, copy the chat output back here
and we'll diagnose.
