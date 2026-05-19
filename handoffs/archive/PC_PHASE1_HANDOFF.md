# FUTUNER — PC machine handoff (Phase 1 ownership)

> Paste-ready prompt for a fresh Claude Code session running on the PC under WSL2 Ubuntu. The PC owns Phase 1 work going forward; the Mac retains Phase 2. Read this entire doc before touching code.

---

## What this machine is for

**You are the Phase 1 owner.** The Mac is mid-Phase-2-flash work and keeps that scope; the PC takes everything Phase 1 from here on.

- **Phase 1** = live RAM tuning + non-destructive UDS + customer-facing surfaces. Already shipped at 8 eval gates green: feature_manager, wot_logger, dtc, vin_pairing, sbf live tune, ui, plus ethanol BLE bridge and ethanol constraints/rev limiter safety. You maintain, extend, and validate these.
- **Phase 2** = full 8 MB ECU reflash via UDS/AES-128. **Off-limits on this machine.** Mid-flight on Mac; don't read it deeply, don't modify it, don't run it.

You also pick up Hermes corpus characterization work (the long-running offline-AI archive analysis) since the 164 GB `034_local/` archive was migrated to this machine.

---

## Read first (in this order)

1. `~/esp/obd/CLAUDE.md` — workspace router. Has the active PUSH FREEZE callout. Read entirely.
2. `~/esp/obd/FUTV1.1/CLAUDE.md` — project hard rules. Read entirely.
3. `~/esp/obd/HANDOFF_TO_PC.md` — comprehensive Mac→PC migration doc. **Skip §8 (Phase 2 state) and §9 (Phase 2 bugs)**; they're not your scope. Read everything else.
4. `~/esp/obd/FUTV1.1/docs/MISSION_SPEC.md` §1 (Phase 1 features only).
5. `~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_VALIDATE_DONGLE.md` — Phase 1 hardware-loop validation procedure.
6. `~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_BUILD_SRM_CLI.md` — the `tools/srm` CLI subcommands you'll use.

---

## Absolute rules (carry through every action)

| Rule | Why |
|---|---|
| 🛑 **PUSH FREEZE active** | No `git push`, no `gh pr create`, no tag pushes. Local commits fine. See FUTV1.1/CLAUDE.md Hard Rule 7. |
| **Phase 2 is OUT OF SCOPE** | Don't modify `firmware/src/flash/mdg1_*` or `firmware/src/flash/phase2_*` or `firmware/src/flash/sa2_*`. Don't build with `FUTUNER_PHASE2_ENABLED=1`. Don't run `tools/srm flash --phase2` or `tools/srm capture`. The Mac is mid-bugfix on these; touching them here causes merge conflicts. |
| **CAN ID 0x7E0 outbound / 0x7E8 inbound ONLY** | Any frame on another ID in production transport = halt and surface. Standard Phase 1 hard rule. |
| **No magic numbers** | Every constant in `*_config.h` with "approval before lock" annotation. |
| **ON/OFF discipline** | All features arbitrate via `feature_manager`. Nothing runs unless explicitly enabled via WS/serial command. |
| **Mandatory progress logs** | Every working session appends to today's `~/esp/obd/status-YYYY-MM-DD.md` AND `file-update-YYYY-MM-DD.md`. Independent of the Mac's logs (Mac writes its own status entries; you write yours). Tag your entries with `[PC]` so coordination is clean. |
| **Proprietary data stays local** | Bins, AES keys, MM captures, VINs — never leave this machine except via the existing FUTUNER cloud server. |
| **Frozen modules** | `firmware/src/scal/`, `firmware/src/bdef/`, `firmware/src/ecu_write/` are byte-perfect from FUTV1.0. Never modify. `firmware/test/verify_frozen.sh` is the gate. |
| **No mid-flight conflicts with Mac** | Mac owns `firmware/src/flash/`, `firmware/test/mdg1_flash_orchestrator/`, `firmware/test/mdg1_payload/`, `tools/flash_shadow_diff.py`, `tools/candump_to_shadow_log.py` (if it exists). Don't touch any of those. If you find yourself wanting to, halt and surface — there's a coordination issue. |

---

## Phase 1 surface area (your scope)

| Component | Source | Eval gate |
|---|---|---|
| Feature manager (ON/OFF arbitration) | `firmware/src/feature_manager.{c,h}` | `firmware/test/feature_manager/eval.sh` |
| WOT logger (auto + manual triggering, 60s cap, gzip, cloud sync) | `firmware/src/wot_logger.{c,h}` | `firmware/test/wot_logger/eval.sh` |
| DTC read/clear (UDS 0x19 + 0x14) | `firmware/src/dtc.{c,h}` | `firmware/test/dtc/eval.sh` |
| VIN pairing (AP→STA→server token, VIN lock) | `firmware/src/vin_pairing.{c,h}` | `firmware/test/vin_pairing/eval.sh` |
| SBF live tune (RAM updates, ethanol blend switching) | `firmware/src/sbf/` | `firmware/test/sbf/eval.sh` |
| UI / WebSocket live gauges | `firmware/src/ui/`, `ui/` (frontend) | `firmware/test/ui/eval.sh` |
| Ethanol BLE bridge (sensor pairing, manual fallback) | `firmware/src/ethanol_ble/` | (subset of feature_manager + sbf eval) |
| Ethanol constraints + rev limiter safety | `firmware/src/ethanol_constraints.{c,h}` | (within sbf eval) |
| Cloud server (Python, backend) | `cloud/` | `cloud/tests/` |
| Cloud dashboard (frontend) | `ui/dashboard/` (if applicable) | varies |
| `tools/srm` CLI (excluding flash/capture subcommands) | `tools/srm/` | `tools/srm/test/test_cli.py` |

If a bug or feature request lands on any of the above, it's yours to work.

---

## Out-of-scope on this machine (Mac-owned)

| Component | Source | Why off-limits |
|---|---|---|
| Phase 2 orchestrator | `firmware/src/flash/mdg1_flash_orchestrator.{c,h}` | Mac is mid-bugfix on session-control + NRC handling |
| Phase 2 transports | `firmware/src/flash/mdg1_transport_*.{c,h}` | Production transport just wired; bugs surfaced |
| Phase 2 payload | `firmware/src/flash/mdg1_payload.{c,h}` | Frozen state, oracle-validated |
| LZRB codec | `firmware/src/flash/lzrb.{c,h}` | Used only by Phase 2 |
| SA2 VM | `firmware/src/flash/sa2_vm.{c,h}` | Security Access bytecode interpreter |
| Variant manifest loader | `firmware/src/flash/mdg1_variant_manifest.{c,h}` | Phase 2 boot-time loader |
| AES iface (mbedtls + host) | `firmware/src/flash/mdg1_aes_mbedtls.c`, `firmware/test/mdg1_payload/tiny_aes.{c,h}` | Phase 2 crypto path |
| Phase 2 autostart | `firmware/src/flash/phase2_hil_autostart.{c,h}` | HIL preflight one-shot mechanism |
| Phase 2 eval gates | `firmware/test/mdg1_flash_orchestrator/`, `firmware/test/mdg1_payload/` | Mac runs these |
| Phase 2 tooling | `tools/flash_shadow_diff.py`, `tools/candump_to_shadow_log.py` (if landed) | Mac uses these for Phase 2 diff |
| `tools/srm flash --phase2`, `tools/srm capture` | CLI subcommands | Phase 2 dispatch |
| HIL preflight doc | `docs/HIL_PREFLIGHT_RS7_CAL_FLASH_READINESS.md` | Mac's active doc |
| Phase 2 prerequisites | `docs/PHASE_2_PREREQUISITES.md` | Reference only; don't modify |

If any of these need changes, file a request and surface it; don't edit them here.

---

## Bootstrap sequence (first session on the PC)

```
1.  Verify the rsync landed.
    ls -la ~/esp/obd/
    Confirm: FUTV1.1/, FUTV1.0/, CLAUDE.md, status-2026-05-12.md,
             file-update-2026-05-12.md, HANDOFF_TO_PC.md, PC_PHASE1_HANDOFF.md

2.  Verify Hermes-relevant assets:
    ls ~/.hermes/.env                    (Hermes API key)
    ls -d ~/034_local/                   (164 GB ECU archive — should exist)
    ls -lh ~/sniffer/*.log               (MM captures — should exist)

    Note: the Mac retains the sniffer captures (Phase 2 reference data),
    but if they came over in your rsync that's fine. You won't use them
    for Phase 1 work, but they're harmless to have.

3.  Set up WSL2 USB pass-through if you'll be doing HIL validation on
    this machine (otherwise skip):
    - Install usbipd-win on Windows host
    - usbipd bind --busid <BUSID> for the dongle
    - usbipd bind --busid <BUSID> for the Candlelight (if applicable)
    - usbipd attach --wsl --busid <BUSID> for each

4.  Install toolchain:
    sudo apt update
    sudo apt install -y python3-pip python3-venv can-utils sshpass usbutils
    pip3 install --break-system-packages gs_usb pyserial requests websockets
    # ESP-IDF only if you'll be building firmware:
    # (see ~/esp/obd/HANDOFF_TO_PC.md §6 for install commands)

5.  Verify environment:
    cd ~/esp/obd/FUTV1.1
    tools/srm doctor
    # Any FAIL — fix before proceeding.

6.  Regression baseline — run all 8 prior eval gates (Phase 1 + payload):
    tools/srm validate --eval-gates-only
    Expected: feature_manager / wot_logger / dtc / vin_pairing / sbf / ui
              / mdg1_payload / mdg1_flash_orchestrator — all PASS.
    Any FAIL = migration broke something. Halt, investigate. Do NOT
    work-around a failing gate.

7.  Verify Hermes endpoint reachable:
    curl -m 5 http://192.168.1.180:3000/api 2>&1 | head -3
    If alive: optionally resume the corpus jobs (§Hermes section below).
    If dead: skip Hermes work until host is back up.

8.  Start today's status log if it doesn't exist yet:
    touch ~/esp/obd/status-$(date +%Y-%m-%d).md
    touch ~/esp/obd/file-update-$(date +%Y-%m-%d).md

9.  Tell the operator: "PC bootstrap complete. Ready for Phase 1 work."
```

Do not proceed past step 6 if any eval gate fails. Migration debugging takes precedence.

---

## Hermes corpus work (long-running, low-attention)

The corpus characterization sweep from yesterday is partially complete:

| Script | Bins done | Total | Resume status |
|---|---|---|---|
| `tools/hermes_boxcode_parser.py` | 25 | 1,352 | `.progress` file at `tools/hermes_boxcode_parser.progress` |
| `tools/hermes_corpus_catalog.py` | 10 | 3,484 | `.progress` file at `tools/hermes_corpus_catalog.progress` |

Both scripts support `--resume`. Resume on this PC once Hermes endpoint is reachable:

```bash
cd ~/esp/obd/FUTV1.1

# Verify env
echo "OPENAI_API_KEY length: ${#OPENAI_API_KEY}"
# If 0, source ~/.hermes/.env or export manually

# Confirm Hermes is up
curl -m 5 http://192.168.1.180:3000/api 2>&1 | head -3

# Launch
nohup python3 tools/hermes_boxcode_parser.py --resume > tools/hermes_boxcode_parser.stdout 2>&1 &
disown
nohup python3 tools/hermes_corpus_catalog.py --pass all --resume > tools/hermes_corpus_catalog.stdout 2>&1 &
disown
jobs
```

Both produce output incrementally to JSON files in `tools/`. Tail logs to monitor:

```bash
tail -f ~/esp/obd/FUTV1.1/tools/hermes_*.log
```

**Known operational hazards:**

- Single Hermes instance running at any time per script. If you find duplicate processes, `pkill -f hermes_corpus_catalog` and start one fresh.
- Hermes endpoint at `.180` has been intermittent (power outage took it down once). If you see persistent connection-refused errors, the host is dead; pause and check.
- Output files (`hermes_corpus_catalog_<ts>.json`) are written incrementally; you can monitor progress without stopping the script.

---

## Coordination with the Mac (don't trip the Mac's Phase 2 work)

| Topic | Discipline |
|---|---|
| Git state | Push freeze active. Neither machine pushes. Local commits fine on either machine. Reconciliation happens later via owner-led merge. |
| File ownership | Mac owns Phase 2 (`firmware/src/flash/`, related tests, Phase 2 docs). PC owns Phase 1 (everything else). Status logs are append-only; both machines append their own daily entries tagged `[PC]` or `[MAC]` for traceability. |
| Status log conflicts | Both machines may write to `status-YYYY-MM-DD.md` and `file-update-YYYY-MM-DD.md` on the same date. Tag your entries clearly: `## [PC] YYYY-MM-DD — <topic>`. Don't worry about merge conflicts in append-only logs; the owner reconciles later. |
| Cross-machine emergencies | If you spot a Phase 2 bug that the Mac doesn't know about, write it up as a new P-item in `docs/PHASE_2_PREREQUISITES.md` (one of the few cross-cutting files; both sides can append to it). Tag with `[PC]` and a note "for Mac to address." |
| Hermes scripts | The boxcode parser and corpus catalog are PC-owned now. The Mac shouldn't restart them. |

---

## Things you can work on today (after bootstrap is clean)

In rough priority order — pick based on what's most urgent and what hardware you have access to:

1. **Run a full Phase 1 HIL validation** on whatever vehicle is accessible to this machine. Procedure: `docs/CLAUDE_CODE_VALIDATE_DONGLE.md` consolidated by `tools/srm validate`. If you have a paired dongle + a vehicle (any MDG1 platform with VIN-paired dongle), you can do this independently. Hardware: dongle, Candlelight, OBD-II Y-splitter, target vehicle.

2. **Resume Hermes corpus work** (low-attention background). The 48 new fingerprints inventoried in yesterday's sweep are Phase 3 fuel — getting full coverage on the 3,484-bin archive is strategic value. Just dispatch, leave running, check back periodically.

3. **Address Phase 1 bugs / feature requests** if any landed in `~/esp/obd/status-*.md` recent entries or the project's tracker. Anything tagged Phase 1 or affecting feature_manager/wot_logger/dtc/vin_pairing/sbf/ui is your scope.

4. **Cloud server work** if `cloud/` has open items. Server-side VIN binding, token lifecycle, log upload, dashboard.

5. **UI / frontend work** if `ui/` has open items. WebSocket gauges, fault display, customer flow.

6. **Documentation cleanup** on Phase 1 surfaces. There are some doc inconsistencies flagged in P-17 (HIL preflight doc references REV2 incorrectly, SEFI reference doc has wrong pins) — these are Phase 1-adjacent and can be cleaned up here.

If none of these are pre-prioritized by the operator, ask: "What's the most pressing Phase 1 work right now?"

---

## What NOT to do (failure modes)

- **Don't touch `firmware/src/flash/`** or any Phase 2 surface area. Even read-only inspection is fine, but no modifications.
- **Don't build with `FUTUNER_PHASE2_ENABLED=1`.** Production firmware on this machine stays Phase 1.
- **Don't run `tools/srm flash --phase2` or `tools/srm capture`.** Both are Mac-owned dispatch paths.
- **Don't push to git.** Push freeze active.
- **Don't ignore failing eval gates.** If a regression test breaks, halt. The whole point of the gate is to catch this.
- **Don't restart Hermes scripts without checking for duplicate processes first.** Concurrent runs corrupt the progress file.
- **Don't append to status logs without tagging `[PC]`.** Coordination across two machines depends on traceability.
- **Don't take on Phase 2 work even if the operator asks for it during a session.** Surface the conflict: "Phase 2 is Mac-owned per the PC handoff; suggest you address that on the Mac." This protects the work-split.

---

## Reference: key constants (Phase 1-relevant)

| Constant | Value |
|---|---|
| Tester→ECU CAN ID | `0x7E0` |
| ECU→Tester CAN ID | `0x7E8` |
| Functional broadcast | `0x7DF` |
| Bitrate | 500 kbps |
| BOARD_V10 TX pin | GPIO 21 |
| BOARD_V10 RX pin | GPIO 14 |
| Hermes endpoint | `http://192.168.1.180:3000/api` |
| Hermes model | `nemo180:latest` |
| Hermes auth | `~/.hermes/.env` → `OPENAI_API_KEY` |
| Dongle AP SSID | `FUTUNER_<MAC-suffix>` (Phase 1 default) |
| Dongle AP password | `password` (default — P-19 follow-up to change) |
| Admin unlock password | `futuner_admin_2024` (hardcoded — P-20 follow-up) |
| Default dongle IP on AP | `192.168.10.1` |
| Battery floor (HIL validation) | 11.5 V |

---

## When you're done with each session

1. Append today's chip report / activity summary to `~/esp/obd/status-$(date +%Y-%m-%d).md` under `## [PC] <date> — <topic>`.
2. Append per-file deltas to `~/esp/obd/file-update-$(date +%Y-%m-%d).md`.
3. Don't commit anything.
4. If Hermes is running, leave it running (background); confirm it's still healthy via `tail -3 tools/hermes_*.log`.
5. Note any cross-machine items (Phase 2 issues you noticed, doc updates needed) in the status log clearly tagged for Mac.

---

This machine is the Phase 1 foundation going forward. The Mac is mid-Phase-2-flash; reunification happens when both sides are at stable points. Until then, work cleanly inside scope and trust the split.

Proceed to bootstrap step 1.
