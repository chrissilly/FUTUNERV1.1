# Claude Code (Windows session) — Comprehensive UI + WS + CAN Test

> Self-contained prompt for a Claude Code session running on a
> Windows PC, started by another user in Sean's org who has no prior
> context on this project. The agent reads the project docs first,
> understands the constraints, exercises the dongle's full WS command
> surface with parallel Candlelight CAN-frame verification, and
> reports back comprehensively.
>
> No interactive questions. No destructive operations. No commits.

---

## Hardware state assumed before the user pastes

- Dongle is in AP mode at `192.168.10.1`. Windows PC is connected to
  the dongle's AP (one of its two NICs). DHCP-assigned the Windows
  side an IP from the dongle.
- Candlelight USB-CAN adapter is plugged into the Windows PC via USB.
- Candlelight is wired to the OBD-II port via Y-splitter, sharing
  the bus with the dongle.
- Dev car: key in ON position, engine OFF (KOEO). The C8 J533
  gateway puts the powertrain CAN segment to sleep when idle; a
  TesterPresent on 0x7E0 wakes it. The agent will issue a periodic
  TesterPresent keep-alive during the test to prevent re-sleep.
- Repo cloned at `C:\Users\<user>\esp\obd\FUTV1.1\` (or equivalent
  on this user's machine — agent uses `cd` relative to the repo
  root it can locate).

---

## Paste this into Claude Code

```
You are continuing FUTUNER (an aftermarket ECU tuning dongle for
Bosch MG1/MDG1/MED17 ECUs over UDS/ISO-TP via CAN). I'm in another
user's account in our org, so you have no prior context. Read this
prompt fully before running anything. Don't ask me clarifying
questions — every choice is pre-decided below.

OBJECTIVE
Validate every read-only WebSocket command exposed by the dongle's
firmware while independently verifying on the CAN bus via the
Candlelight USB-CAN adapter. Two-sided check per command: WS
response correct AND CAN traffic matches expectation (or correct
silence). Report back comprehensively. Do NOT fire destructive
commands. Do NOT modify firmware, cloud, or UI source. Do NOT
commit anything.

==========================================================
PROJECT CONTEXT (read in this order before doing anything)
==========================================================

  1. <repo_root>/CLAUDE.md                                — project
     rules. Binding. Includes hard rules on CAN bus discipline
     (CAN ID 0x7E0 only; never 0x7DF, 0x710, 0x7E1–0x7E7 — those
     trigger 10+ minute J533 gateway lockouts).
  2. <repo_root>/docs/SESSION_HANDOFF.md                  — session
     ground truth. What's shipped, what's frozen, what's in flight.
  3. <repo_root>/firmware/src/commands/commands.c         — the
     dongle's COMMAND_REGISTRY. ~40 commands. Read this to enumerate
     what to test.
  4. <repo_root>/firmware/src/sbf/sbf_orchestrator.c      — the
     event payload shapes the dongle broadcasts (apply_started,
     apply_progress, apply_completed, apply_failed, unload).
  5. <repo_root>/tools/can_sniff.py                       — the
     Candlelight CAN-frame capture utility. Reading the source is
     fine; the API is `--filter <hex IDs>` and it writes a parseable
     log of timestamped frames.

INFER THE REPO ROOT: try in this order
  - $env:USERPROFILE\esp\obd\FUTV1.1
  - $env:USERPROFILE\projects\FUTV1.1
  - C:\esp\obd\FUTV1.1
  - the current working directory if it contains CLAUDE.md and a
    firmware\ subfolder
If none of these contain CLAUDE.md, STOP and say
"FUTV1.1 repo not found at expected paths."

==========================================================
INPUTS (read from env at start; do NOT ask user)
==========================================================

  DONGLE_HOST  default: 192.168.10.1   (the dongle's AP IP)
  DONGLE_PORT  default: 80             (HTTP/WS port)
  REPORT_DIR   default: <repo_root>\..\test_reports
                                       (created if missing)

If the user has set any of these via PowerShell `$env:NAME='value'`
before starting Claude Code, use those values. Otherwise the
defaults above. Do not error on unset env — defaults are sufficient.

==========================================================
HARD RULES (binding throughout)
==========================================================

- CAN ID 0x7E0 only (request) / 0x7E8 only (response). Any frame on
  any other ID during a dongle command — STOP, capture last 10 s of
  CAN to <REPORT_DIR>\anomaly_canbus.log, surface to me.
- If the ECU returns NRC 0x10 / 0x12 or persistent timeouts on
  0x7E0 — that's a J533 lockout pattern. STOP, write
  <REPORT_DIR>\anomaly_lockout.log, surface immediately. Do not
  retry. The fix is ignition off → 10+ min wait → ignition on.
- If the dongle stops responding to WS commands — STOP, capture
  <REPORT_DIR>\anomaly_dongle_unresponsive.log, surface.
- DO NOT fire any destructive command (full list in PHASE 3 below).
- DO NOT modify firmware/src/, firmware/test/, cloud/, ui/, or any
  frozen module.
- DO NOT make any git commit.

==========================================================
ARCHITECTURE — ONE Python process owns the Candlelight
==========================================================

There must be exactly ONE Python process accessing the Candlelight
at any time (gs_usb / libusb does not allow concurrent access).
Inside that process:

  Thread A (TesterPresent keep-alive)
    Sends `02 3E 00 00 00 00 00 00` on CAN ID 0x7E0 every 2 s for
    the entire duration of phases 3 + 4. Prevents J533 sleep.

  Thread B (continuous frame capture)
    Reads every frame the Candlelight observes into an in-memory
    ring buffer with timestamps.

  Main thread (test driver)
    For each command in PHASE 3, marks a start timestamp, sends the
    WS command, awaits response, marks an end timestamp, then
    extracts the frames from buffer B that fall between those
    timestamps and writes them to a per-command log:
      <REPORT_DIR>\frames_<cmd>.log

Do NOT spawn `tools/can_sniff.py` as a subprocess per command —
that conflicts with the keep-alive. Adapt the can_sniff.py logic
into the same Python process.

==========================================================
PHASE 1 — Pre-flight (silent; one PASS/FAIL line per check)
==========================================================

  python -c "import websockets, asyncio, json"        (libs present)
  python -c "import gs_usb"                           (Candlelight lib;
                                                       if missing, run
                                                       `python -m pip
                                                       install --user
                                                       gs_usb`)
  Test WS reachability: open ws://$DONGLE_HOST:$DONGLE_PORT/ws and
  send {"command":"get_status"}; expect a JSON response within 5 s.
  Detect Candlelight: gs_usb.GsUsb.scan() must return ≥1 device.

If any pre-flight fails: write <REPORT_DIR>\preflight_fail.log with
the failure detail, STOP. Do not continue.

==========================================================
PHASE 2 — Start keep-alive + capture threads
==========================================================

Start Thread A (KA) and Thread B (capture). Confirm the first
TesterPresent goes out and a positive response arrives on 0x7E8 —
this proves the bus is awake before walking the WS surface.

If no 0x7E8 response within 5 s of the first TesterPresent: STOP,
log to <REPORT_DIR>\anomaly_bus_silent.log, surface.

==========================================================
PHASE 3 — Exercise every read-only WS command with two-sided check
==========================================================

For each command below, in order:

  1. Mark start_ts (monotonic clock).
  2. Send the WS request via the open WS connection. Capture the
     full JSON response and its arrival timestamp.
  3. Wait an additional 1 s for any trailing CAN traffic.
  4. Mark end_ts.
  5. Extract frames from Thread B's buffer where
     start_ts ≤ frame.ts ≤ end_ts. Filter to ONLY 0x7E0 / 0x7E8.
     Write to <REPORT_DIR>\frames_<cmd>.log in a parseable format
     (one frame per line: timestamp_ms, id_hex, dlc, hex_bytes).
  6. Compute combined verdict:
       PASS    — WS response success:true AND CAN matches expectation
                 (commands marked "CAN: none expected" should see
                 only the keep-alive frames during the window;
                 strip those from the count by ID + payload match).
       PARTIAL — WS success but CAN traffic unexpected (extra IDs,
                 unexpected service codes, NRCs).
       FAIL    — WS error (success:false) OR CAN shows a forbidden
                 ID; in the latter case, halt per Hard Rules.

Safe (read-only) commands and CAN expectations:

  get_status               CAN: none (excluding KA frames)
  list_commands            CAN: none
  license_status           CAN: none
  wifi_status              CAN: none
  flex_status              CAN: none
  can_sniff_status         CAN: none
  live_tune_status         CAN: none
  list_available_vars      CAN: none
  get_logger_profile       CAN: none
  get_logger_data          CAN: 0x22 (read by ID) on 0x7E0;
                                positive 0x62 on 0x7E8
  get_errors               CAN: none
  fs_info                  CAN: none
  fs_list /                CAN: none
  fs_list /storage         CAN: none
  fs_list /storage/sbf     CAN: none
  fs_list /storage/wot     CAN: none
  dtc_read                 CAN: 0x19 0x02 on 0x7E0;
                                positive 0x59 on 0x7E8

Forbidden during this test (DO NOT FIRE):
  dtc_clear, clear_errors, fs_write, fs_delete, fs_mkdir,
  wot_log_start, wot_log_stop,
  live_tune_start, live_tune_set, live_tune_stop,
  flex_load_scal, flex_unload_scal, flex_enable, flex_disable,
  flex_set_override,
  can_sniff_start, can_sniff_stop, can_send_raw, write_ecu,
  pair_ecu, remove_pairing,
  set_auth_token, vin_pair_now,
  set_logger_profile, delete_logger_profile, configure_logger,
  logger_start, logger_stop, reboot

==========================================================
PHASE 4 — 30 s async event window
==========================================================

After PHASE 3, keep the WS connection open and the capture thread
running for 30 seconds. Capture:

  - Any WS messages with an "event" field (apply_started,
    apply_progress, apply_completed, apply_failed, unload, can_frame,
    or any new event we haven't seen). Should normally be quiet.
  - Any CAN frames in the window other than the keep-alive frames.

Write captured events to <REPORT_DIR>\async_events.log and frames
to <REPORT_DIR>\async_frames.log.

==========================================================
PHASE 5 — Stop keep-alive cleanly
==========================================================

Signal Thread A to exit, signal Thread B to exit, wait for both
to join (≤2 s each). Close the WS connection.

==========================================================
PHASE 6 — Comprehensive report
==========================================================

Write <REPORT_DIR>\REPORT.md with the following structure (use
real values from the test, not placeholders):

  # Dongle UI / WS / CAN test — YYYY-MM-DD HH:MM:SS

  ## Environment
  - DONGLE_HOST: 192.168.10.1 (etc.)
  - Repo root: C:\Users\...\FUTV1.1
  - Python: <version>
  - gs_usb: <version>
  - websockets: <version>

  ## Pre-flight: PASS

  ## Bus wake: PASS — TesterPresent at <ts>, response 0x7E8 at <ts+Nms>

  ## Per-command results
  | Command              | WS verdict | CAN verdict   | Elapsed | Notes |
  |----------------------|------------|---------------|---------|-------|
  | get_status           | PASS       | CLEAN         | 12 ms   | VIN=<>, fw=<>, mac=<>, ip=<>, uptime=<> |
  | list_commands        | PASS       | CLEAN         | 8 ms    | N commands registered |
  | license_status       | PASS       | CLEAN         | 4 ms    | paid=<>, revoked=<>, vin=<> |
  | wifi_status          | PASS       | CLEAN         | 5 ms    | sta_state=<>, sta_ip=<>, ap_active=<> |
  | flex_status          | PASS       | CLEAN         | ...     | |
  | can_sniff_status     | PASS       | CLEAN         | ...     | |
  | live_tune_status     | PASS       | CLEAN         | ...     | state=<> |
  | list_available_vars  | PASS       | CLEAN         | ...     | N vars |
  | get_logger_profile   | PASS       | CLEAN         | ...     | N slots |
  | get_logger_data      | PASS       | 0x22→0x62     | ...     | sample frame |
  | get_errors           | PASS       | CLEAN         | ...     | |
  | fs_info              | PASS       | CLEAN         | ...     | total/free |
  | fs_list /            | PASS       | CLEAN         | ...     | <entries> |
  | fs_list /storage     | PASS       | CLEAN         | ...     | <entries> |
  | fs_list /storage/sbf | PASS       | CLEAN         | ...     | SBF files |
  | fs_list /storage/wot | PASS       | CLEAN         | ...     | WOT logs |
  | dtc_read             | PASS       | 0x19→0x59     | ...     | N DTCs |

  ## Async window (30 s)
  - WS events received: <list with timestamps>
  - CAN frames seen on 0x7E0/0x7E8 excl. KA: <count>
  - Frames on other IDs: <list — should be empty>

  ## Anomalies
  - <Each WS PASS but CAN unexpected, with frame dump>
  - <Each CAN traffic but WS errored, with response>
  - <Any frame on a forbidden ID, halted before completion>
  - <Any unexpected NRCs>

  ## Files written
  - REPORT.md (this file)
  - frames_<cmd>.log per command
  - async_events.log
  - async_frames.log
  - preflight_fail.log / anomaly_*.log if applicable

Print the absolute path to REPORT.md as the last line of stdout
so the user can find it easily.

==========================================================
HALT BEHAVIOR
==========================================================

If any Hard Rule fires (forbidden CAN ID, J533 lockout pattern,
dongle unresponsive), STOP immediately, write the appropriate
anomaly_*.log, print a single line stating the failure, and exit.
Do not retry. Do not attempt recovery. Do not fire any further
commands. The user reads the anomaly log and decides next move.

==========================================================
LOGGING
==========================================================

Append a one-paragraph summary to
<repo_root>\..\status-YYYY-MM-DD.md (today's date). Create the
file if missing. The paragraph should include: timestamp,
PASS/FAIL count from the per-command table, anomaly count, path
to REPORT.md.

==========================================================
DO NOT
==========================================================

- Do not modify any source file under firmware/, cloud/, ui/.
- Do not make a git commit.
- Do not fire any destructive command.
- Do not retry anything that halted via Hard Rules.
- Do not ask me clarifying questions — every choice is pre-decided.

Proceed.
```

---

## Notes for Sean (NOT for the agent)

**Why this prompt is long:** the user pasting this has no prior
project context, so the prompt has to ground them. The agent reads
CLAUDE.md, SESSION_HANDOFF.md, commands.c, and sbf_orchestrator.c to
understand the constraints before doing anything. No external
clarification needed.

**Windows-specific notes:**
- Path conventions use Windows syntax (`C:\Users\...`).
- `python` (not `python3`) is the typical Windows invocation; the
  agent should adapt if `python3` is what's installed.
- `$env:NAME` is the PowerShell syntax for env vars.
- `gs_usb` works the same on Windows via libusb; the Candlelight
  driver may need WinUSB via Zadig if the device shows up as an
  unknown adapter — the agent's pre-flight will catch this.

**Architectural correction baked in:** the prompt explicitly forbids
spawning `can_sniff.py` as a subprocess (which conflicted with the
keep-alive in the Mac session). One Python process owns the
Candlelight, with threaded keep-alive + capture + main test driver.
Per-command frame logs come from buffer extraction by timestamp,
not by separate subprocess invocation.

**What "comprehensive" means here:**
- Self-contained context (the agent reads its own briefing)
- Explicit hard rules with halt behavior per failure class
- Every safe WS command enumerated with expected CAN signature
- Every destructive command enumerated as forbidden
- Per-command CAN frame logs preserved for post-mortem
- Async event window after the active phase
- Comprehensive REPORT.md with environment + per-command table +
  anomaly section + file index
- Status-log append for the working agreement
- Clear "do not" list at the end so the agent can't scope-creep

If the agent surfaces an anomaly mid-flight, the per-anomaly log
file gives you everything needed to continue debugging without
re-running. The REPORT.md path is printed as the last line of
stdout, which makes it easy for the user to copy-paste back to you.

If you want to add anything — specific commands you want hit, env
vars you want set, or different failure-mode behaviors — say so and
I'll splice it in.
