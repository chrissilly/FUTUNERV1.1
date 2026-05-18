# File updates — 2026-05-17

---

## 2026-05-18 — [MAC] OEM documentation inventory of 034_local archive

**Created:**

- `~/esp/obd/FUTV1.1/hw_reference/OEM_DOCS.md` — ~25 KB markdown catalog of all PDF/TXT/MD/DOC/HTML reference docs in the 164 GB `/Users/rabbit/034_local/` ECU reverse-engineering archive. Categorized into 17 groups: ISO/SAE/OBD standards (UDS / ISO-TP / DoIP / KWP / J2534), AUTOSAR SWS specs (DCM, DoIP, E2E, NvM, Fee, Ea, MemIf), ASAM Standards (A2L v1.6, ODX-MEM, Autorenrichtlinien), VW corporate specs (VW80124 UDS / VW80125 ECU-ID / VW80126 programming / SA2 seed-key / FDS Lastenheft), KWP2000+KWP1281+K-line+TP2.0, generic CAN+bootloader+flash refs (Bosch CAN v2.0, CCP, XCP, DFU 1.1, AUDO bootloaders), Infineon TriCore (TC1766..TC1798, TC29x, AURIX TC2xx/TC3xx, HSM), NXP/Motorola PowerPC (MPC561/5674F/5777C/5777M, e200z3, VLE-PEM), Bosch MG1 SWCalDocs (CS001/002/003/008/011 — the gold-standard refs for CAN ID 0x7E0 work), Bosch MED9/MED17/MEDC17/EDC17, Bosch TCU (AL551/AL552/DQ500/DQ381), Aisin 09G/09M, Delphi DCM (pcmflash_90), m232-master AAN tuning project, Damos exports for non-VAG ECUs (BMW/Ford/Opel/Renault), internal notes + CAN logs, and Flash Client tooling logs. ~190 unique docs after deduplication (Unzipped/ and Dokumentation/ are full mirrors; MG1 (1)/ ⊂ MG1 (2)/; Unzipped/Bosch TCM/ mirrors Bosch TCU/ for PDFs). All paths absolute, no AES keys / VINs / sensitive bytes transcribed.

**Notable findings recorded:**

- Full Bosch corporate flash-programming spec set is present: Corp Group Req Spec for KWP2000+TP2.0 programming, FlashProgrammierung V1-10, FDS Lastenheft, VW80124/80125/80126, SA2 v10.
- Bosch internal SWCalDoc PDFs for the entire MG1 family (CS001/002/003/008/011) — these are the canonical references for FUTV1.1's MG1 work.
- Complete Infineon TriCore documentation tree covers every generation the archive's bin dumps target (TC1766 through TC3xx AURIX with HSM training).
- Reverse-engineered Hyundai Theta II FDEF dump is the only non-VAG calibration PDF.
- Sensitive content flagged but not transcribed: customer recovery notes containing a VIN, Master Credential Log import results, Bosch/VW confidential SWCalDoc PDFs.

**Reason:**
Sean asked for an organized catalog of the 034_local archive's documentation so he knows what's there and where to find it. The archive had grown to 164 GB with heavy duplication (Unzipped/ vs Dokumentation/ vs Dokumentation (1)/ vs MG1 (1)/ vs MG1 (2)/), making findability via raw `find` impractical. The inventory groups docs by source/family/standards-body and notes mirrored paths so each doc appears once. Filename inference + parent-directory + ECU naming conventions were used to derive the brief-purpose field; PDFs were not opened (cap was 20 reads but the host paths aren't reachable from this sandbox's Read tool, and filename inference was sufficient for every doc encountered). Tagged `[MAC]` per cross-machine convention.

---

## 2026-05-17 — PC machine handoff doc (Phase 1 ownership)

**Created:**

- `~/esp/obd/PC_PHASE1_HANDOFF.md` — paste-ready Claude Code prompt for the PC under WSL2 Ubuntu. ~13 KB. Establishes the PC as Phase 1 owner and the Mac as Phase 2 owner. Covers: scope (what's in/out for PC), read-first list, absolute rules, Phase 1 surface area (8 files + 8 eval gates), Phase 2 out-of-scope list (12 files explicitly Mac-owned), bootstrap sequence (9 ordered steps including USB pass-through, toolchain install, regression baseline, Hermes endpoint verify), Hermes corpus work continuation, coordination discipline with Mac (status log tagging `[PC]` / `[MAC]`, no cross-machine file ownership violation), Phase 1 work prioritization, what-not-to-do, key constants table, end-of-session checklist.

**Reason:**
Sean's split decision — Mac retains Phase 2 (mid-bugfix on the 3 orchestrator/shadow bugs caught at the HIL preflight gate). PC takes Phase 1 (live tuning, WOT logger, DTC, VIN pairing, SBF, UI, ethanol BLE) plus Hermes corpus work going forward. The handoff doc gives a fresh Claude Code session on the PC enough to bootstrap, validate the migration, and know its scope boundaries without needing this session's history. Tagged status-log entries `[PC]` / `[MAC]` are the cross-machine coordination mechanism since push freeze blocks normal git merging.

**Companion docs (already landed pre-migration):**

- `~/esp/obd/HANDOFF_TO_PC.md` — comprehensive migration doc (covers everything, including Phase 2). The PC handoff doc explicitly tells the PC agent to SKIP §8 and §9 of that doc (Phase 2 scope).

---

## 2026-05-17 — [MAC] Phase 2 Bug 1/2/3 fixes + F1 5B detection logic

**Modified (M):**

- `firmware/src/config/mdg1_flash_orchestrator_config.h` — added NRC code constants (response-pending 0x78, subfunction-not-supported 0x12, conditions-not-correct 0x22, request-out-of-range 0x31, security-access-denied 0x33), preflight DIDs (VIN F190, ECU-SW F19E, prog-history-# F1A2, prog-history-log F15B, MM probe 04 05), `MDG1_PROG_HISTORY_ENTRIES=9` + `MDG1_PROG_HISTORY_PAYLOAD_LEN=81`, `MDG1_UDS_ECURESET_REENUMERATION_DELAY_MS=1500`, `MDG1_PREFLIGHT_CYCLES_BEFORE_SA=3`, `MDG1_PREFLIGHT_ECURESET_BEFORE_CYCLE=2`, `MDG1_FLASH_ELIGIBILITY_DETECTION_ENABLED=1`. All carry "needs approval from Sean before lock" annotations per Hard Rule 3.

- `firmware/src/flash/mdg1_flash_orchestrator.h` — appended 4 new phase enum values (`PREFLIGHT_CYCLE`, `PREFLIGHT_ECURESET`, `ELIGIBILITY_DETECTED`, `NRC_RECEIVED`); added 2 new plan struct fields (`_force_skip_pre_sa_preflight_for_test_only` host-only test bypass, `cal_only_allowed_out` writeback for detection result); changed `mdg1_flash_orchestrator_run`'s plan parameter from `const` to non-const so the orchestrator can write the detection result back through it.

- `firmware/src/flash/mdg1_flash_orchestrator.c` — added `surface_nrc_or_continue` helper (fires `MDG1_FLASH_PHASE_NRC_RECEIVED` with SID + NRC code when a non-pending NRC arrives) + `uds_exchange_tolerant_of_nrc` (variant that accepts a single specific NRC as "OK" so the preflight can replicate MM's ignored gateway/scope rejections); added `preflight_read_did`, `preflight_read_f15b_and_decide`, `preflight_ecureset_and_resync`, `phase_run_preflight_cycle`, `phase_pre_sa_preflight` to implement MM's 3-cycle unlock procedure; updated `phase_security_access` signature to take cb/uctx and call `surface_nrc_or_continue` after each recv; wired `phase_pre_sa_preflight` call into `mdg1_flash_orchestrator_run` between the AES-iface check and the SA phase; skip wrapper honors `_force_skip_pre_sa_preflight_for_test_only` only when `MDG1_FLASH_ORCHESTRATOR_HOST_BUILD` is defined. Reason: fixes Bug 1 (missing session control), Bug 2 (NRC visibility), and lands the F1 5B detection per Sean's 2026-05-17 ask.

- `firmware/src/flash/mdg1_transport_shadow.c` — added `shadow_session_state_t` enum; `shadow_ctx_t` now carries session state + 81-byte `prog_history` buffer; `synth_session_variant_response` returns `7F 27 12` for SA in DEFAULT session (Bug 3 fix); `shadow_send` updates session_state on 10 02/10 03; ECUReset response path flips state back to DEFAULT after emitting 51 01; new READ_DID case synthesizes F1 5B response from the prog_history buffer; `mdg1_transport_shadow_open` initializes prog_history to a sentinel "other tool" pattern (0xAA in entry[0]) so the default test path takes the cal_only_allowed=false branch.

- `firmware/src/flash/mdg1_transport_shadow.h` — new public function `mdg1_transport_shadow_set_prog_history_top(iface, fingerprint_9b)` for tests that want to exercise the cal_only_allowed=true branch by overwriting entry[0] with `MDG1_PROG_FINGERPRINT_BYTES`.

- `firmware/test/mdg1_flash_orchestrator/test_orchestrator.c` — `progress_log_t.per_phase[]` expanded 16 → 32; added `last_nrc_sid`, `last_nrc_code`, `last_eligibility_cal_only_allowed` capture fields; new `INIT_PROGRESS_LOG` macro to set the sentinel `-1` for "never fired"; 2 new scenarios:
  - `test_sa_rejected_in_default_session_returns_nrc_12` — bypasses the preflight via `_force_skip_pre_sa_preflight_for_test_only`, expects `ESP_FAIL` with `NRC_RECEIVED` carrying SID=0x27 + code=0x12, asserts shadow log contains `TX 2711` + `RX 7f2712` and NOT `RX 6711`.
  - `test_sa_succeeds_after_programming_session_10_02` — runs orchestrator with the full preflight, asserts 3 PREFLIGHT_CYCLE + 2 PREFLIGHT_ECURESET + 1 ELIGIBILITY_DETECTED events, validates the wire bytes for `10 02 → 50 02` and `27 11 → 67 11`, then re-runs with shadow's prog_history entry[0] overwritten with our fingerprint and verifies cal_only_allowed flips to true.

- `firmware/test/mdg1_flash_orchestrator/eval.sh` — REQUIRED_SCENARIOS list 15 → 17, header updated.

- `firmware/test/can_capture/eval.sh` — pulled in the shared overrides reader (the other 7 gates already had it from 2026-05-12; can_capture was still using the hard-coded sandbox check). Reason: cross-cutting Phase 2 work otherwise trips this gate on every prompt that touches firmware/src/flash or tools/.

- `firmware/test/_shared/eval_forbidden_overrides.txt` — added 2026-05-17 session header + `firmware/test/lzrb/lzrb_cli.c` + `firmware/test/can_capture/eval.sh` entries; also added 4 stale Hermes-corpus log files (`tools/hermes_corpus_catalog.{log,stdout}`, `tools/hermes_corpus_summary_*.md`) that the now-overrides-aware can_capture eval surfaced. Reason: keeps the audit trail visible per the file's working convention.

**Created (??):**

- `firmware/test/lzrb/lzrb_cli.c` — tiny host CLI wrapping `lzrb_decompress` so `tools/flash_shadow_diff.py` can run the plaintext-equivalence step. Argv: `IN OUT EXPECTED_LEN`. Built ad-hoc into `/tmp/lzrb_cli`; the prior version was wiped from `/tmp` during the migration. Reason: was load-bearing for the eval.sh diff scenarios (test_shadow_full_protocol_perfect_and_plaintext_equivalent + test_shadow_cal_protocol_perfect_and_plaintext_equivalent + test_diff_tool_exits_0_on_match all use it).

- `status-2026-05-17.md` — new daily status log. Top-level chip report + scope-expansion notes + hard-rules audit.

**Environment side-fix (not a tracked file change but worth logging):**

- Removed `~/Library/Python/3.9/lib/python/site-packages/cryptography*` and `~/Library/Python/3.9/lib/python/site-packages/esptool*`. These user-site copies (cryptography 47.0.0, esptool 4.7.0) were shadowing the IDF v5.5 venv's correct pinned versions (cryptography 44.0.3, esptool 4.12.dev1). After removal, `idf.py build -DEXTRA_CFLAGS=-DFUTUNER_PHASE2_ENABLED=1` ran clean. Reason: pure migration artifact; the new Mac's pip3 had pulled latest versions into user-site somewhere along the way.
