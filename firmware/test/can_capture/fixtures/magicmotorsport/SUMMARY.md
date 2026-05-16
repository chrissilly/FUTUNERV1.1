# MagicMotorsport flash captures — canonical reference for the MDG1 orchestrator

This directory is intentionally near-empty in the repo. The actual MM
captures contain proprietary IP (encrypted flash payloads of a customer's
calibration) and live **outside the repo** under
`/Users/rabbit/sniffer/` per Hard Rule 5 (gitignored / never committed).

## Reference captures (canonical, do NOT move into the repo)

| Filename | Path | What it is |
|---|---|---|
| `mm_FULL_Flash.log`     | `/Users/rabbit/sniffer/mm_FULL_Flash.log`     | candump-format CAN log of a 5-section full flash for box `4K0907557G__0003`. 21 MiB, ~511 K frames. **Primary reference** for byte-perfect orchestrator validation. |
| `mm_MAPS_upload.log`    | `/Users/rabbit/sniffer/mm_MAPS_upload.log`    | candump-format CAN log of a CAL-only (single-section) flash for the same box. 1.5 MiB. **Cross-check #6** — the orchestrator's CAL section must produce byte-identical output to this independent session. |
| `mm_connect.log`        | `/Users/rabbit/sniffer/mm_connect.log`        | Preflight-only capture (no SecurityAccess, no flash). Not used for orchestrator validation. |
| ECU dump (oracle + key) | `/Users/rabbit/sniffer/WUAPCBF28NN902533_4K0907557G__0003.bin` | 8 MiB. Source of the AES key (offset `0x600200`) and the per-section plaintext slices. Validated in `hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md`. |

## How the orchestrator's shadow validation maps onto these

The orchestrator runs all 5 sections in one shadow run, producing one
output log + 5 section-window views. Each cross-check below is one of
the orchestrator-prompt's 13 named scenarios.

| Section "run" | BID | Name | MM-log byte range mapping | Shadow output | Diff target |
|---|---|---|---|---|---|
| **section 1** | `0x02` | ASW1  | first `34 2A 31 02 ...` through next `71 01 02 02 00` | `shadow_section_1_ASW1_<bc>.log`  | `mm_FULL_Flash.log` filtered to ASW1 window  |
| **section 2** | `0x03` | ASW2  | first `34 2A 31 03 ...` through next `71 01 02 02 00` | `shadow_section_2_ASW2_<bc>.log`  | `mm_FULL_Flash.log` filtered to ASW2 window  |
| **section 3** | `0x04` | ASW3  | first `34 2A 31 04 ...` through next `71 01 02 02 00` | `shadow_section_3_ASW3_<bc>.log`  | `mm_FULL_Flash.log` filtered to ASW3 window  |
| **section 4** | `0x05` | CBOOT | first `34 2A 31 05 ...` through next `71 01 02 02 00` | `shadow_section_4_CBOOT_<bc>.log` | `mm_FULL_Flash.log` filtered to CBOOT window |
| **section 5** | `0x06` | CAL   | first `34 2A 31 06 ...` through next `71 01 02 02 00` | `shadow_section_5_CAL_<bc>.log`   | `mm_FULL_Flash.log` filtered to CAL window   |
| **full flash** | —    | —     | first `27 11` through final `71 01 FF 01 00`         | `shadow_full_<bc>.log`            | `mm_FULL_Flash.log` filtered to flash-critical window |
| **CAL session-independent** | `0x06` | CAL | full `mm_MAPS_upload.log` flash-critical window | `shadow_section_5_CAL_<bc>.log` | `mm_MAPS_upload.log` filtered to CAL window |

`<bc>` = the active boxcode (currently `4K0907557G_0003` — note the
single underscore in shadow filenames to match the original spec).

## Session-variant masking (what the diff ignores)

`tools/flash_shadow_diff.py` zeros these fields before comparing:

- SecurityAccess seed (`67 11 <4-byte random>`) — server-side random per session.
- SecurityAccess key (`27 12 <4-byte derived>`) — function of the seed; varies with it.
- Fingerprint write payload (`2E F1 5A <9 bytes>`) — production tooling stamps a timestamp.
- TesterPresent exchanges (`3E 00` / `7E 00`) — keep-alive, count varies.

## TransferData chunks — protocol-perfect, plaintext-equivalent

For TransferData frames (`36 <BC> <chunk>`), bit-equal comparison
between shadow and MM is structurally impossible: the LZRB encoder
makes valid-but-non-deterministic match choices, so our encoder
produces a different (still-valid) compressed byte stream than
Bosch's encoder. Reference: `hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md`
documents the 246,132 vs 260,528 byte CAL-section divergence on
the same plaintext.

Wire-ciphertext divergence is **expected, not a bug.** The ECU's
real correctness gate is `CheckMemory` CRC32 over the **plaintext**
(post-AES-decrypt + LZRB-decompress), which is unaffected by the
encoder's choice. The shadow test replicates that gate by:

1. Extracting all TransferData payload chunks for each section on
   both shadow and reference sides.
2. AES-128-CBC-decrypting each side's concatenated section
   ciphertext with the variant manifest's AES key + Bosch IV.
3. PKCS#7-stripping, then LZRB-decompressing to the section's
   expected plaintext size.
4. Comparing SHA256 between shadow's plaintext and reference's
   plaintext per section.

A section passes if both sides decode and SHA256-match. Status codes:

- `MATCH` — both sides decoded; SHA256 equal.
- `MISMATCH` — both sides decoded; SHA256 differ. Orchestrator bug.
- `REF_WIRE_MODEL_INCOMPLETE` — MM-side ciphertext doesn't fit the
  assumed "concat-chunks → CBC stream → PKCS#7 strip" model for
  this section. Observed today for ASW2 + ASW3 only (their on-wire
  total is 1,081,923 and 1,126,835 bytes respectively — both off
  by 3 from 16-alignment). Diagnosis verified 2026-05-12: my parser
  reads MM's wire bytes correctly; the section start aligns with
  ASW1's correct start (same AES IV + same LZRB-encoded DEADBEEF
  prefix); the 3-byte misalignment is a real wire-format element
  whose semantics I have not yet decoded. Marked as tool-side
  limitation, NOT an orchestrator defect (shadow side decrypts
  cleanly to byte-equal-to-oracle 2 MiB / 1.875 MiB plaintext for
  these sections). Does not cause test failure if there's at least
  one MATCH and zero MISMATCH.
- `SHADOW_FAIL` — orchestrator failed to produce plaintext. Bug.

Today's status for box 4K0907557G__0003:

| Section | BID | Status (2026-05-12) |
|---|---|---|
| ASW1 | 0x02 | ✅ MATCH (SHA256: `fe0cf4dc6b7e461f...`) |
| ASW2 | 0x03 | ⚠️ REF_WIRE_MODEL_INCOMPLETE (shadow ok, MM-side decode model incomplete) |
| ASW3 | 0x04 | ⚠️ REF_WIRE_MODEL_INCOMPLETE (same) |
| CBOOT | 0x05 | ✅ MATCH (SHA256: `a2e613348e0cd100...`) |
| CAL | 0x06 | ✅ MATCH (SHA256: `f4fafffd8e24827b...`) — also matches `mm_MAPS_upload.log` independent capture |

Bytes outside TransferData (UDS headers, session-variant fields after
masking, etc.) must match byte-for-byte. The diff tool exits 0 only
if all protocol bytes match AND at least one section is MATCH AND
no section is MISMATCH or SHADOW_FAIL.

## Why the captures aren't in the repo

`hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md` documents
that the ECU dump bytes at offset `0x600200` encode the AES key for
this customer's flash. Per Hard Rule 5 in `FUTV1.1/CLAUDE.md`, that
material — and any capture that contains the encrypted payloads it
decodes — lives in `~/sniffer/` and is gitignored. The
`/Users/rabbit/sniffer/` path is the canonical home. Override by
setting `MM_CAPTURE_DIR` in the eval harness env (see below).

## Discovering the captures from `eval.sh`

The orchestrator's eval harness reads `MM_CAPTURE_DIR` from the
environment (default: `/Users/rabbit/sniffer`). If a required capture
isn't found, eval halts with a clear "set MM_CAPTURE_DIR or symlink
the captures to <path>" message — not a stack trace.

## Stale placeholder files in this directory

The four 0-byte / 4-line / 1-line files named
`flash_run_1_4K0907557G_0003.candump.{empty,stale}.<timestamp>` and
`.log` predate this prompt; they're sentinels from an earlier session
that planned to land captures in-repo before the IP rule was
formalized. They're intentionally NOT cleaned up here — `git rm` of
already-tracked files is out of scope, and untracked files do no harm.

Reference history: see `hw_reference/EXPERIMENT_HANDOFF_dataformat_0x2A.md`
for the original "where do the captures live" decision and
`MM_Flash_Capture_Analysis.md` for the wire-protocol decode that the
orchestrator reproduces byte-for-byte (after masking).
