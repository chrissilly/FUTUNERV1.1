
---

## 2026-05-12 (late, pre-migration) — Mac → PC handoff doc

**Created:**

- `~/esp/obd/HANDOFF_TO_PC.md` — comprehensive pickup document for the project's migration to the Windows PC (via WSL2 + Ubuntu). 16 sections covering: start-here checklist, project context, workspace layout, hard rules, hardware (dongle V10 binary-verified, Candlelight, USB pass-through via usbipd-win, dev RS7 specs), toolchain install, `tools/srm` CLI workflow, Phase 2 milestone state, open bugs (the 3 caught at the HIL gate last night), HIL preflight resume sequence, Hermes endpoint state + scripts, reference reading list, what-not-to-do, key constants memorization table, resume TL;DR, migration-specific concerns. ~28 KB.
- Reason: project is migrating Mac → PC mid-session-cycle. A self-contained handoff doc gives the new agent (or a fresh Claude Code session on the PC) enough to bootstrap without losing context on hard rules, current state, open bugs, or operational discipline. Captures all P-items, all key constants, the dev RS7 details, and the resume order.

---

## 2026-05-12 — HIL preflight prompt augmented with Implementation tasks

**Modified:**

- `FUTV1.1/docs/HIL_PREFLIGHT_RS7_CAL_FLASH_READINESS.md`: added a new `## Implementation tasks (write these BEFORE running Q1–Q8)` section between the "Read first" list and "Pre-decided choices." Spells out the four code changes the fresh agent must write before the preflight can actually run: (1) HIL halt gate in the orchestrator + config header + phase enum, (2) serial-command handler `phase2_hil_preflight` that invokes the orchestrator with feature_manager enable/disable around it, (3) candump-to-shadow-log Python adapter for diffing Candlelight capture against MM, (4) explicit decision tree for Q8's CheckMemory approach (default session → extended session `10 03` → fallback to `22 F1 9E` + `22 F1 A2` sanity reads; never enter programming session `10 02` for a read-only check). Also added a `## Mandatory progress logs` block restating Sean's standing rule (status + file-update logs) so a fresh agent with no prior session context still knows to append.
- Reason: original doc had strategic context (goal, abort conditions, acceptance criteria) but was 70% sufficient for a fresh agent — the 30% gap was the code work itself (≈250 LOC across 4 files). Augmentation makes the agent's first 30 min deterministic instead of exploratory, removing the "agent asks mid-flight clarifying question while elbow-deep in CAN wiring" failure mode.

---

## 2026-05-12 — Hermes long-running archive work scripts

**Created:**

- `FUTV1.1/tools/hermes_boxcode_parser.py` — fire-and-forget script for inferring box codes for the 1,352 bins where filename regex couldn't parse one. Batches of 25, resumable via progress file, incremental JSON output. Reads `tools/proposed_manifest_merge_2026-05-12.json`, filters entries with null `box_code`, sends filename + parent dir + embedded ASCII strings (from regions around the known key offsets) to Hermes. Estimated runtime: ~15–30 min on the full 1,352. Pure pre-decided LLM dispatch, no agent needed mid-loop.

- `FUTV1.1/tools/hermes_corpus_catalog.py` — three-pass comprehensive corpus characterization. Pass 1 produces per-bin profiles (ECU family, vehicle inference, SW/cal versions, tool signature, dump type, anomalies) across all 3,521 archive bins. Pass 2 clusters by family and identifies canonical-vs-derivative dumps, twin bins, version progression. Pass 3 deep-dives the 462 rejects to propose non-documented key offsets via entropy scanning + Hermes interpretation. Each pass independently runnable + resumable. Estimated total runtime: 6–12 hours unattended. Outputs JSON per pass plus a Markdown summary rollup.

**Reason:**
Hermes/Nemotron is free compute for us (locally hosted, no per-token cost), so the right move is to give it interpretive work where LLM judgment actually matters — that's a multi-hour archive characterization rather than the trivially-deterministic key-classification work we already did locally. This produces real strategic assets: a complete ECU corpus catalog, family clustering, and candidate key offsets for unknown-family bins.

**Operational pattern (fire-and-forget):**
```
cd ~/esp/obd/FUTV1.1
tmux new -s catalog
nohup python3 tools/hermes_corpus_catalog.py --pass all > /dev/null 2>&1 &
# Ctrl-b d to detach
```

Both scripts honor CONFIG-at-top + no magic numbers per workspace conventions. Both write progress and log files for unattended runs. Neither modifies any `secrets/*` file or commits anything (push freeze respected).

---

## 2026-05-12 — Phase 1 HIL validation handoff doc

**Created:**

- `~/esp/obd/rabbit.md` — self-contained handoff doc for `rabbit@sillyrabbitmotorsport`'s Claude account to execute Phase 1 hardware-loop validation on a separate host machine. Contains: context summary, host prerequisites, hardware preconditions, the full paste-ready validation prompt (8 phases, three-stream monitoring, chip report template), and operator-side notes. Approx 18 KB.
- Reason: validation handoff to a different Claude account on a different machine. The doc is intentionally self-contained so the receiving agent has zero prior session context yet enough to bootstrap from `tools/srm doctor` through to the per-phase chip report.
- Owner action expected: rsync (Mac → Linux/WSL2) or USB-tar (Mac → PC) the workspace folder excluding Phase 2 keys + `vag_mdg1_drive_pull/`, then move `rabbit.md` to wherever the receiving agent will look for it (home dir, project root, etc.).

---

## 2026-05-12 — Push freeze activated

- `FUTV1.1/CLAUDE.md`: added Hard Rule 7 (🛑 PUSH FREEZE) — no `git push`, no `gh pr create`, no tag pushes until owner explicitly lifts. Local commits remain allowed and encouraged.
- `~/esp/obd/CLAUDE.md` (workspace router): added one-line freeze callout pointing at Hard Rule 7.
- `FUTV1.1/secrets/AES_KEYS_MASTER.md`: added freeze callout in the header.
- Reason: recent AES key recovery work (Phase 2 flash) needs owner audit before anything leaves the local machine.

---

## Late-2026-05-12 — Orchestrator + shadow-validation landing

**Created:**

- `firmware/src/flash/mdg1_flash_orchestrator.{c,h}` — 5-section UDS sequencer, transport-agnostic
- `firmware/src/flash/mdg1_uds_transport.h` — function-pointer transport iface
- `firmware/src/flash/mdg1_transport_shadow.{c,h}` — host-side log + replay impl
- `firmware/src/flash/mdg1_transport_can.c` — production transport, dormant (no init call from anywhere)
- `firmware/src/flash/mdg1_aes_mbedtls.c` — production mbedtls AES iface adapter for `mdg1_payload`
- `firmware/src/flash/mdg1_variant_manifest.{c,h}` — minimal hand-rolled JSON loader + SHA-256 key-fingerprint validator
- `firmware/src/config/mdg1_flash_orchestrator_config.h` — all wire-protocol constants + timing budgets
- `firmware/test/mdg1_flash_orchestrator/test_orchestrator.c` — 13 named scenarios, 32 P / 1 SKIP / 0 F
- `firmware/test/mdg1_flash_orchestrator/Makefile` — host build
- `firmware/test/mdg1_flash_orchestrator/eval.sh` — 12-section graded harness, 63 P / 0 F
- `firmware/test/can_capture/fixtures/magicmotorsport/SUMMARY.md` — canonical-capture map + masking + per-section status
- `firmware/test/can_capture/fixtures/expected_responses_4K0907557G_0003.json` — 1015 TX/RX pairs extracted from `mm_FULL_Flash.log` by the new extractor
- `firmware/test/_shared/eval_forbidden_overrides.txt` — per-prompt allowlist for cross-cutting FORBIDDEN edits
- `tools/extract_mm_expected_responses.py` — MM log → JSON fixture extractor (stdlib only)
- `tools/flash_shadow_diff.py` — protocol-perfect + plaintext-equivalent diff (stdlib + `cryptography` + `lzrb_cli`)

**Modified:**

- `firmware/src/flash/mdg1_flash.c` — `mdg1_flash_transfer_data` now emits already-packed ciphertext; ad-hoc per-chunk AES dropped (orchestrator-level `mdg1_payload_pack` is the proper path)
- `firmware/src/main.c` — Phase 2 init under `#if FUTUNER_PHASE2_ENABLED` (registers mbedtls AES iface; feature_manager registration deferred)
- `firmware/src/CMakeLists.txt` — added 8 new flash sources (mdg1_payload, mdg1_variant_manifest, mdg1_transport_shadow, mdg1_transport_can, mdg1_aes_mbedtls, mdg1_flash_orchestrator, lzrb, mdg1_flash) + flash/ to INCLUDE_DIRS
- `secrets/mdg1_variant_manifest.json` — added `4K0907557G 0003` to `boxcode_index`; extended `MG1 CS002IFX RS` variant with `flash_sections_by_boxcode` (5 entries) + `plaintext_source_by_boxcode`
- `docs/PHASE_2_PREREQUISITES.md` — P-08 🔴 → 🟡 with orchestrator landing note
- 6 prior eval.sh scripts (feature_manager, wot_logger, dtc, vin_pairing, sbf, ui) + mdg1_payload/eval.sh — all updated with the override-reader; same code in each

---

## Hermes extraction sweep + ECU key corpus inventory

**Created:**
- `tools/hermes_sweep.py` — variant key-extraction dispatcher (Option B architecture: local pre-scan + Hermes for ambiguous cases only; this run skipped Hermes since pre-scan was decisive)
- `tools/proposed_manifest_merge_2026-05-12.json` — 3,059 candidate manifest entries, fingerprints only (no key bytes), with regex-parsed box codes + family inference
- `tools/hermes_extraction_report_2026-05-12.md` — full methodology + fingerprint distribution + 48-NEW table + 462-reject categorization + smoke test result
- `hw_reference/ecu_key_corpus_2026-05-12/README.md` — corpus overview, scope, methodology
- `hw_reference/ecu_key_corpus_2026-05-12/key_fingerprint_inventory.json` — machine-readable per-fingerprint inventory
- `hw_reference/ecu_key_corpus_2026-05-12/key_fingerprint_inventory.md` — human-readable inventory table grouped by ECU family
- `firmware/test/bin_inventory.md` — per-bin (3,521 rows) pre-scan: sizes + offset SHA-256[:8] + entropy at both candidate offsets, NO raw bytes
- `docs/HIL_PREFLIGHT_RS7_CAL_FLASH_READINESS.md` — paste-ready prompt for the next-session HIL preflight gate on the dev RS7

**Modified:**
- `docs/PHASE_2_PREREQUISITES.md` — added P-15 entry (recover 48 new key bytes); existing P-14 preserved

**Untouched (per spec):**
- `secrets/AES_KEYS_MASTER.md`, `secrets/aes_keys_per_boxcode.json`, `secrets/mdg1_variant_manifest.json` — proposal only, no merge
- All firmware sources
- All prior eval scripts (regression check confirmed green)

---

## Three-layer HIL preflight gate (Layer 1/2/3)

**Created:**
- `firmware/src/flash/phase2_hil_autostart.h` — NVS-armed one-shot HIL preflight runner API. arm/run_if_armed/is_armed.
- `firmware/src/flash/phase2_hil_autostart.c` — implementation. Reads + CLEARS the NVS flag before invoking the orchestrator (one-shot, crash-safe), runs shadow preflight, dumps log over UART base64 between markers.
- `firmware/src/commands/phase2_hil_preflight_commands.h` — public signatures for cmd_phase2_hil_preflight + cmd_phase2_hil_preflight_arm.
- `firmware/src/commands/phase2_hil_preflight_commands.c` — WS/serial command handlers. JSON envelope: ok, log_path, rc, events, halt_events_seen, erase_events_seen.

**Modified:**
- `firmware/src/config/mdg1_flash_orchestrator_config.h` — added MDG1_HIL_PREFLIGHT_HALT_BEFORE_ERASE compile-time gate, MDG1_HIL_NVS_NAMESPACE/KEY/AUTOSTART_LOG_*_PATH/MARKER constants. All "needs approval from Sean before lock" annotated.
- `firmware/src/flash/mdg1_flash_orchestrator.h` — added MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE enum value (appended, did not renumber) + mdg1_flash_plan_t::hil_halt_before_erase runtime field.
- `firmware/src/flash/mdg1_flash_orchestrator.c` — inserted halt gate between fingerprint phase and per-section loop. Fires when compile-time flag set OR plan->hil_halt_before_erase=true. Returns ESP_ERR_NOT_FINISHED + emits MDG1_FLASH_PHASE_HIL_HALT_BEFORE_ERASE progress event.
- `firmware/src/flash/mdg1_uds_transport.h` — added ESP_ERR_NOT_FINISHED (0x10C) to the host shim.
- `firmware/src/flash/mdg1_transport_shadow.c` — replaced five 8204-byte stack-alloc hex buffers with heap helper `hex_alloc_and_canon` to fix main_task stack overflow on ESP32-S3. Host build still works (heap is fine there too).
- `firmware/src/main.c` — wired phase2_hil_autostart_run_if_armed() between Phase 2 AES iface register and "System initialized". Includes bench-only PHASE2_HIL_AUTOSTART_FORCE_ARM_THIS_BUILD guard.
- `firmware/src/CMakeLists.txt` — added phase2_hil_autostart.c + phase2_hil_preflight_commands.c to app_sources. Bench helper target_compile_definitions left commented-out as a toggleable one-liner.
- `firmware/src/config/futuner_config.h` — flipped FUTUNER_PHASE2_ENABLED default from 0 to 1 for the bench preflight build (kconfig was the simplest propagation path; `-DEXTRA_CFLAGS` did not propagate to component sources).
- `firmware/src/commands/commands.c` — registered phase2_hil_preflight + phase2_hil_preflight_arm in COMMAND_REGISTRY (both CMD_SECURITY_SECURED).
- `firmware/src/commands/serial_console.c` — added local case handlers for phase2_hil_preflight and phase2_hil_preflight_arm. (Currently not reachable from USB-CDC due to P-16; included for symmetry once console wiring is fixed.)
- `firmware/test/mdg1_flash_orchestrator/test_orchestrator.c` — added scenario test_hil_preflight_halt_before_erase_no_erase_emitted. Asserts halt return, fingerprint frames present, no erase frame, halt event fires once.
- `firmware/test/mdg1_flash_orchestrator/eval.sh` — added the new scenario name to REQUIRED_SCENARIOS (14 total, was 13).
- `docs/PHASE_2_PREREQUISITES.md` — added P-16 (USB-Serial-JTAG primary console for interactive HIL commands).

---

## Pre-go-HIL prod transport prep (Step A + B-1 + B-2)

**Created:**
- `firmware/src/flash/mdg1_transport_can.h` — public open/close signatures for the production CAN transport.

**Modified:**
- `firmware/src/isotp_coordinator/isotp_coordinator.h` — added `ISOTP_OWNER_PHASE2_FLASH` enum value (new owner kind).
- `firmware/src/flash/mdg1_transport_can.c` — replaced the unconditional-INVALID_STATE stubs with real wiring: `can_manager_send_isotp` for TX, poll-loop on `can_manager_receive_isotp` for RX. Coordinator acquire on open, release on close. Added UDS-level TX-tee `MDG1_TX_CAN: TX 0x7E0 len=N <hex>` so wire bytes can be inspected from the boot log without an external sniffer.
- `firmware/src/config/mdg1_flash_orchestrator_config.h` — added `MDG1_TRANSPORT_CAN_RECV_POLL_INTERVAL_MS=10`, `MDG1_TRANSPORT_CAN_COORDINATOR_TIMEOUT_MS=3000`, `MDG1_TRANSPORT_CAN_TEE_LOG_HEX_BYTES=32`, `MDG1_HIL_AUTOSTART_LOG_PROD_{ABS,FS}_PATH` constants. All annotated "needs approval from Sean before lock."
- `firmware/src/flash/mdg1_flash_orchestrator.h` — added host-build-only `_force_skip_primary_halt_for_test_only` plan field (gated `#ifdef MDG1_FLASH_ORCHESTRATOR_HOST_BUILD`).
- `firmware/src/flash/mdg1_flash_orchestrator.c` — defensive-secondary halt block at top of per-section loop. Fires on either compile-time OR runtime HIL flag. Emits FAILED progress + ESP_ERR_INVALID_STATE + screaming "DEFENSIVE HALT" to stderr.
- `firmware/src/flash/phase2_hil_autostart.h` — added `phase2_hil_mode_t` enum (NONE/SHADOW/PROD) + `phase2_hil_autostart_arm_with_mode()`. `phase2_hil_autostart_arm()` kept as alias for shadow mode.
- `firmware/src/flash/phase2_hil_autostart.c` — added `run_prod_preflight()` paralleling `run_shadow_preflight()` but opening `mdg1_transport_can` instead of `mdg1_transport_shadow`. NVS flag semantics promoted from bool to mode enum. Dispatch in `run_if_armed` branches on mode. Prod-mode acceptance allows ESP_ERR_TIMEOUT/ESP_FAIL on quiet bench.
- `firmware/src/commands/phase2_hil_preflight_commands.c` — `cmd_phase2_hil_preflight_arm` accepts "shadow"/"prod" string in params (default shadow). Returns mode in JSON response.
- `firmware/src/main.c` — added `PHASE2_HIL_AUTOSTART_FORCE_ARM_PROD_THIS_BUILD` bench-only helper mirroring the existing shadow helper.
- `firmware/src/CMakeLists.txt` — added the prod-mode FORCE_ARM toggle line (commented out for customer firmware).
- `firmware/src/can/can_driver.c` — added diagnostic logging at init+start: pins, bitrate, mode, filter, TWAI state. Makes board misconfigurations visible in boot log.
- `firmware/test/mdg1_flash_orchestrator/test_orchestrator.c` — added scenario `test_hil_defensive_secondary_engages_when_primary_bypassed`. Sets `_force_skip_primary_halt_for_test_only=true` + `hil_halt_before_erase=true`, expects the secondary halt to catch.
- `firmware/test/mdg1_flash_orchestrator/eval.sh` — REQUIRED_SCENARIOS list updated to 15 entries.
- `docs/PHASE_2_PREREQUISITES.md` — added P-17 (doc cleanup: HIL preflight doc refs BOARD_REV2 + SEFI HW reference has wrong CAN pins).
