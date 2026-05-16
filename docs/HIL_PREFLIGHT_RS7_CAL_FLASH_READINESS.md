# HIL preflight — dev RS7 CAL flash readiness check (paste-ready prompt)

> Paste this into a fresh Claude Code session in `~/esp/obd/FUTV1.1/`
> when ready to do the hardware-in-the-loop preflight on the dev RS7
> before any real-car flash attempt. Don't dispatch this until Sean
> says "go HIL" — it talks to the dev car.

---

The dev RS7 (`4K0907557G__0003`, VIN `WUAPCBF28NN902533`) is the
canonical bench target for Phase 2 flash validation. Before any
RoutineControl Erase touches the car, run a HIL preflight that
exercises the orchestrator's SecurityAccess + fingerprint write
path on the real ECU AND diffs the dongle's emitted UDS bytes
against MM's reference capture. **HALT BEFORE THE FIRST EraseMemory
ROUTINE** — the goal is to prove preflight parity, not flash.

This is the last validation gate before P-07 (real-bench Phase 2
flash) opens. Failure here means the orchestrator's wire bytes
diverge from MM's on the real car; do not attempt a flash.

## ABSOLUTE rules

- **No EraseMemory.** The orchestrator MUST halt before emitting the
  first `31 01 FF 00 ...` routine. Wiring will set a hard stop in
  the orchestrator (gate by an env var or runtime flag).
- **No SBOOT touching.** SBOOT (`0x80000000..0x8001C000`) is
  write-protected by the ECU itself, but our orchestrator should
  never even attempt addressing it.
- **CAN ID `0x7E0` outbound / `0x7E8` inbound only.** Production
  CAN transport's hard guard from the orchestrator prompt enforces
  this — verify it's active.
- **Passive sniff via Candlelight in parallel** (separate USB-CAN
  adapter — NOT the dongle's own CAN port). Captures the wire
  conversation for independent diff against MM.
- **Engine OFF, key ON.** Bench mode. Battery on charger if the
  ECU's Programming Preconditions Check (`0x31 01 02 03`) fails on
  low voltage.
- **PUSH FREEZE still active** if it was when this prompt landed.
  Read `~/esp/obd/FUTV1.1/CLAUDE.md` first.

## Read first

- `~/esp/obd/CLAUDE.md` (workspace router)
- `~/esp/obd/FUTV1.1/CLAUDE.md` (hard rules)
- `~/esp/obd/FUTV1.1/docs/PHASE_2_PREREQUISITES.md` P-07 + P-08
- `~/esp/obd/FUTV1.1/hw_reference/MM_Flash_Capture_Analysis.md`
  (§2.1–§2.3 — preflight + SA + fingerprint)
- `~/esp/obd/FUTV1.1/hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md`
  (AES key location + IV + algorithm spec)
- `~/esp/obd/FUTV1.1/firmware/src/flash/mdg1_flash_orchestrator.{c,h}`
  (the orchestrator the preflight will run)
- `~/esp/obd/FUTV1.1/firmware/test/can_capture/fixtures/magicmotorsport/SUMMARY.md`
  (canonical-reference path discipline)

## Implementation tasks (write these BEFORE running Q1–Q8)

This prompt requires writing four pieces of code before the preflight
can actually run. Don't skip these or guess — read the relevant
existing files first, match the patterns already in the codebase, then
write. Compile after each task; don't accumulate. If any task surfaces
ambiguity, halt and ask Sean — don't paper over.

### Task 1 — HIL halt gate (≈15–30 LOC across 3 files)

Goal: a compile-time flag that makes the orchestrator halt between
fingerprint write and the first EraseMemory RoutineControl call.

- `firmware/src/flash/mdg1_flash_orchestrator_config.h`:
  Add `#ifndef MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE` /
  `#define MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE 0` / `#endif`.
  Inline comment annotating "approval-before-lock" per the
  no-magic-numbers convention. Default 0 = OFF.
- `firmware/src/flash/mdg1_flash_orchestrator.h`:
  Add `MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE` as the next enum
  value in the phase enum. Don't renumber existing values.
- `firmware/src/flash/mdg1_flash_orchestrator.c`:
  Find the spot between fingerprint-write completion and the start
  of the per-section loop that performs RoutineControl-Erase
  (block IDs 1–5). Insert, guarded by `#if MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE`:

  ```c
  #if MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE
      if (progress_cb) {
          progress_cb(MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE,
                      0, plan, progress_ctx);
      }
      ESP_LOGI(TAG, "HIL preflight halt — fingerprint written, "
                    "erase suppressed");
      return ESP_ERR_NOT_FINISHED;
  #endif
  ```

  Verify by grepping that the halt insertion is the FIRST possible
  return point after the fingerprint-ack path. If any other path
  could reach Erase first, halt and surface.

### Task 2 — Serial command handler (≈100–200 LOC across ≤2 files)

Goal: a serial-invokable command `phase2_hil_preflight` that runs
the orchestrator with the HIL halt flag effective. WS surface is
deferred to a later prompt; this prompt uses serial only.

- Look at how `CMD_DTC_READ` (or another existing serial-invokable
  feature command) is registered in `firmware/src/commands/commands.c`
  to match the dispatch pattern exactly.
- Add `CMD_PHASE2_HIL_PREFLIGHT` enum value + case clause.
- Implement `phase2_hil_preflight_handler(const char *args)`:
  1. `feature_manager_enable(FEATURE_PHASE2_FLASH)` — abort with
     a clear error if another feature is currently active (ON/OFF
     discipline; the orchestrator is the single owner of CAN during
     its run).
  2. Load variant: `mdg1_variant_manifest_load_for_box_code(
     "4K0907557G__0003", &variant_out)`. Halt on failure with the
     manifest loader's error code.
  3. Construct `mdg1_flash_plan_t` populated with the variant.
     Read the struct definition in `mdg1_flash_orchestrator.h` to
     know which fields are required and which are output-only.
  4. Define a progress callback that streams events to `UART_NUM_0`
     as `[PHASE2_HIL] phase=<name> pct=<n>` (one line per event).
  5. Call `mdg1_flash_orchestrator_run(&plan, prod_can_transport,
     progress_cb, NULL)` and capture the return code.
  6. Expected return: `ESP_ERR_NOT_FINISHED` (that's what Task 1's
     halt returns). Any other return code = log + halt + report
     to serial.
  7. `feature_manager_disable(FEATURE_PHASE2_FLASH)` in a cleanup
     block — must run regardless of outcome.

### Task 3 — Candump-to-shadow-log adapter (≈50 LOC, 1 file)

Goal: parse the Candlelight candump capture into the line-oriented
format that `tools/flash_shadow_diff.py` expects.

- New file: `tools/candump_to_shadow_log.py`
- Input candump format: `(timestamp) interface ID#data` per line
  (this is `candump -L` output).
- Filter to ID `0x7E0` (TX from tester → ECU) and `0x7E8` (RX from
  ECU → tester). Discard everything else.
- ISO-TP reassemble: single-frame (`0x0n`), first-frame (`0x1n`),
  consecutive-frame (`0x2n`), flow-control (`0x3n`). If
  `flash_shadow_diff.py` already has a reassembler, import or
  port it — don't write a second copy.
- Emit lines: `TX <hex-bytes-no-spaces>` for 0x7E0 messages,
  `RX <hex-bytes-no-spaces>` for 0x7E8 messages.
- Stdin/stdout: read candump from argv[1] or stdin, write to
  stdout or argv[2]. Match the convention `flash_shadow_diff.py`
  uses for shadow input.

### Task 4 — Q8 CheckMemory decision tree

Q8 of the procedural section punts on which CheckMemory approach
to use. Order of attempt, no guessing:

1. **Default session, no prior `10 02`:** send
   `31 01 02 02 45 85 0B EA` directly.
   - Positive (`71 01 02 02 00`) → done, CAL matches manifest,
     proceed.
   - NRC `7F 31 22` (conditionsNotCorrect) → try step 2.
   - NRC `7F 31 11` (serviceNotSupported) → try step 3.

2. **Extended diagnostic session:** send `10 03` then
   `31 01 02 02 45 85 0B EA`.
   - Extended session is read-only; safe to enter, doesn't risk
     state.
   - Do **NOT** enter programming session `10 02` for this check —
     too much state risk for a read-only validation.

3. **Fallback if both fail:** read `22 F1 9E` (SW number) and
   `22 F1 A2` (programming history) as sanity checks. Flag the
   actual CRC verification as DEFERRED in the chip report.
   Proceeding to real flash without CRC confirmation is Sean's
   call — surface clearly, don't decide unilaterally.

## Mandatory progress logs (workspace standing rule)

Append to `~/esp/obd/status-YYYY-MM-DD.md` and
`~/esp/obd/file-update-YYYY-MM-DD.md` per the workspace's
log-discipline rule (see `FUTV1.1/CLAUDE.md` → Hard rules →
Mandatory progress logging). Status log gets the final chip
report. File-update log gets one delta block per file touched
in Implementation tasks 1–3, with a one-line "why" per file.

## Pre-decided choices

### Q1 — Build the firmware with Phase 2 ENABLED

```
cd ~/esp/obd/FUTV1.1/firmware
source ~/esp/esp-idf/export.sh
idf.py -DEXTRA_CFLAGS="-DFUTUNER_PHASE2_ENABLED=1" build
```

Verify rc=0. The resulting binary at
`build/futuner_v2.bin` is the artifact to flash to the dongle.

### Q2 — Add a HARD-STOP gate to the orchestrator

Before flashing, introduce a runtime flag
`MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE` (#define in
`mdg1_flash_orchestrator_config.h`, default 0). When set to 1, the
orchestrator returns `ESP_ERR_NOT_FINISHED` immediately after the
fingerprint write phase succeeds and emits a progress event
`MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE` (new enum value).

For the preflight build:

```
idf.py -DEXTRA_CFLAGS="-DFUTUNER_PHASE2_ENABLED=1 -DMDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE=1" build
```

This is the gate that turns "would-flash" into "halts-before-erase."

### Q3 — Flash dongle + verify boot

```
./flash.sh -p /dev/cu.usbmodemXXXX
./monitor.sh   # confirm boot logs, FUTUNER version, FEATURE_PHASE2_FLASH registered
```

Expected boot log line (per the orchestrator prompt's `main.c`
addition):

```
I (T) FUTUNER: Phase 2 flash: mbedtls AES iface registered (FUTUNER_PHASE2_ENABLED=1)
```

If not present, the build flag didn't propagate. Halt.

### Q4 — Wire the dev RS7

- Battery charger ON
- Ignition: key to RUN (position 2), do NOT crank
- OBD-II port: dongle CAN-H/CAN-L
- Candlelight: tap CAN-H/CAN-L (in parallel with dongle); USB to
  separate laptop running `candump can0 -L > /tmp/hil_preflight_capture.log`
  to record the full bus
- Confirm Candlelight sees the dongle's tester-present frames at
  `0x700` (gateway) and `0x7E0` (engine ECU) within 5 seconds

### Q5 — Trigger orchestrator preflight via WS command

In a follow-up prompt (not this one), wire a WS command
`phase2_flash_hil_preflight` that:

1. Loads the variant for `4K0907557G__0003` via the manifest loader
2. Constructs an `mdg1_flash_plan_t` with the HIL halt flag set
3. Calls `mdg1_flash_orchestrator_run(plan, prod_can_transport,
   progress_cb, NULL)`
4. Streams progress events to the UI

For THIS prompt, the WS surface doesn't exist yet — invoke via
serial command instead. Add to `commands/system_commands.c`:

```c
case CMD_PHASE2_HIL_PREFLIGHT:
    return phase2_hil_preflight_handler(...);
```

### Q6 — Expected wire-byte sequence

After the orchestrator runs, the dongle should have emitted (on
`0x7E0`, captured by Candlelight on the bus):

1. SecurityAccess seed-request (`27 11`)
2. SA seed receive (`67 11 <4-byte seed from ECU>`)
3. SA2 VM compute, key send (`27 12 <4-byte key>`)
4. SA key ack (`67 12`)
5. Fingerprint write (`2E F1 5A 21 11 22 00 06 46 22 0A 68`)
6. Fingerprint ack (`6E F1 5A`)
7. **HALT.** Progress event fired:
   `MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE`. No bytes emitted past
   this point.

If a `31 01 FF 00 ...` frame appears in the Candlelight log, the
halt flag didn't take — kill everything immediately and investigate.

### Q7 — Diff against MM's reference

Take the Candlelight-captured log, slice the SA-seed-request through
fingerprint-ack window, and diff against
`/Users/rabbit/sniffer/mm_FULL_Flash.log` for the same window
using `tools/flash_shadow_diff.py`:

```
python3 tools/flash_shadow_diff.py \
    --shadow /tmp/hil_preflight_capture.log.parsed \
    --reference /Users/rabbit/sniffer/mm_FULL_Flash.log \
    --window flash-critical
```

(The `--shadow` input here is a UDS-level log, not the Candlelight
raw candump. Need a thin adapter: parse Candlelight's candump,
ISO-TP-reassemble UDS messages, emit `TX <hex>` / `RX <hex>` lines.
That adapter is the new tooling this prompt needs to write —
estimate 50 LOC of Python reusing `flash_shadow_diff.py`'s
internals.)

**Expected result:** PROTOCOL MATCH for the SA → fingerprint window
after session-variant masking (SA seed/key + fingerprint bytes are
masked; everything else byte-perfect).

### Q8 — Read ECU's current CAL CRC

To validate that the dev RS7's current CAL section matches the
manifest's expected CRC (i.e. the ECU hasn't been re-flashed since
MM did its capture), execute:

```
22 F1 9E      # ECU SW number (sanity check VIN matches)
22 F1 A2      # programming history (will tell us if MM-or-anyone
              # has flashed since the manifest capture)
```

Then read the CAL section CRC directly. The ECU exposes this via
`22 <CRC_DID>` if the bootloader supports it (uncertain — may
require entering programming session first via `10 02`, which
risks state). Alternative: invoke `31 01 02 02` (CheckMemory)
with the manifest's expected CRC `0x45850BEA` — ECU's positive
response (`71 01 02 02 00`) confirms the CAL plaintext on the
ECU matches what our manifest expects.

If CheckMemory returns negative: the dev RS7 has drifted from MM's
reference state. **Identical-content reflash is no longer
identical** — halt + investigate (was the car re-flashed by
another tool? did MM's capture become stale?).

## Acceptance criteria — all must pass

- [ ] `./build.sh` with `FUTUNER_PHASE2_ENABLED=1 +
      MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE=1` exits 0
- [ ] Dongle boots, logs Phase 2 AES iface registered
- [ ] Dev RS7 SA seed exchange succeeds (`67 11 <seed>` received)
- [ ] SA2 VM produces a key the ECU accepts (`67 12` positive)
- [ ] Fingerprint write succeeds (`6E F1 5A`)
- [ ] Orchestrator HALTS — no `31 01 FF 00 ...` Erase frame on bus
- [ ] Candlelight log captured without dropped frames
- [ ] `flash_shadow_diff.py` reports PROTOCOL MATCH for the SA →
      fingerprint window
- [ ] ECU's CAL section CheckMemory `31 01 02 02 45 85 0B EA`
      returns positive (`71 01 02 02 00`) — confirms ECU CAL
      content matches the manifest

## When done

Print a chip report:

```
HIL preflight — dev RS7 CAL flash readiness — YYYY-MM-DD HH:MM
=================================================================
Firmware build (Phase 2 ON + halt flag):       PASS/FAIL  (rc=?)
Dongle boot + Phase 2 init:                    PASS/FAIL
SA seed exchange:                              PASS/FAIL  (seed=??)
SA2 VM key derivation:                         PASS/FAIL
Fingerprint write:                             PASS/FAIL
HALT before EraseMemory:                       PASS/FAIL  (zero 31 01 FF 00 frames)
Candlelight capture integrity:                 PASS/FAIL  (frames captured, no drops)
flash_shadow_diff PROTOCOL MATCH:              PASS/FAIL
ECU CAL CheckMemory vs manifest:               PASS/FAIL  (CRC 0x45850BEA matches)

If all PASS: P-07 (real-bench Phase 2 flash) opens. Schedule the
                 actual flash session under a different prompt.

If anything FAIL: halt, surface, file an issue against P-07. Do
                  NOT attempt a real flash until every check is green.
```

Append the chip report to `~/esp/obd/status-<DATE>.md` under
`## HIL preflight RS7 CAL`. Per-file delta to
`~/esp/obd/file-update-<DATE>.md`.

## Forbidden

- Cranking the engine (battery sag could corrupt mid-test)
- Attempting EraseMemory or any subsequent RoutineControl
- Modifying the variant manifest, AES key table, or SA2 script
  during this prompt (this is a read-only validation pass)
- Committing anything to git
- Pushing anything to GitHub (PUSH FREEZE)
- Running this prompt on any vehicle OTHER than the dev RS7
  (`4K0907557G__0003`, VIN `WUAPCBF28NN902533`)

## When NOT done — bail conditions

- Battery voltage drops below 11.5 V → halt, charge, retry
- Gateway J533 returns `7F 14 11` for ClearDTC THREE TIMES in a row
  → suspect gateway lockout, key-cycle the car and wait 10 min
- Any UDS exchange takes longer than 5 s (P2* timeout) → halt,
  surface, do not retry blindly
- Candlelight sees a frame on a CAN ID not in the allowed set
  (`0x7E0`, `0x7E8`, `0x700`, `0x77C`, `0x77D`, `0x780`, `0x7E9`,
  `0x17FC0076`, `0x17FE0085`, `0x17FE009C`) → halt, surface

## Proceed only after Sean's "go HIL"

This prompt's outcome is a go/no-go for the actual flash, not the
flash itself. P-07 ("Real-bench Phase 2 validation per variant")
opens only when this prompt's chip report is all-green.
