# FUTUNER project handoff — Mac → PC migration

> Pickup document for the new development machine. Read this start-to-finish before touching any code or hardware on the PC. Authored at the end of a multi-day Phase 2 push that landed real production wiring and surfaced two real bugs at the HIL preflight gate. The system is in a known state; the bugs are bounded; the next move is clear after you bootstrap.

---

## 1. Start-here checklist (do these in order on the new PC)

1. **Verify the rsync landed cleanly.** Walk the tree, confirm sizes match the Mac roughly. Check that `secrets/` made it, status logs at workspace root made it, and `034_local/` (the 164 GB ECU archive) is present at the expected path.
2. **Read the hard rules.** `~/esp/obd/CLAUDE.md` (workspace router, push-freeze callout) then `~/esp/obd/FUTV1.1/CLAUDE.md` (project hard rules). Memorize the push freeze status before any git activity.
3. **Read this handoff doc completely** before touching code or hardware.
4. **Run `tools/srm doctor`** to verify the new environment (ESP-IDF, Python deps, network reach to cloud + Hermes endpoint). Fix anything red before proceeding.
5. **Run all 8 prior eval gates as a regression smoke test.** Any FAIL = the migration is broken; halt and investigate before doing anything else.
6. **Read the open bugs section (§9)** before any car-side work. Two orchestrator bugs are open; they would have failed a real CAL flash. Both have to be fixed before HIL preflight can complete green.

The hardware (dongle + Candlelight) does NOT need to be hooked up for steps 1–5. Step 6 onwards is when you go car-side.

---

## 2. Project context, quick version

**FUTUNER** is an ESP32-S3 aftermarket ECU tuning dongle for Bosch MG1/MDG1/MED17 ECUs, communicating via UDS/ISO-TP over CAN. Two phases:

- **Phase 1 (shipped, eval-gated green)**: feature manager, WOT logger, DTC read/clear, VIN pairing, SBF live tune with ethanol blending, ethanol constraints + rev limiter safety, UI WebSocket server, ethanol BLE bridge.
- **Phase 2 (under construction, gated OFF by default)**: full 8 MB binary reflash via UDS + AES-128. Built and shadow-validated; HIL preflight gate caught 2 real bugs that block real flash.

The full product spec lives at `FUTV1.1/docs/MISSION_SPEC.md`.

---

## 3. Workspace layout

| Path | Status | What it is |
|---|---|---|
| `~/esp/obd/` | active workspace | Cowork's selected folder; everything roots here |
| `~/esp/obd/FUTV1.1/` | **active** | Current build. All new work happens here. |
| `~/esp/obd/FUTV1.0/` | legacy, ref only | Predecessor branch. Reference only. Do not modify. |
| `~/esp/obd/SEFIv1/` | archived | Pre-FUTUNER project. Historical reference. Read-only. |
| `~/esp/obd/status-YYYY-MM-DD.md` | mandatory | Daily status log per workspace rule |
| `~/esp/obd/file-update-YYYY-MM-DD.md` | mandatory | Daily file-change log per workspace rule |
| `~/esp/obd/HANDOFF_TO_PC.md` | this doc | Mac → PC handoff |
| `~/esp/obd/rabbit.md` | side-project | Phase 1 HIL validation handoff doc for the rabbit@ account (separate Claude account, separate machine — not relevant on this PC unless you want to dispatch Phase 1 validation in parallel) |
| `~/esp/obd/FUTV1.1/secrets/` | sensitive | AES keys, VIN, manifests, cloud creds. Gitignored. Proprietary. |
| `/Users/rabbit/034_local/` (Mac path) → `~/034_local/` on PC | 164 GB ECU corpus archive | 3,484 .bin files across families. Used by Hermes scripts. |
| `/Users/rabbit/sniffer/` → `~/sniffer/` on PC | MM flash captures | `mm_FULL_Flash.log` (21 MB) and `mm_MAPS_upload.log` (1.5 MB). Reference data for the orchestrator's Layer 3 + Phase 4 diff. |
| `~/.hermes/.env` | API key + config | Hermes (Nemotron) endpoint creds |

---

## 4. Hard rules (carry through every prompt, every commit, every action)

| Rule | Why |
|---|---|
| 🛑 **PUSH FREEZE active** | No `git push`, no `gh pr create`, no tag pushes until owner explicitly authorizes. Local commits fine; remote pushes not. See FUTV1.1/CLAUDE.md Hard Rule 7. |
| **CAN ID 0x7E0 outbound / 0x7E8 inbound ONLY** | Any frame on another ID in production transport = halt and surface. C8 J533 gateway lockout pattern (NRC 0x10/0x12, persistent timeouts) = halt, key cycle, wait 10+ min. |
| **BOARD_V10 pinout: TX=GPIO21, RX=GPIO14** | Binary-verified per `FUTV1.0/firmware_v2/src/can/can_config.h:17-24` (DROM extraction from working v1.5 firmware). NOT REV2 — the HIL preflight doc's reference to REV2 is aspirational, not verified. Do NOT change sdkconfig from BOARD_V10. |
| **ON/OFF discipline** | All Phase 1+2 features arbitrate via `feature_manager`. Nothing executes unless explicitly enabled. Phase 2 (FUTUNER_PHASE2_ENABLED) is OFF by default. |
| **No magic numbers** | Every constant lives in a `*_config.h` with annotation "approval before lock". No hardcoded values inline. |
| **Mandatory progress logs** | Every working session appends to today's `~/esp/obd/status-YYYY-MM-DD.md` AND `file-update-YYYY-MM-DD.md`. Mandatory, not optional. |
| **Proprietary data stays local** | Bin dumps, AES keys, MM captures, VINs — never leave this machine except through the existing FUTUNER cloud server. No third-party uploads, no AI services that aren't local Hermes, no commits with sensitive data. |
| **Frozen modules** | `firmware/src/scal/`, `firmware/src/bdef/`, `firmware/src/ecu_write/` are byte-perfect from FUTV1.0. Never modify. `firmware/test/verify_frozen.sh` is the regression gate. |

---

## 5. Hardware

### Dongle

- **ESP32-S3**, 16 MB flash, 8 MB PSRAM
- **MAC: `30:ed:a0:b6:35:40`** (this is Sean's original SEFI v1.0 dongle, the one used for FUTV1.0 reverse engineering)
- **BOARD_V10**: TX=GPIO21, RX=GPIO14 (binary-verified; do NOT switch to REV2)
- **Power**: dual-fed. USB 5V from host AND OBD-II 12V regulated to 5V. Pulling either alone leaves the other powering the chip — for a true cold-boot, pull BOTH (or use the `reboot` serial command).
- **Console**: USJ primary (USB Serial JTAG over USB-CDC) since P-16 landed. UART_NUM_0 stays as secondary for ./monitor.sh compatibility.
- **Reboot mechanism**: serial command `reboot` over the USB-CDC console — esp_restart() preserves NVS. Cleaner than physical disconnect.

### Candlelight USB-CAN

- Passive sniffer for wire-level capture
- Plugged USB to the host, tapped onto CAN-H/CAN-L via OBD-II Y-splitter (parallel with the dongle)
- Driver: `gs_usb` (Python) for `tools/can_sniff.py`; `candump -L` for raw capture

### USB pass-through for WSL2

Both the dongle and the Candlelight need to be visible inside WSL2. One-time setup on Windows PowerShell as Administrator:

```powershell
winget install usbipd
# After WSL2 is running and devices are plugged in:
usbipd list                                 # find each device's BUSID
usbipd bind --busid <BUSID>                 # one-time
usbipd attach --wsl --busid <BUSID>         # attach to WSL2 (re-attach after every unplug/reboot)
```

Inside WSL2, verify with `lsusb`. The dongle should enumerate at `/dev/ttyACM0` or similar; the Candlelight should be visible to `gs_usb`.

### Dev RS7 (validation target)

- **Box code**: `4K0907557G__0003`
- **VIN**: `WUAPCBF28NN902533`
- **ECU**: Bosch MDG1 (MG1 CS002IFX RS variant)
- **Battery**: must be > 11.5 V minimum at any point in a HIL session. Programming preconditions check Bosch-side prefers > 13.0 V but for halt-before-erase preflight, 11.5 V is the floor.

---

## 6. Toolchain (what to install on PC's WSL2 Ubuntu)

```bash
# ESP-IDF (match the version pin in FUTV1.1/firmware/sdkconfig)
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && git checkout <version-from-sdkconfig>
./install.sh esp32s3
# Add to ~/.bashrc:
#   alias get_idf='. ~/esp/esp-idf/export.sh'

# Python deps
sudo apt update
sudo apt install -y python3-pip python3-venv can-utils sshpass usbutils
pip3 install --break-system-packages gs_usb pyserial requests websockets

# Verify
python3 -c "import gs_usb, serial, requests, websockets; print('ok')"
candump --help | head -1
```

Verify everything via `tools/srm doctor` — it checks env vars, tool presence, network reach to cloud + Hermes. If doctor passes, you're ready.

---

## 7. Workflow / CLI

The `tools/srm` CLI consolidated all the bench/cloud/flash/validate subcommands. Per-subcommand docs at `FUTV1.1/docs/CLAUDE_CODE_BUILD_SRM_CLI.md`.

| Command | What it does |
|---|---|
| `tools/srm doctor` | Verify environment (deps, tools, network) |
| `tools/srm flash` | Build firmware + flash dongle. Default Phase 2 OFF. |
| `tools/srm rsync` | Push cloud source + rebuild |
| `tools/srm validate` | Run all 7 validation phases (eval gates + hardware loops) |
| `tools/srm capture` | MagicMotorsport flash capture session |
| `tools/srm status` | Current state — what's flashed, what's enrolled |
| `tools/srm full` | flash + provision + validate end-to-end |

Each subcommand fails fast on missing env vars (`CLOUD_SSH_PASS`, `ADMIN_API_KEY`, `STA_SSID`, `STA_PASS`, `DONGLE_HOST`). `srm doctor` enumerates what's required.

---

## 8. Phase 2 — what's built, what's working, what's broken

### What's built and validated

| Component | State | Evidence |
|---|---|---|
| `mdg1_payload.{c,h}` (LZRB compress → AES-128-CBC encrypt → PKCS#7 pad) | byte-perfect against oracle | 52/0 host tests, verified against real RS7 CAL wire ciphertext |
| AES-128-CBC key for `4K0907557G__0003` | recovered + 6-path verified | Stored as path+offset reference in `secrets/aes_keys_per_boxcode.json`; bytes never in repo |
| `mdg1_flash_orchestrator.{c,h}` (5-section UDS sequencer) | built, transport-agnostic | 13 host scenarios green (Layer 1) |
| `mdg1_transport_can.c` (production CAN transport) | wired real, no longer stub | Quiet-bench TX-tee captured `27 11` on real silicon (Layer 2 + Step B-2) |
| `mdg1_transport_shadow.c` (host-side replay) | shadow-validated | Layer 3 byte-perfect against MM CAL section |
| Variant manifest loader | runtime-load, sha256-verified | `mdg1_variant_manifest.c`, validates fingerprint before AES key use |
| `mdg1_aes_mbedtls.c` | mbedtls iface adapter | Used in firmware build; tiny_aes used for host tests |
| Halt-before-erase mechanism | double-guarded | Primary halt + defensive secondary halt; Layer 1 scenarios 14 + 15 verify both fire |
| Coordinator arbitration (`ISOTP_OWNER_PHASE2_FLASH`) | works on real bus | Phase 3.5 connection_manager-silence check passed — P-18 CLOSED |
| USB-stdin command dispatcher (P-16) | landed | USJ primary console; `screen /dev/cu.usbmodem*` works for serial commands |
| Hermes corpus inventory (sweep results) | 3,059 bins classified, 53 unique fingerprints, 48 new | Output in `tools/proposed_manifest_merge_2026-05-12.json`, `tools/hermes_extraction_report_2026-05-12.md` |

### What's pending (P-items)

| P# | Item | Status |
|---|---|---|
| P-07 | Real-bench Phase 2 flash | 🔴 blocked by 2 open bugs (§9) |
| P-08 | Phase 2 wire protocol | 🟢 done (shadow + HIL gate-tested) |
| P-14 | ASW2/ASW3 wire-format decode (3-byte alignment gap) | 🟡 not blocking shadow validation; required before any ASW2/ASW3 real flash |
| P-15 | Recover 48 NEW AES key fingerprints (Phase 3+ ECU families) | 🔴 future scope |
| P-16 | USJ primary console + dispatcher via stdin | 🟢 DONE this session |
| P-17 | Doc cleanup — HIL preflight references REV2 (should be V10), SEFI ref doc has wrong pins | 🔴 documentation follow-up |
| P-18 | connection_manager silence during Phase 2 | 🟢 CLOSED — coordinator arbitration works |
| P-19 | Default AP password is "password" — change to non-trivial or first-boot setup | 🟡 security follow-up |
| P-20 | Admin password "futuner_admin_2024" hardcoded — move to NVS | 🟡 security follow-up |
| P-?? (filed last night) | Orchestrator missing session-control before SA | 🔴 **blocks real flash** |
| P-?? (filed last night) | Orchestrator NRC handling — bail on non-pending NRCs | 🔴 **blocks real flash** |
| P-?? (filed last night) | Shadow harness NRC modeling gap | 🔴 **blocks real flash** |

The three unfiled P-items from last night need proper numbering — assign P-21, P-22, P-23 (or next available) when you write today's status log.

---

## 9. Open bugs (blocking real CAL flash)

Caught at the HIL preflight gate on the dev RS7, May 12 evening. Halt-before-erase mechanism preserved through the failure — zero Erase frames hit the wire.

### Bug 1 — Orchestrator missing session-control step

**Symptom**: orchestrator emits `27 11` (SA seed request) in default diagnostic session. ECU responds `7F 27 12` (NRC subFunctionNotSupported-InActiveSession). SA never completes.

**Root cause**: MDG1 ECUs gate SecurityAccess sub-function 0x11 behind extended or programming session. The orchestrator currently goes INIT → SA directly, skipping the session-control step.

**Fix**: before the SA seed request, send a session-control command matching MM's wire pattern. Verify which session SID MM used by grepping `~/sniffer/mm_FULL_Flash.log` (or the `MM_Flash_Capture_Analysis.md` reference) — almost certainly `10 02` (programming session) since fingerprint write is part of the programming flow. Don't guess; verify.

**Files to touch**:
- `firmware/src/flash/mdg1_flash_orchestrator.c` — insert session control in the orchestrator's run sequence before SA
- Possibly `firmware/src/flash/mdg1_flash_orchestrator_config.h` — add a constant for the session SID

**Estimated effort**: 15–30 min.

### Bug 2 — NRC handling treats only 0x78 as pending

**Symptom**: when ECU returns a final-failure NRC (anything other than 0x78), orchestrator waits 5 seconds for "more data" before timing out, instead of bailing immediately with the NRC code.

**Root cause**: `uds_recv_skip_pending` in the orchestrator's recv path checks for `0x7F xx 0x78` to mean "keep waiting." Other NRCs fall through silently, causing the recv to time out at P2* = 5000 ms.

**Fix**: in `uds_recv_skip_pending` (or wherever the NRC dispatch is), if the response is `0x7F` and the third byte is NOT `0x78`, return immediately with the NRC code surfaced via the progress callback. Don't wait.

**Files to touch**:
- `firmware/src/flash/mdg1_flash_orchestrator.c` (or the UDS recv helper file)

**Estimated effort**: 15–30 min.

### Bug 3 — Shadow harness NRC modeling gap

**Symptom**: Layer 1's host scenarios all pass against the shadow transport's idealized protocol model — but that model never returns NRCs. Bug 1's session-gating issue was invisible to host testing.

**Root cause**: `mdg1_transport_shadow.c::synth_session_variant_response` always returns positive responses for SA, regardless of session state.

**Fix**: update `synth_session_variant_response` to model NRC behavior — if SA seed request arrives before a session-control request was processed, return `7F 27 12`. Add a Layer 1 scenario `test_sa_rejected_in_default_session_returns_nrc_12` so this regression is caught at host gate level next time.

**Files to touch**:
- `firmware/src/flash/mdg1_transport_shadow.c`
- `firmware/test/mdg1_flash_orchestrator/test_orchestrator.c` (add scenario)
- `firmware/test/mdg1_flash_orchestrator/eval.sh` (update scenario-grep list to include #16)

**Estimated effort**: 30–45 min.

### Combined fix recommendation

Land all three bugs in one prompt. Order: shadow harness update first (so Bug 1's fix is regression-testable at host), then Bug 1 and Bug 2 together with the new Layer 1 scenario as the gate. Then re-flash dongle, re-arm, re-run HIL preflight Phase 3.

---

## 10. HIL preflight resume sequence (after bugs fixed)

When all three bugs are fixed and Layer 1 is 16/0:

1. **Reflash dongle** with fixed orchestrator + `FUTUNER_PHASE2_ENABLED=1` (Phase 2 ON build). Build via `tools/srm flash` or equivalent. Verify boot log shows expected Phase 2 banner.
2. **Wire car**: dongle in OBD-II via Y-splitter, Candlelight on other leg, battery charger ON.
3. **Run Phase 1 baseline** (5-sec Candlelight sniff, key to RUN, verify allowed CAN IDs only). Per the HIL preflight doc Q4–Q5.
4. **Arm autostart** in PROD mode via USB serial command:
   ```
   screen /dev/ttyACM0 115200
   phase2_hil_preflight_arm prod
   ```
   Expected response: `{"ok":true,"armed":true,"mode":"prod"}`
5. **Trigger reboot** via the dongle's `reboot` serial command (cleaner than physical disconnect — preserves NVS arm flag).
6. **Capture continuously** via Candlelight + USB UART throughout the reboot → autostart → halt window.
7. **Verify autostart marker**:
   ```
   [PHASE2_HIL_AUTOSTART] complete — mode=prod rc=268 halt=1 erase=0
   ```
   `rc=268` is `ESP_ERR_NOT_FINISHED`. `erase=0` is the safety property.
8. **Verify wire sequence**: SA seed → SA key → fingerprint write → fingerprint ack → HALT. Zero `31 01 FF 00` frames must appear.
9. **Diff Candlelight capture** against MM reference via `tools/flash_shadow_diff.py` — protocol match (session-variant fields masked).
10. **Run Q8 CheckMemory CRC**: `31 01 02 02 45 85 0B EA`. Expect `71 01 02 02 00`. If positive, CAL on the dev RS7 matches the manifest — identical-content reflash is still identical.
11. **Print chip report** per the HIL preflight doc. Append to status log.

Only after this all-green run does the actual CAL flash become unblocked.

---

## 11. Hermes (offline-AI corpus characterization)

### Endpoint

- URL: `http://192.168.1.180:3000/api` (local LAN; Nemotron-120B hosted)
- Model: `nemo180:latest` (NOT `nemotron-120b` as some early docs say)
- Auth: Bearer token, loaded from `~/.hermes/.env` → `OPENAI_API_KEY`

### Scripts

- `FUTV1.1/tools/hermes_boxcode_parser.py` — box-code inference for the 1,352 bins where filename regex couldn't parse a part number. Resumable via `.progress` file.
- `FUTV1.1/tools/hermes_corpus_catalog.py` — three-pass comprehensive corpus characterization (per-bin profile, family clustering, reject deep-dive). Resumable.
- `FUTV1.1/tools/hermes_sweep.py` — dispatcher kept for future LLM tasks.

### State at handoff

| Script | Bins done | Total | Notes |
|---|---|---|---|
| `hermes_boxcode_parser.py` | 25 | 1,352 | ~1.85% — hit Hermes timeouts May 12 evening |
| `hermes_corpus_catalog.py` | 10 | 3,484 | ~0.29% — Hermes host went down via power outage May 13 evening |

Both have `.progress` files at `tools/hermes_*.progress` that survived. Resume on PC with `--resume`:

```bash
cd ~/esp/obd/FUTV1.1
export PYTHONPATH="$HOME/.local/lib/python3.10/site-packages:$PYTHONPATH"
# (adjust Python version per WSL2 install)

# Verify Hermes is reachable first
curl -m 5 http://192.168.1.180:3000/api/chat/completions

# Then resume
nohup python3 tools/hermes_boxcode_parser.py --resume > tools/hermes_boxcode_parser.stdout 2>&1 &
disown
nohup python3 tools/hermes_corpus_catalog.py --pass all --resume > tools/hermes_corpus_catalog.stdout 2>&1 &
disown
```

### Known Hermes issues

- Single point of failure: when `.180` goes down, both scripts hang in retry loops. Worth thinking about a watchdog or fallback eventually.
- Timeouts: per-request 120s (boxcode) / 180s (catalog). Some batches need longer; adjust if you see many timeout failures.
- Duplicate-instance bug: if you launch a script while another is already running, both write to the same progress file and races corrupt it. Always `pkill` first if uncertain.

---

## 12. Reference reading list

In priority order for a fresh agent on the new PC:

1. `~/esp/obd/CLAUDE.md` — workspace router, push freeze
2. `~/esp/obd/FUTV1.1/CLAUDE.md` — project hard rules
3. This document (you're reading it)
4. `~/esp/obd/FUTV1.1/docs/MISSION_SPEC.md` — Phase 1 + Phase 2 spec
5. `~/esp/obd/FUTV1.1/docs/PHASE_2_PREREQUISITES.md` — open P-items with status
6. `~/esp/obd/FUTV1.1/hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md` — AES key recovery chain
7. `~/esp/obd/FUTV1.1/hw_reference/MM_Flash_Capture_Analysis.md` — MM wire-protocol reference (§2.1–2.3 for preflight + SA + fingerprint)
8. `~/esp/obd/FUTV1.1/docs/HIL_PREFLIGHT_RS7_CAL_FLASH_READINESS.md` — paste-ready HIL preflight prompt with augmentation (Implementation tasks section)
9. `~/esp/obd/FUTV1.1/firmware/src/flash/mdg1_flash_orchestrator.{c,h}` — orchestrator source
10. `~/esp/obd/FUTV1.0/firmware_v2/src/can/can_driver.c` — reference for V10 hardware CAN driver wiring (used as the template for `mdg1_transport_can.c`'s implementation)
11. `~/esp/obd/status-2026-05-12.md` and `file-update-2026-05-12.md` — last working day's logs
12. `~/esp/obd/FUTV1.1/tools/hermes_extraction_report_2026-05-12.md` — corpus inventory state

---

## 13. What NOT to do (failure modes to avoid)

- **Don't real-flash until Bug 1 + Bug 2 + Bug 3 are fixed AND HIL preflight passes green.** Halt-before-erase will prevent bricking, but a real-flash attempt with these bugs in place will fail at SA and waste the session. Fix bugs first.
- **Don't push to git.** Push freeze is active. Local commits fine; `git push` / `gh pr create` / tag pushes blocked until the owner explicitly authorizes.
- **Don't modify scal/, bdef/, or ecu_write/.** They're frozen byte-perfect from FUTV1.0. `verify_frozen.sh` is the regression gate.
- **Don't switch sdkconfig from BOARD_V10 to BOARD_REV2.** The HIL doc's REV2 reference is aspirational, not binary-verified. The dongle on hand is V10.
- **Don't run Phase 2 firmware (`FUTUNER_PHASE2_ENABLED=1`) on a customer car.** Dev RS7 only until P-07 opens.
- **Don't send any captured bin/log/wire trace to external services.** Hermes is local. Nothing leaves this machine except via the FUTUNER cloud server.
- **Don't ignore mandatory progress logs.** Every working session appends to `status-YYYY-MM-DD.md` AND `file-update-YYYY-MM-DD.md`. Skipping these breaks the audit trail.
- **Don't accept the agent's "fix without diff review" pattern.** Every code change touching the orchestrator or transport_can.c should be reviewed before flash, especially the bug fixes outlined in §9. Last session, the diff-review gate caught a stub transport that would have failed at first car contact.

---

## 14. Key constants worth memorizing

| Constant | Value |
|---|---|
| Tester→ECU CAN ID | `0x7E0` |
| ECU→Tester CAN ID | `0x7E8` |
| Functional broadcast | `0x7DF` |
| Bitrate | 500 kbps |
| BOARD_V10 TX pin | GPIO 21 |
| BOARD_V10 RX pin | GPIO 14 |
| Dev RS7 box code | `4K0907557G__0003` |
| Dev RS7 VIN | `WUAPCBF28NN902533` |
| AES key fingerprint (4K0907557G__0003, sha256-first-8) | `7fa117fa` |
| AES key bin offset (IFX MDG1 family) | `0x600200` |
| Fixed Bosch IV | `00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F` |
| MDG1 CAL CRC (expected for 4K0907557G__0003) | `0x45850BEA` |
| Battery floor (HIL preflight) | 11.5 V |
| Battery preferred (real flash) | > 13.0 V |
| P2_CAN timeout (UDS recv) | 5000 ms |
| Hermes endpoint | `http://192.168.1.180:3000/api` |
| Hermes model | `nemo180:latest` |
| Dongle MAC | `30:ed:a0:b6:35:40` |

---

## 15. Resume sequence summary (TL;DR for the new PC)

```
1.  Confirm rsync landed
2.  Read this doc + CLAUDE.md files
3.  Set up WSL2 USB pass-through for dongle + Candlelight
4.  tools/srm doctor → fix any red lines
5.  Run all 8 prior eval gates → confirm regression-clean
6.  Verify Hermes endpoint up (curl)
7.  Resume Hermes corpus jobs with --resume (background)
8.  Start new status-YYYY-MM-DD.md for today
9.  Fix Bug 1 + Bug 2 + Bug 3 (see §9)
10. Re-run HIL preflight Phase 3 on dev RS7
11. If green: real CAL flash unblocked
```

---

## 16. Migration-specific concerns

### Permissions

- `secrets/` is gitignored — won't be in any git operation regardless, but verify it transferred via rsync. If missing, the AES key references break and Phase 2 won't function.
- `~/.hermes/.env` lives outside the project — make sure it transferred (or recreate with the same `OPENAI_API_KEY` value).

### Path differences

Project paths are all relative under `~/esp/obd/` — should work identically on WSL2 once the home directory layout matches. Two locations to double-check after migration:

- `~/sniffer/` (MM captures) — referenced by `flash_shadow_diff.py` with absolute paths in some places. Verify it transferred and update any absolute paths if WSL2 maps them differently.
- `~/034_local/` (164 GB archive) — referenced by `hermes_corpus_catalog.py`'s `archive_root` config and by manifest's `bin_path` entries. The script supports `FUTUNER_ARCHIVE_ROOT` env var; export it if the PC's path differs.

### Background process state

Three things were running on the Mac during migration:

1. Two `hermes_corpus_catalog.py` instances (duplicate-launch bug, both stuck in Hermes retry loop). Killed before rsync.
2. The dongle in operator-ready state (NVS arm flag self-cleared from last night's HIL Phase 3 attempt).
3. The rsync itself.

On PC arrival, none of these are running. Start fresh per the resume sequence above.

### What didn't make the migration on purpose

The Hermes job output files (`hermes_corpus_catalog_*.json`, `hermes_boxcode_parsed_*.json`) and the progress files DID rsync. So `--resume` works on PC.

---

Authored after a long Phase 2 push that landed real architecture (production transport, halt-before-erase, USB-stdin dispatcher) and surfaced real bugs at the HIL gate (the system worked — caught the bugs before any flash). The migration to PC is a clean operational reset; the project state is well-characterized and the path forward is bounded.

Don't rush the bootstrap. Walk through §1's checklist before touching anything. The discipline that got us this far was "verify before act"; that discipline starts on day one of the new machine too.
