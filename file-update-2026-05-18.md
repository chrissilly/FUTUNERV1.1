# File updates — 2026-05-18

---

## 2026-05-18 — [MAC] Hermes PDF summarizer for OEM_DOCS.md backfill

**Created:**

- `~/esp/obd/FUTV1.1/tools/hermes_pdf_summarizer.py` (929 lines, pyflakes clean) — host-side tool that backfills the filename-inferred catalog at `hw_reference/OEM_DOCS.md` with actual PDF content summaries via Hermes (Nemotron-120B at 192.168.1.180:3000). Architecture: parses OEM_DOCS.md to extract (filename, category) pairs, glob-resolves each filename to the real on-disk path in `/Users/rabbit/034_local/`, applies a hard-coded sensitive-skip gate (customer-recovery VIN notes, Master Credential Log, Bosch/VW confidential SWCalDocs — never sent to Hermes), extracts text with pdfplumber (preferred) or pypdf/PyPDF2 (fallback) using a head+mid+tail strategy for >30-page PDFs (capped at 20 KB), dispatches to Hermes for structured JSON summarization, writes incremental crash-safe JSON, and generates `hw_reference/OEM_DOCS_DETAILED.md` as a companion catalog organized in the same section order as OEM_DOCS.md. Modes: `--mode pdf` (default), `--mode txt` (second pass for internal .txt notes, with extra skips for seed/key constants under hard rule 5). Flags: `--dry-run`, `--resume`, `--limit N`, `--merge-only`. Output: `tools/hermes_pdf_summaries_<ts>.json` (PDF pass) or `tools/hermes_txt_summaries_<ts>.json` (txt pass); progress at `tools/hermes_pdf_summarizer.progress` and `tools/hermes_txt_summarizer.progress`; log at `tools/hermes_pdf_summarizer.log`.

**Modified:**

- `~/esp/obd/FUTV1.1/tools/README.md` — added `hermes_pdf_summarizer.py` to the script table (along with `hermes_corpus_catalog.py` and `hermes_boxcode_parser.py` which were previously undocumented in the README); new "hermes_pdf_summarizer.py — quick start" section with smoke-test, full-sweep, resume, and merge-only invocations; documented the sensitive-skip gate's authoritative location (`CONFIG["sensitive_path_prefixes"]` and `CONFIG["sensitive_filename_patterns"]` at the top of the script, citing OEM_DOCS.md "Sensitive content notes"); added `requests` and `pdfplumber` to the dependency list.

**Acceptance checks (all green):**

- `python3 -m pyflakes tools/hermes_pdf_summarizer.py` — clean.
- `bash firmware/test/verify_frozen.sh` — `FROZEN MODULE CHECK: PASS (6 files match baseline)`. No firmware changes.
- Sensitivity unit tests (14 cases, inline smoke): all pass. DMG1\d/D-MG1-/VAG_MG1CS/SWCalDoc-substring/Freigabe/FL_/EV_ECM filename patterns + customer-recovery path prefix + VAG-Seed-Key/MED9-Seed-Key txt-mode-extras all correctly gate. ISO standards / Infineon datasheets / pcmflash modules correctly pass through.
- Parser smoke: parses 17 sections / 200 rows from OEM_DOCS.md; preserves section order. `discover_docs("pdf")` resolves 136 unique PDFs on disk; 11 are sensitive-skipped (the MG1 SWCalDoc set + Freigabe + FL_/EV_ECM corporate flash docs — matches the OEM_DOCS.md flagged list).
- End-to-end `--dry-run --limit 5` from `~/esp/obd/FUTV1.1/`: discovered all 136 PDFs in ~7 seconds (rglob over 164 GB), extracted 5 sample PDFs successfully (mix of `head_mid_tail` for >30 page docs and `all` for small docs), wrote a valid JSON output with all expected metadata fields (`_path`, `_category`, `_mirror_paths`, `page_count`, `extracted_bytes`, `extraction_strategy`, `extraction_backend`, `_processed_at`, plus `dry_run`/`would_send_bytes` flags). `--merge-only` then generated a well-structured `OEM_DOCS_DETAILED.md` with header + tallies + per-category sections in OEM_DOCS.md order. Test artifacts cleaned up afterwards.

**Reason:**

The OEM doc inventory at `hw_reference/OEM_DOCS.md` (built earlier today by a sub-agent) is filename-inferred — the cataloguing agent couldn't read PDF contents from its sandbox, so the brief-purpose column is best-effort from filename + parent-dir + standards-body conventions. With Hermes available locally at `192.168.1.180:3000/api`, we now have a path to deepen those summaries to real content abstracts without exfiltrating any data off the local machine. The tool maintains the existing sensitivity gate from OEM_DOCS.md — customer recovery / master credential / Bosch SWCalDoc / Freigabe / FL_/EV_ECM PDFs get a stub-only entry with content NEVER leaving the file. Phased: PDFs first (`--mode pdf` default), then `--mode txt` for the internal RE notes that are higher-value but partially gated.

**Notes for the owner:**

- The script's 929-line length is above CLAUDE.md's ~500-line "smell" threshold. The dual-mode (PDF + TXT) + extraction-backends + Hermes-call + crash-safe-resume + markdown-merge concerns are all single-file in `hermes_corpus_catalog.py` (655 lines) so the convention is "tool scripts are larger." If a split is wanted, the natural seams are extract/discover/hermes/merge modules — flag if you'd like that pass.
- CONFIG block top-of-file is annotated "approval needed" for: request_timeout_sec=240, max_retries_per_pdf=3, retry_backoff_sec=5, max_tokens_per_response=4096, extracted_text_cap_bytes=20480, small_pdf_page_threshold=30, large_pdf_head_pages=5, large_pdf_tail_pages=2, large_pdf_mid_sample_pages=1. Defaults chosen to match `hermes_corpus_catalog.py` where applicable.
- Tag `[MAC]` per cross-machine convention.

---

## 2026-05-18 — [MAC] NRC handling fixes — pending-loop + post-SA NRC surface

**Modified:**

- `firmware/src/config/mdg1_flash_orchestrator_config.h` (main tree) — added 2 constants: `MDG1_UDS_RX_STACK_SMALL_BYTES = 32u` (stack buffer for control-message exchanges in the new `uds_exchange_strict` helper) and `MDG1_SHADOW_PENDING_INJECT_SLOTS = 8u` (max independent SIDs the shadow can pending-inject for).

- `firmware/src/flash/mdg1_flash_orchestrator.c` (main tree) — added new internal helper `uds_exchange_strict` that bundles `send_request → uds_recv_skip_pending → surface_nrc_or_continue → uds_assert_positive` with optional `out_rx` for callers needing the response bytes. Refactored 9 call sites: `phase_security_access` (SA seed + key), `preflight_ecureset_and_resync` (the pre-SA ECUReset), `phase_fingerprint`, `phase_section_erase`, `phase_section_request_download`, `phase_section_transfer_data` per-chunk, `phase_section_transfer_exit`, `phase_section_check_memory`, `phase_check_prog_deps`, `phase_ecu_reset` (final closeout). Threaded `mdg1_flash_progress_cb_t cb, void *uctx` through 7 phase function signatures that didn't previously accept them; updated all call sites in `mdg1_flash_orchestrator_run` to pass `cb, uctx`.

- `firmware/src/flash/mdg1_transport_shadow.{c,h}` (main tree) — added pending-NRC injection API: `mdg1_transport_shadow_inject_pending(iface, sid, count)` arms the shadow to emit `7F <sid> 78` (RCRRP) for the next `count` requests with that SID before falling through to the normal response synthesis. Up to `MDG1_SHADOW_PENDING_INJECT_SLOTS` distinct SIDs can be armed simultaneously. Shadow_recv path checks the per-SID slot at the start of each call and decrements; the orchestrator's `uds_recv_skip_pending` loop consumes the injected pendings transparently. Models the dev-RS7 capture's observed 6× `7F 11 78` on ECUReset (2 per reset × 3 resets).

- `firmware/test/mdg1_flash_orchestrator/test_orchestrator.c` (main tree) — added 2 new named scenarios at the end of `main()`:
  - `test_orchestrator_handles_pending_before_positive` — arms `inject_pending(MDG1_UDS_SID_ECU_RESET, 2)` then runs the full preflight + SA + fingerprint pipeline; asserts orchestrator reaches HIL halt (not bail-on-pending). Counts shadow-log `RX 7f1178` occurrences and expects exactly 2; confirms `RX 5101` follows. 9 EXPECTs.
  - `test_post_sa_nrc_fires_progress_event` — arms `inject_pending(MDG1_UDS_SID_WRITE_DID, 100)` to overflow the orchestrator's 8-iteration `uds_recv_skip_pending` cap; asserts `ESP_ERR_TIMEOUT` propagates correctly and no SECTION_ERASE event fires. Counts shadow-log `RX 7f2e78` occurrences and expects ≥ 8. 8 EXPECTs.

- `hw_reference/NRC_ERROR_HANDLING_AUDIT.md` (main tree, copied from worktree first) — prepended "Status — fixes landed 2026-05-18" section that table-maps each of the 2 🔴 findings to its implementation (file:line) and its verifying test name. Updated the front matter to reflect that the audit is no longer read-only.

**Copied (worktree → main tree):**

- `hw_reference/NRC_ERROR_HANDLING_AUDIT.md`
- `hw_reference/UDS_MG1_FLOW_CROSSREF.md`

(These were originally written in the worktree but the file:line citations target the main tree's 884-line orchestrator. Main is now the authoritative location for both.)

**Eval-gate results:**

| Gate | Pre-fix | Post-fix |
|---|---|---|
| `host_test_runner` | 82 PASS, 1 SKIP, 0 FAIL | **101 PASS**, 1 SKIP, 0 FAIL |
| `mdg1_flash_orchestrator/eval.sh` | PASS 67/67 | **PASS 67/67** |
| `verify_frozen.sh` | PASS | **PASS** (6 files match baseline) |

**Reason:**

The NRC audit caught two real bugs that would have failed at the next HIL Phase 3 dispatch:

1. **Pending-loop gap (confirmed-failing).** Four call sites (`preflight_ecureset_and_resync`, `phase_fingerprint`, `phase_section_request_download`, `phase_ecu_reset`) used `uds_exchange + uds_assert_positive` without looping on `7F xx 78` RCRRP. MM's dev-RS7 capture shows 6× `7F 11 78` (2 per reset × 3 ECUResets) — meaning sites #1 and #4 would have failed the assert on the first pending and aborted with "reset failed". The shadow harness had been masking this because it always emitted positive responses synchronously without pending.

2. **Post-SA NRC silence.** Eight post-SA phases called `uds_assert_positive` without `surface_nrc_or_continue` first, so any non-pending NRC bailed with a generic message ("erase failed", "td failed") instead of firing `MDG1_FLASH_PHASE_NRC_RECEIVED` with the actual SID + NRC byte. Operator would have lost 1-2 hours per failure mode of bench-debug time.

The fix routes everything through one new helper (`uds_exchange_strict`) that does the right thing by construction. Threading `cb`/`uctx` through 7 phase signatures was the cost; the upside is the orchestrator's NRC handling is now uniform across the entire flash path, and the shadow harness can reproduce the wire-observable pending-burst patterns that MM captured.

**Not committed:** push freeze is lifted but no commits were made this turn — owner can review the diff and commit when ready.

[MAC]

---

## 2026-05-18 — [MAC] NRC error-handling audit

**Created:**

- `~/esp/obd/FUTV1.1/hw_reference/NRC_ERROR_HANDLING_AUDIT.md` (~38 KB) — comprehensive audit of MG1 flash orchestrator's NRC handling. Catalogues the full ISO 14229-1 §A.1 NRC table (43 codes), cross-correlates with MM dev-RS7 wire capture (10 distinct NRCs observed, 67 NRC frames total), maps each NRC to VW80126's per-service guidance, audits the orchestrator's 4 NRC-handling code paths (`uds_assert_positive`, `uds_recv_skip_pending`, `uds_exchange_tolerant_of_nrc`, `surface_nrc_or_continue`), and produces a prioritized fix list.

**Key findings:**

- 🔴 **2 critical pre-HIL-3 issues** (NEW — not surfaced in the cross-ref doc):
  1. **Pending-loop gap on 4 bare-`uds_exchange` sites** (preflight ECUReset line 271, fingerprint write line 476, RequestDownload line 522, final ECUReset line 640). These call `uds_exchange + uds_assert_positive` without looping on `7F xx 78` RCRRP. **Sites 1 & 4 are confirmed-failing**: MM observed `7F 11 78` 6 times during the 3 ECUReset events, meaning our current orchestrator would receive the pending NRC, fail `uds_assert_positive`, and abort the flash with "reset failed" before the ECU even sends its final response. This is a regression of P-04 (pre-flash safety gate hardened) and P-08 (eval harness green); the shadow harness needs to emit pending-pre-positive to reproduce the bug.
  2. **Post-SA NRCs go un-surfaced** — Bug 2 fix (2026-05-17) wired `surface_nrc_or_continue` into the SA path + F1 5B read but didn't extend to the 8 post-SA flash phases (fingerprint, erase, RequestDownload, TransferData, TransferExit, CheckMemory, CheckProgDeps, final reset). Real-bench failures would surface as generic "td failed" / "erase failed" without the actual NRC byte being shown. Doesn't affect correctness; significantly increases bench-debug time per failure mode.

- 🟡 **3 moderate fixes for P-07** (real-bench Phase 2):
  - TransferData NRC 0x73 wrongBlockSequenceCounter retry-with-same-BC (ISO §A.1 permits, we don't)
  - NRC 0x72 generalProgrammingFailure dedicated progress event class (signals dying flash chip)
  - VW80126 §5.1.3 pre-programming hygiene (ControlDTCSetting + CommunicationControl)

- 🟢 **2 nice-to-haves** (SA lockout UX, customer-facing NRC docs)

**MM NRC inventory (during successful flash):**
- 30× `7F 22 78` (ReadDID pending)
- 15× `7F 22 31` (ReadDID requestOutOfRange — DID 0x0405 probe, tolerated ✅)
- 13× `7F 31 78` (RoutineControl pending)
- 6× `7F 11 78` (ECUReset pending — the critical one our orchestrator mishandles)
- 6× `7F 10 78` (SessionControl pending)
- 5× `7F 37 78` (TransferExit pending)
- 2× `7F 36 78` (TransferData pending)
- Plus 6 (`14 11`) ClearDTC NRCs and 2 post-reset MM-only NRCs

**Method:**

Re-extracted ISO 14229 (300p), VW80124 (122p), VW80126 (84p) via pdfplumber into `/tmp/uds_mg1_extract/` (cross-ref agent's extracts had been cleaned up). Extracted §A.1 table from ISO14229.txt lines 10940-11250. Greped `mm_FULL_Flash.log` for `7E8#03 7F XX YY` patterns (511,495 lines → 67 NRC frames, 12 distinct SID/NRC pairs). Read `mdg1_flash_orchestrator.c` NRC-handling primitives line-by-line and mapped every `uds_assert_positive` call site (12 sites) vs `surface_nrc_or_continue` call sites (3 sites). Identified the 4 bare-`uds_exchange` call sites that lack pending-loop handling. Cross-referenced VW80126 §6.4.3 / §6.6.3 / §6.7.4 / §6.8.3 / §6.9.3 NRC tables against our service set.

**Reason:**

Logical next step after the cross-ref doc. The cross-ref audited PROTOCOL correctness (are we sending the right bytes?). This audit covers ERROR HANDLING correctness (do we handle the wire-observable failure modes correctly?). The two together close the protocol-conformance loop before HIL Phase 3. **The pending-loop bug is the highest-priority finding from either audit** — it's a confirmed-failing case on every real-bench flash that the shadow harness currently doesn't reproduce.

**Sensitive content discipline:** No SWCalDoc content opened. All cited specs are public (ISO 14229) or widely-circulated VW corporate (VW80126/VW80124).

[MAC]

---

## 2026-05-18 — [MAC] UDS → MG1 flow cross-reference doc

**Created:**

- `~/esp/obd/FUTV1.1/hw_reference/UDS_MG1_FLOW_CROSSREF.md` (~25 KB) — 7-section cross-reference of MM empirical wire (`~/sniffer/mm_FULL_Flash.log`) vs VW80126 / VW80124 / VW80125 / SA2-060331-V10 / ISO 14229-1 canonical specs vs FUTV1.1's `mdg1_flash_orchestrator.c`. Each step (10 02 session, 3-cycle preflight, F1 5B history read, 11 01 ECUResets, 27 11/12 SecurityAccess, 2E F1 5A fingerprint, 31 01 FF 00 erase ×5, 34 RequestDownload ×5, 36 TransferData chains, 37 TransferExit, 31 01 02 02 CheckMemory ×5, 31 01 FF 01 CheckProgDeps, 11 01 closeout, MM post-reset cleanup) shown 3 ways: spec wire-format citation, MM-observed bytes, orchestrator emission with file:line. Each step classified ✅ MATCH / ⚠️ DIFFERS / ❓ UNVERIFIED / ➕ EXTRA / ➖ MISSING.

**Key findings (chip report):**

- 12 ✅ MATCH (all critical wire-byte-exact)
- 1 ⚠️ DIFFERS — CheckMemory simplified envelope (intentional Bosch MG1 deviation from VW80126 §6.7.5; MM proves the ECU accepts the simplified form)
- 3 ❓ UNVERIFIED — dataFormatIdentifier 0x2A semantics, ALFID 0x31 nibble interpretation, DID 0x0405 probe purpose (all cosmetic / behavioral-only verified)
- 4 ➕ EXTRA — 3-cycle preflight (Bosch MG1-required, not VW80126 §5-required), hardcoded MM-style fingerprint date, SA fallback sentinel, HIL halt-before-erase gate
- 2 ➖ MISSING — VW80126 §5.1.3 pre-programming `ControlDTCSetting(off)` + `CommunicationControl(enableRxAndDisableTx)` (MM doesn't do them either pre-flash; MG1 tolerates); MM-style post-reset cleanup (all NRC'd, harmless)
- **SA2 VM impl: 5/5 PASS** on SA2-060331-V10 §2.5 reference test vectors (compiled `sa2_vm.c` standalone, ran spec example script with all 5 seed/key pairs → byte-exact key output)
- **Pre-HIL-Phase-3 blockers: NONE**

**Method:**

Extracted text from 13 OEM PDFs (VW80126 84p, VW80124 122p, VW80125 43p, SA2-V10 11p, ISO14229 300p, ISO15765-2 42p, ISO15765-3 100p, FDS_Lastenheft 16p, FlashProgrammierung 32p, BenchFlashNotice 2p, RamboPatch 1p, pcmflash_89 2p, pcmflash_92 38p) via `pdfplumber` into `/tmp/uds_mg1_extract/`. Cross-referenced grep'd MM `mm_FULL_Flash.log` (511,495 lines) against orchestrator at `firmware/src/flash/mdg1_flash_orchestrator.c` (884 lines) + `mdg1_flash_orchestrator_config.h` (437 lines) + `sa2_vm.c` (164 lines). SA2 VM verified by compiling standalone via clang and running spec test vectors. Per-step deltas surfaced with file:line citations + verbatim bytes.

**Reason:**

Hermes (Nemotron at 192.168.1.180:3000) was unreachable, so the operator approved Option B — direct UDS→MG1 cross-reference by Claude reading the PDFs locally, no Hermes round-trip. Goal: answer "are we doing the flash protocol correctly?" before the next HIL Phase 3 dispatch on the dev RS7. The doc is the protocol-audit evidence that P-08 (orchestrator + eval harness) and P-07 (real-bench Phase 2 validation) reference for "the bytes are right." `MISSING` items are non-blocking; the `DIFFERS` item is an intentional Bosch MG1 simplification.

**Sensitive content discipline:** None of the Bosch/VW confidential SWCalDoc PDFs (D-MG1-*, DMG1*, VAG_MG1CS*) were opened or transcribed. All cited specs are corporate-but-non-confidential standards (VW80124/125/126/SA2-V10 are widely circulated reverse-engineering references; ISO 14229 / ISO 15765 are public standards). MM capture is from the dev RS7 (Sean's own car).

[MAC]

---

## 2026-05-18 — [MAC] Push freeze lifted

**Modified:**

- `~/esp/obd/CLAUDE.md` — removed the PUSH FREEZE block between the title-blockquote separator and the "Active project: FUTV1.1/" section.
- `~/esp/obd/FUTV1.1/CLAUDE.md` — removed Hard Rule 7 (PUSH FREEZE). Rules 1–6 retained; the trailing `---` separator before the Repository layout section is preserved.
- `~/esp/obd/FUTV1.1/.claude/worktrees/flamboyant-turing-897f0f/CLAUDE.md` — same removal as the main tree's copy, so this worktree's branch is consistent.

**Reason:**

Owner said "remove push freeze" at end of session. Per the freeze rule's own lift instructions ("To lift this freeze, the owner will say so explicitly"), the rule blocks are removed from all three CLAUDE.md locations carrying them. The freeze was originally added 2026-05-12 around the Phase 2 AES key recovery work that needed owner audit before publication; that audit window is now closed. `git push`, `gh pr create`, and tag pushes are all allowed again.

**Not committed:** owner can commit these CLAUDE.md changes alongside (or separately from) the worktree-branch work whenever they're ready. No git operations were performed in this session.

---

## [MAC] MM-recorded ECU replay validator (2026-05-18)

### New files
- `FUTV1.1/tools/extract_mm_ecu_responses.py` — host-side extractor.
  Parses a candump (default `/Users/rabbit/sniffer/mm_FULL_Flash.log`),
  filters to 0x7E8 frames, ISO-TP reassembles (SF/FF/CF; drops FC), and
  emits an ordered JSON list of recorded ECU responses with timestamps
  and frame-type metadata. Stdlib-only; runs in <1 s.
- `FUTV1.1/firmware/test/mdg1_flash_orchestrator/fixtures/mm_ecu_responses.json`
  — generated fixture, 1091 reassembled ECU responses (positive +
  NRC + pending). Checked in as deterministic test data per prompt
  guidance.

### Modified files
- `FUTV1.1/firmware/src/config/mdg1_flash_orchestrator_config.h`
  — added `MDG1_SHADOW_REPLAY_MAX_RESPONSES = 2048` (approval-before-lock
  annotation included).
- `FUTV1.1/firmware/src/flash/mdg1_transport_shadow.h`
  — added 3 public APIs: `mdg1_transport_shadow_load_recorded_replay`,
  `mdg1_transport_shadow_replay_cursor`, `mdg1_transport_shadow_replay_total`.
- `FUTV1.1/firmware/src/flash/mdg1_transport_shadow.c`
  — added `shadow_mode_t` enum (SYNTH default, REPLAY new),
  `replay_playback_t` storage, `load_replay_fixture` JSON parser,
  `free_replay` cleanup. `shadow_recv` now branches on mode: REPLAY
  returns next recorded response from cursor; exhaustion returns
  ESP_ERR_NOT_FOUND. Cleanup tied into `mdg1_transport_shadow_close`.
- `FUTV1.1/firmware/test/mdg1_flash_orchestrator/test_orchestrator.c`
  — added `test_orchestrator_handles_mm_recorded_ecu_responses_full_flash`
  scenario. Drives the orchestrator against the MM-recorded fixture
  in REPLAY mode with HIL halt-before-erase set. Includes a DIAG block
  that dumps fixture-cursor + last-NRC details when the run does not
  reach HIL halt, so divergence points are localized in the eval log.
- `FUTV1.1/firmware/test/mdg1_flash_orchestrator/eval.sh`
  — REQUIRED_SCENARIOS list extended 17→18; header comment updated to
  match.
- `FUTV1.1/firmware/test/_shared/eval_forbidden_overrides.txt`
  — added 2026-05-18 section authorizing the new `tools/` and
  `firmware/test/mdg1_flash_orchestrator/fixtures/` paths so the
  can_capture sandbox-containment check stays clean.

### Not committed
The orchestrator host harness now reports 6 failures from the new
scenario (see `status-2026-05-18.md [MAC]` for the chip report).
Per prompt: HALT, surface the divergence, do not paper over. No
commits; no pushes.
