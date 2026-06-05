# P-80 — Refcounted Logger Polling Lifecycle: Pre-Patch Diagnostic

Per Rule 12: this markdown is required reading before any firmware patch
to the logger-polling lifecycle ships.

**Status: design signed off; patch applied + flashed to dev dongle.
Post-fix verification trace captured 2026-06-05 and confirms the fix
(see "Post-fix verification" below). Awaiting owner commit sign-off
per Rule 12(c) before the patch lands on origin/main.**

---

## Symptom (Sean, on the live RS7)

> "hitting Stop doesn't stop the CAN traffic"

User clicks the Dashboard Start button → gauges populate. Clicks
Dashboard Stop → gauges fade in the UI, but CAN frames keep flowing
on the bus indefinitely. No path back to "polling off" short of a
dongle reboot.

## Wire-log evidence (existing capture)

Source: `firmware/test/can_capture/dev_session/obd_clear.log`
(rooted-can_tail capture, ~39 min span, ts 130 → 2475).

```
Total dongle logger-poll requests on 7E0: 9,626
Dongle polling span:                      ts 471.04 → ts 2475.07
Continuous polling duration:              2,004 s = 33 m 24 s
logger_stop frames observed:              0
```

The dongle issued logger-poll requests every ~200 ms continuously
across the entire post-replug portion of the log. Sean was actively
clicking UI elements during this window (Dashboard, DTC clear,
VCDS comparison). At no point did polling stop. No `logger_stop`
WS command was ever sent — consistent with the UI's
`dashboardStop` intentionally suppressing it.

## Root cause (code audit)

### Global flag

`firmware/src/state_machine/connection_manager.c:28-29`

```c
/* Set true via connection_manager_logger_start(). */
static bool logger_polling_enabled = false;
```

Single global. Set/cleared by a pair of public functions on
lines 941 / 947. Read at line 620 in the polling-decision branch:

```c
if (is_patched && logger_manager_is_configured() && logger_polling_enabled) {
    /* poll the ECU */
}
```

### All 4 callers of start/stop (and their gating)

| File | Line | Caller | Path |
|---|---|---|---|
| `commands/system_commands.c` | 174 | `cmd_logger_start` | WS `logger_start` command |
| `commands/system_commands.c` | 180 | `cmd_logger_stop` | WS `logger_stop` command (NEVER fired by UI) |
| `commands/serial_console.c` | 168 | serial `logger_start` line | dev serial console |
| `commands/serial_console.c` | 172 | serial `logger_stop` line | dev serial console |

No reference-counting; every caller fights for the same global bit.

### UI never fires logger_stop

`ui/control_panel.js:2632-2637`

```js
function dashboardStop(){
  …
  /* Do NOT fire logger_stop — logger lifecycle is owned elsewhere. */
}
```

Comment block at line 2350 reinforces this: "logger_stop would kill
polling for everyone." Dispatch correctly identifies this as
aspirational — nothing else actually owns polling, so the
"protection" leaves polling stuck on.

### WOT logger does NOT touch polling

`firmware/src/logger/wot_logger.c`

```
$ grep 'logger_polling\|logger_start\|logger_stop\|polling_is_active'
  (no matches inside wot_logger_start / wot_logger_stop)
```

WOT capture implicitly depends on polling already being on. With a
refcount, WOT MUST acquire its own ref or the dashboard-Stop case
will kill polling out from under an active WOT capture.

### Hypotheses

| # | Hypothesis | State |
|---|---|---|
| 1 | A global flag governs polling | **Proven** — `logger_polling_enabled` at connection_manager.c:29, read in the polling branch at line 620 |
| 2 | Dashboard Stop suppresses the WS logger_stop deliberately | **Proven** — comment block at control_panel.js:2632-2637 |
| 3 | Polling is therefore sticky-on from first Start until reboot | **Proven** — 33-minute continuous polling span in obd_clear.log with no logger_stop ever sent and Sean actively clicking UI |
| 4 | WOT logger silently depends on polling being already on | **Proven** — wot_logger.c has zero polling calls |
| 5 | A 5th consumer of polling exists that the dispatch didn't enumerate | **Proven** — `serial_console.c:168/172` is a 5th call site. Dispatch enumerated WS-side + Dashboard + WOT + Live Tune but not the dev serial console |
| 6 | No 6th consumer exists | **Likely** — full grep of `connection_manager_logger_start/stop` across firmware/src/ returns only the 4 sites in (1+5). No silent callers in feature_manager, livetune, sbf, or anywhere else |

## Why P-72 single-owner doesn't already cover this

P-72 enforced single-owner on `logger_manager` apply (the
clear+add-required+add-saved sequence). That's about WHAT the
logger reads. P-80 is about WHEN the polling loop runs (the
control flow at connection_manager.c:620). Different state,
different layer. Both refactors mirror the same "many writers,
one truthful state" pattern.

## Proposed fix shape (per dispatch DESIGN section)

Refcount with two consumer flavors:
- **Internal consumers** (`LOGGER_CONSUMER_WOT`, `LOGGER_CONSUMER_LIVETUNE`):
  fixed-size bool array; touched from can_task only (per P-72
  inheritance).
- **WS-fd consumers** (per-fd, up to N concurrent clients):
  bitmap protected by critical section; touched from WS task and on
  WS disconnect.

Polling decision becomes:

```c
if (is_patched && logger_manager_is_configured() && refcount_nonzero()) { … }
```

Wire each consumer:

| Consumer | Acquire | Release |
|---|---|---|
| Dashboard streaming | `cmd_logger_start` (WS) | `cmd_logger_stop` (WS) — UI to send it from `dashboardStop` |
| Dashboard WS disconnect | (n/a — release auto-fires) | command_handler disconnect hook |
| WOT capture | top of `wot_logger_start` | top of `wot_logger_stop` |
| Live Tune (Phase 3) | gated by `FUTUNER_PHASE3_ENABLED` | same |
| Serial console (5th consumer) | `serial_console.c:168` rewrite | `serial_console.c:172` rewrite — needs a new `LOGGER_CONSUMER_SERIAL` enum value OR reuse `LOGGER_CONSUMER_INTERNAL_*` |

Magic numbers go in `firmware/src/config/logger_config.h`:
- `LOGGER_WS_CONSUMER_MAX_FDS` (suggest 8 — matches
  `MAX_AUTHENTICATED_CLIENTS` from pre-P-75)
- `LOGGER_REFCOUNT_SENTINEL_NONE = 0`

The global flag at `connection_manager.c:29` is **deleted** (not
left dangling). Public functions `connection_manager_logger_start/
stop` become deprecation shims that bridge to
`connection_manager_logger_ws_acquire/release` with the calling fd,
OR are deleted entirely and callers updated.

## Recommended HALT pre-patch

Dispatch's HALT trigger about the 5th consumer is hit: `serial_console.c`
is a 5th call site. Two ways to handle:

(a) **Add `LOGGER_CONSUMER_SERIAL`** to the enum (matches dispatch's
    "internal consumer" flavor since serial is a single global path,
    not per-client).
(b) **Bridge serial through the same WS-fd bitmap** with a sentinel
    fd value (e.g., fd = -1 reserved for serial).

(a) is cleaner. Surfacing for owner choice before patching.

## Post-fix wire-log evidence (planned)

Per dispatch R2: after the patch lands, the commit body will paste:

```
ts X.000   7E0#06 3E 33 50 01 D6 78 00     last logger poll BEFORE Dashboard Stop
ts X.020   WS in:  logger_stop
ts X.025   WS out: logger_stop OK refcount=0
ts X.220   (next expected poll cycle — NOT EMITTED)
ts X.420   (next-next — still no poll)
ts X.620   (still nothing)
ts X.>1.0  TesterPresent keepalive on 7E0#02 3E 00 (expected; this is
                                                    NOT polling)
```

(NOT included: tester-present frames. The HALT trigger about
"dongle still talking" applies — clarify in the commit body that
TesterPresent is the connection-manager keepalive, separate from
logger polling.)

Sean will reproduce this manually post-flash via the live UI and
confirm against wire log.

## Post-fix verification (2026-06-05) — ACTUAL trace

Source: `firmware/test/can_capture/dev_session/log.log` (rooted
can_tail capture, sorted span ts 2018.9 → 4294.9, ~38 min). Captured
on the live RS7 with the P-80 firmware flashed.

Logger-poll signature on this build is the 6-byte ReadData frame
`7E0#06 3E 33 50 01 D6 ..` (followed by its `7E0#30 08` flow-control
for the multi-frame response). TesterPresent keepalive is
`7E0#02 3E 00`. The two are distinct on the wire, which is what lets
us prove "polling stopped, connection alive."

Counts over the whole capture: **14,513 logger polls**, **22,120
TesterPresent**. Active poll cadence ~90 ms.

### Stop event — polling halts within one cycle, keepalive continues

```
ts 2040.872  7E0#02 3E 00            TesterPresent
ts 2041.068  7E0#06 3E 33 50 01 D6   <-- LAST logger poll before Stop
ts 2041.088  7E0#30 08 ..            (flow-control for that poll's response)
ts 2041.173  7E0#02 3E 00            TesterPresent
ts 2041.383  7E0#02 3E 00            TesterPresent
ts 2041.514  7E0#02 3E 00            TesterPresent
   ... [NO logger-poll frame for the next 408 s] ...
ts 2449.6    7E0#06 3E 33 50 01 D6   (polling resumes — second Start)
```

Window 2041.1 → 2449.6 (408 s): **POLL = 0, TesterPresent = 2838**
(~6.9/s). Polling ceased on the first cycle after the WS `logger_stop`
(refcount → 0) and stayed off; the connection-manager keepalive was
untouched. A second Start at 2449.6 brought polling back, and a brief
5 s Stop at 2451.8 → resume 2457.2 confirms the lifecycle is now freely
toggleable rather than sticky-on-until-reboot.

This is the behavior the old global flag could not produce: under
`7b4e525`-era firmware the `obd_clear.log` capture showed 9,626
continuous polls over 33 min with zero stops. The refcount fix
makes Stop actually stop.

Note (per the HALT trigger in the planned-evidence section above):
the WS `logger_stop` request and its `refcount=0` reply ride the
WebSocket, not CAN, so they do not appear in this wire log. The CAN
evidence is the cessation of the `06 3E 33 ..` poll frame while the
`02 3E 00` keepalive continues — exactly the separation Rule 12
asks us to witness on the bus.

## Sign-off requested

Per Rule 12 (c), this diagnostic needs explicit "ship the fix" from
Sean before any patch lands. Specifically:

1. Refcount design as described above.
2. Choice on serial-console consumer (a vs b above).
3. Public-API decision: deprecate or delete the global-flag
   `connection_manager_logger_start/stop` functions.
4. Acknowledgment that WOT capture path will be modified
   (`wot_logger_start/stop` gain acquire/release calls).

After sign-off the patch lands in one commit with:
- C99 refcount machinery in connection_manager.{c,h}
- UI dashboardStop sending logger_stop
- WOT acquire/release wired
- Serial console rewired
- AC-12 + AC-13 Playwright additions
- Wire-log evidence excerpt in commit body
