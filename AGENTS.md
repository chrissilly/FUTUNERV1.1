# FUTUNER — Project Context for Codex

> This file is loaded automatically by Codex in every session.
> Read it before doing any work in this repo. Treat its rules as binding.

---

## What this project is

FUTUNER is a subscription-gated aftermarket ECU tuning dongle (ESP32-S3) that performs real-time calibration switching, live ethanol blending, and full binary reflashing on Bosch MG1 / MDG1 / MED17 ECUs over UDS / ISO-TP via CAN (Ethernet support coming). The product monetizes through VIN-locked lifetime licensing.

Vendor: Silly Rabbit Motorsport. Owner: Sean Cyr.

The canonical product spec is `docs/MISSION_SPEC.md`. The scaling / server architecture is `docs/SCALE_ARCHITECTURE_PROPOSAL.md`. The supported ECU matrix is `docs/boxcode_database.md`. Always consult these before touching architecture.

---

## Hard rules (do not violate)

### 1. CAN bus discipline (will brick the gateway, not just the ECU)

- **CAN ID `0x7E0` ONLY.** Never broadcast on `0x7DF`, never address `0x710` or `0x7E1`–`0x7E7`. The C8 J533 gateway will lock out diagnostic access for 10+ minutes if you scan addresses.
- **Standard UDS services only.** `0x3E`, `0x22`, `0x10` with known subfunctions. No exotic services.
- **No `can_send_raw` probing. Period.**
- Board: `BOARD_REV2` — GPIO 5 TX, GPIO 16 RX, 500 kbps.

### 2. Feature ON/OFF discipline (project rule)

Every user-visible feature (WOT logging, SBF live tune, ethanol BLE bridge, Phase 2 flash, DTC clear, etc.) must:

1. Default to **inactive** at boot. No silent background work.
2. Only run when explicitly started via web UI command or serial command.
3. Expose `start()` / `stop()` / `is_running()` and register with the **feature manager** (the central state arbiter).
4. The feature manager enforces "only one active feature at a time." Attempting to start a feature when another is running results in a warning + clean stop of the running feature, *then* start of the requested one. Never silent preemption, never two features running concurrently.

If you are adding a new feature, the first thing you do is wire it through `feature_manager`. Not optional.

### 3. No magic numbers

Constants — thresholds, timeouts, addresses, key references, log size limits, ethanol hysteresis, rev limits, etc. — must either be:

- Read from a versioned variant manifest (per-ECU values), OR
- Read from a per-firmware-build config (defaults), OR
- Read from per-device NVS (per-customer values).

Integer literals in `.c` files for behavioral constants are not acceptable. If you genuinely need a new constant, name it, default it in config, and surface it in this `AGENTS.md` or `docs/SCALE_ARCHITECTURE_PROPOSAL.md` so it gets reviewed before lock.

### 4. Mandatory progress logging

Every active session must update:

- `status-YYYY-MM-DD.md` at the workspace root — what was done, decisions made, open questions.
- `file-update-YYYY-MM-DD.md` at the workspace root — for every file written or edited, a concise note on what changed and why.

These exist already at `/Users/rabbit/esp/obd/`. Append to today's file if it exists; create today's if it doesn't.

### 5. Proprietary IP

All data here is SRM proprietary intellectual property. Do not exfiltrate, do not retain outside the local working tree, do not paste into third-party services for analysis. AES keys live in `secrets/` which is `.gitignore`d — never copy keys out of that directory in any artifact you produce.

### 6. Modular design + concise files

Keep individual files focused on a single responsibility. If a file exceeds ~500 lines, that's a smell — propose a split before continuing.

### 7. CLI references in docs must be `--help`-verified

Any document (handoff, runbook, README, etc.) that invokes a CLI tool MUST be verified against the tool's actual `--help` output before commit. Two production-blocking incidents (`tools/srm` and `tools/can_sniff.py` flag vapor, both 2026-05-19) traced to docs that referenced surfaces that didn't exist.

Mechanical check (do this in your head before any doc commit that adds a CLI invocation):

1. Run `<cli> --help` and read the output.
2. Every flag your doc uses must appear in `--help` with matching semantics.
3. If the doc requires a flag that doesn't exist, you have two choices:
   (a) add the flag to the CLI and verify, or
   (b) rewrite the doc to use flags that exist.
   NEVER commit a doc that references vapor.

### 8. Check git history before rewriting docs to match thin surfaces

If a doc references a CLI/API surface that doesn't exist in the current code, the default assumption is **NOT** "rewrite the doc." The default assumption is "the surface used to exist and got truncated by a merge / squash / lost commit." Rewriting a doc to match a regressed surface bakes in the regression.

Mechanical check (do this in your head before any "the doc is wrong, fix the doc" reflex):

1. `git log --all --full-history -- <tool path>` (and `--diff-filter=D` for files that may have been deleted).
2. If a prior version had the referenced surface, **restore the tool** — that is the fix.
3. Only rewrite the doc if history confirms the surface was always thin (i.e., the doc author was wrong, not the tool author).

Today's incident (2026-05-19): the HIL handoff doc was rewritten 3× against thin `ws_driver.py` / `tools/srm` / `tools/can_sniff.py` surfaces before anyone checked git history. The right sequence — check history first, restore-or-rewrite second — costs ~5 minutes; the wrong sequence cost ~3 cycles. The lesson sits next to Rule 7 because the two together form the doc-vs-code-surface discipline: Rule 7 catches new vapor at commit time, Rule 8 prevents regressed surfaces from being papered over with doc rewrites.

### 9. WS command names in docs must match firmware registry

Every WS command name a doc invokes (`handoffs/`, `docs/`, `README.md`) must appear in `firmware/src/commands/commands.c` `COMMAND_REGISTRY[]` before commit. This is the WS-command equivalent of Rule 7's `--help`-verify for CLI flags. Rules 7+8+9 together form the doc-vs-code-surface discipline: Rule 7 catches new CLI-flag vapor, Rule 8 prevents regressed surfaces from being papered over, Rule 9 catches WS-command vapor.

Mechanical check:

```
grep -oE '"[a-z_]+",' firmware/src/commands/commands.c | tr -d '",' | sort -u
```

Cross-reference with every command name the doc invokes via `ws_driver.py --script <name>` or equivalent. Doc author writes against the actual registry, not an imagined one.

Today's incident (2026-05-19): pre-HIL gate caught `uds_tester_present` in Phase 1b and `sbf_load` in Phase 3 of `handoffs/PHASE1_HIL_VALIDATION.md` — both vapor. Same class as REG-1..7 but on the WS-command surface instead of CLI flags. Fixes: `uds_tester_present` → `dtc_read` (provokes session, wire-witnesses identically); `sbf_load` → `live_tune_start` (the actual registered command name).

### 10. UI changes ship with Playwright browser tests

Any commit that touches `ui/control_panel.{html,css,js}` (or the bundled `firmware/futuner_control_panel.html`) MUST include or update tests in `tools/ui_tests/` that exercise the customer-visible behavior in a real browser. Host-gate static analysis (`firmware/test/ui/eval.sh` golden contracts) and WS-read verification (`ws_driver.py` probes) are NOT sufficient for customer-experience close gates — they catch only the registry shape and the WS-side payload, not the actual rendered DOM the user looks at.

Mechanical check:

```bash
cd tools/ui_tests
npx playwright test --reporter=line
```

Must exit 0 (excluding `NIGHTLY=1`-gated tests) before a UI commit is declared "done." Add a new `*.test.js` file per UI surface — Dashboard has `ui_dashboard_spec.test.js`; future Diagnostics/Live-Tune/WOT panels each get their own test file driving the visible flows.

The rule sits next to Rule 9 because the same pattern applies: Rule 9 catches WS-command vapor at the doc/code boundary; Rule 10 catches UI vapor at the JS/DOM boundary. Both close a gap where the surface "exists" on one side but does not work on the other.

Today's incident (2026-05-28): the P-69 Dashboard close was declared green based on `eval.sh` (golden-contract grep) + WS probes (`license_status` returned paid:true on the wire). Cowork's browser validation found the top-bar lock still rendered "License: unpaid · VIN (unpaired)". Root cause: the WS `command_handler` wraps every payload in a `data:{}` envelope, but `onLicenseStatusResp`, `updateDash`, and the active-feature dispatch all read top-level `msg.paid` / `msg.nmot_w` / `msg.active_feature`. Those reads silently returned undefined for every response, hiding behind `|| 0` fallbacks in the gauge code. The prototype dashboard appeared to "work" only because engine-off vars are 0, which is also the fallback. Playwright caught all four customer-visible failures (license lock, gauge updates, WOT banner, localStorage persistence) on the first run.

### 11. Wire witness runs continuously during all dev work

Before any HIL probe, UI vet, Playwright run, firmware flash, or ECU interaction: confirm `~/sniffer/can_tail.py` is running and capturing to `firmware/test/can_capture/dev_session/wire_*.log`. If absent, start it. If wedged (zero new frames for >30 s while the dongle is active), restart it. Closing a session without wire witness running means lost evidence; debugging without it means work the next bug forces you to redo.

Stage 1 — start (paste-ready, sudo required on macOS for gs_usb):

```bash
sudo pkill -f can_tail.py 2>/dev/null; sleep 1
STAMP=$(date +%Y%m%d_%H%M%S)
mkdir -p ~/esp/obd/FUTV1.1/firmware/test/can_capture/dev_session/
sudo python3 ~/sniffer/can_tail.py \
  --out ~/esp/obd/FUTV1.1/firmware/test/can_capture/dev_session/wire_${STAMP}.log \
  --timestamp &
```

Verify Candlelight LEDs are blinking. If not: cable / Y-splitter physical check, then if still dead restart once, then halt + surface.

Stage 2 — end-of-session: copy the active log to `firmware/test/can_capture/dev_session/<commit-SHA>.log` so it's pinned to the specific build that produced it. Keep `can_tail.py` running across CC restarts; stop only on intentional dongle / OBD disconnect.

The rule sits next to Rule 10 because both close customer-debug evidence gaps. Rule 10 catches UI vapor at the DOM boundary. Rule 11 catches missing-evidence gaps at the wire boundary — when a bug surfaces and there's no wire log to diff against, the work to reproduce is full and re-incurred. Today's incident (2026-05-28): three days of HIL + UI vetting + Playwright runs without continuous wire capture; P-52 (macOS gs_usb sustained-use wedge) was the historical justification but that justification expired now that the host has been stable for the last several sessions. From now on, missing wire witness ≡ unfinished dev work.

### 12. Wire-surface diagnostic discipline

Any firmware change touching CAN, UDS, ISO-TP, or other wire-bound code requires, **BEFORE any code change**:

(a) A captured wire log of the problem in the actual failure state.

(b) A diagnostic markdown at `firmware/test/<feature>/<date>_diagnostic.md` documenting the byte-level evidence, the proposed fix shape, and an explicit identification of which hypotheses were proven vs disproven by the capture.

(c) **Explicit owner sign-off on the diagnostic before the patch lands.**

Hypothesis-driven wire-surface patches are forbidden. The workflow exists because hypothesis patches have backed us into multiple dead-ends this development cycle. When the symptom is "this CAN exchange doesn't work," the answer is never "guess and reflash" — the answer is "capture the exchange, write down what we saw, propose the fix, get a yes, then patch."

Mechanical check (do this in your head before touching any `firmware/src/can/`, `firmware/src/dtc/`, `firmware/src/isotp_*`, `firmware/src/logger/logger_*.c`, `firmware/src/flash/`, or any other file whose changes affect bytes on the bus):

1. Is there a wire log capturing the current failure?
2. Is there a markdown report at `firmware/test/<feature>/*.md` documenting the byte-level analysis?
3. Has the owner read it and said "ship the fix"?

If any of those three is "no," **stop and produce the missing artifact**. Do NOT commit firmware code.

Today's incident (2026-05-29): P-54 ClearDTC fix went from VCDS wire capture → diagnostic markdown → firmware patch → flash → commit in a single un-paused chain. The diagnostic report `firmware/test/dtc/vcds_clear_capture_2026-05-29.md` was written, but the patch shipped before owner read it. The fix turned out to be correct (Mode 04 vs UDS \$14 was the right swap), but the *process* failure means a wrong fix could have shipped just as easily. Rule 12 lands so that doesn't keep happening.

Rule 12 sits next to Rule 11 because both gate work on evidence: Rule 11 ensures the wire is being captured during dev, Rule 12 ensures the captured evidence drives the patch and not a hypothesis.

---

## Repository layout

```
FUTV1.1/
├── AGENTS.md                                 ← you are here
├── README.md                                 ← high-level layout & build commands
├── docs/
│   ├── MISSION_SPEC.md                       ← canonical product spec (Phase 1 + Phase 2)
│   ├── SCALE_ARCHITECTURE_PROPOSAL.md        ← server / scaling architecture
│   ├── CAN_UDS_PROTOCOL.md                   ← every UDS service the dongle uses
│   ├── boxcode_database.{md,json}            ← supported ECU variant matrix (36 today, 100+ target)
│   ├── ecu_variable_db.json                  ← mapped ECU RAM variables (53 entries)
│   ├── uds_v1_protocol_albin.md              ← v1.0 UDS sequence reference
│   └── HISTORY_uds_regression.md             ← what broke between v1 and v2
├── firmware/
│   ├── src/
│   │   ├── main.c                            ← boot + top-level state
│   │   ├── state_machine/                    ← (note: actually CAN connection mgmt, NOT the feature arbiter — the feature arbiter is its own module)
│   │   ├── can/                              ← CAN driver
│   │   ├── isotp_coordinator/                ← ISO-TP segmentation
│   │   ├── commands/                         ← command dispatch (serial + websocket); ~14 command modules
│   │   ├── scal/, bdef/, ecu_write/          ← live tune RAM update path (proven from v1.0, do not modify casually)
│   │   ├── logger/                           ← gauge stream + WOT log capture
│   │   ├── flash/                            ← Phase 2 binary flash writer
│   │   ├── flex_fuel/                        ← ethanol constraint logic
│   │   ├── websocket/                        ← browser UI streaming
│   │   ├── wifi/                             ← AP→STA pairing
│   │   ├── nvs/                              ← persistent storage
│   │   ├── ota/                              ← firmware update (dual-bank)
│   │   ├── error/                            ← fault recovery + safe-idle
│   │   └── filesystem/, config/              ← support modules
│   ├── futuner_control_panel.html            ← single-page UI (loaded onto dongle)
│   ├── partitions.csv, sdkconfig.defaults, CMakeLists.txt
│   └── build.sh, flash.sh, monitor.sh
├── cloud/                                    ← FastAPI + SQLite cloud (sillyrabbitmotorsport.com)
├── ui/                                       ← active SPA (red/black theme)
├── sbf/                                      ← sample SBF/FBF files
├── tools/                                    ← sbf_to_json.py, can_sniff.py
├── hw_reference/                             ← XDFs, RE docs, working dongle dumps
└── secrets/                                  ← AES-128 keys (GITIGNORED, never commit)
```

---

## Build & flash

```bash
cd firmware
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3   # one-time
./build.sh
./flash.sh -p /dev/cu.usbmodemXXXX
./monitor.sh
```

Source baseline: git commit `7b4e525` is the last verified working firmware (12.4 Hz logger on car). Live-tune logic preserved byte-perfect from `ALBIN/ALBIN_WIP/ALBIN_WIP/sefi_firmware/`. Do not regress these.

---

## Current implementation status (as of 2026-05-05)

| Mission section | Status |
|-----------------|--------|
| 4.1 VIN pairing & licensing | not built |
| 4.2 SBF live calibration | scal/bdef/ecu_write proven; orchestrator needs flesh-out |
| 4.3 Live gauges | working (12.4 Hz proven on dev car) |
| 4.3 WOT logging | trigger + 60s cap + gzip + queue not implemented |
| 4.4 BLE ethanol bridge | not built |
| 4.5 Ethanol constraint logic + rev limit window | not built |
| 4.6 DTC read/clear | command stubs exist (`commands/dtc_commands.c`) |
| 4.7 Transport abstraction | CAN works; abstraction + Ethernet not built |
| 5.1 Phase 2 full binary flash | research complete, keys on file, not implemented |
| **Feature manager (state arbiter)** | **does not exist — required before further feature work** |

---

## Working agreements

- Ask clarifying questions when requirements are ambiguous. Do not assume.
- Critically evaluate any approach proposed. Flag scalability or maintainability concerns.
- Build and test features individually. The ON/OFF rule is enforced through the feature manager — do not bypass it.
- Update `status-YYYY-MM-DD.md` and `file-update-YYYY-MM-DD.md` for every session.
- Reference `docs/MISSION_SPEC.md` as the source of truth for product behavior. Reference `docs/SCALE_ARCHITECTURE_PROPOSAL.md` for server / scaling decisions.
