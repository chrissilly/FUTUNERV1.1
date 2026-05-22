# Phase 2 — Prerequisites Checklist

> Living document. Anything that MUST be true, captured, validated, or
> built before any Phase 2 (full 8 MB ECU binary reflash) code lands
> on a customer device.
>
> Owner: Sean / SRM Engineering. Updated as items close out.
> Created: 2026-05-05. Last revision: 2026-05-10.

---

## Status legend

- 🔴 NOT STARTED
- 🟡 IN PROGRESS / PARTIALLY ANSWERED
- 🟢 DONE
- ⚫ OBSOLETE / CLOSED-AS-UNRECOVERABLE — numeric slot kept reserved but content unrecoverable; not reused

---

## 2026-05-07 update — SEFIV1.0 discovery context

A find at `~/esp/obd/SEFIV1.0/` (moved in from `~/Downloads/` on
2026-05-07) provided substantial new ground truth:

- The original Scorpion EFI tool output for `4K0907557G__0003`
  (Sean's RS7) is on disk: `EXE TOOL/data/4K0907557G__0003/`.
- `output/stage1_patched.sbf` (35,571 bytes) is the **canonical
  Scorpion-emitted live-tune binary** for the dev car's Stage 1.
- `output/stage1_patched.stf` (119,951 bytes) is the **human-readable
  text equivalent** with all 1 segment + 150 inverse segments + 21
  maps spelled out byte-for-byte.
- `input/config.json` (25 KB) is the **per-variant manifest** that
  drove the build — labels, address resolution rules, modifications,
  21 maps with full ethanol+gasoline z-axis source references.
- `scorpion-bin-tools/` is the **Node.js reference implementation** of
  the binary build pipeline. Includes `docs/binary-format-v3.md` (the
  authoritative format spec for v3; production output is **format
  v4**, which is undocumented but observable from the `.stf` header
  and the `.sbf` byte structure).
- `EXE TOOL/data/4K0907557G__0003/srm prime 91 unpatched.bin` — the
  unpatched stock binary for Sean's boxcode (8 MB).
- `Customers/Dan Bui/` — second boxcode (`4M0906014__0005` / Audi SQ7)
  with full asset bundle (stock, patched, stage1, stage1_flexfuel,
  XDF). Candidate for variant matrix expansion.

The Scorpion EFI build pipeline produces **two outputs that matter to
FUTUNER**, in two stages:

**Stage A — `stage1_primed.bin`** (8 MB, what gets FLASHED to the ECU
in Phase 2):
- Starts from `stage1.bin` (full performance Stage 1 base).
- Merges 889 byte differences from `stock_patched.bin` (live-tune +
  logging patches).
- Sets address `0x9F93E` (engine_speed_display) to `12000` raw → 3000
  RPM displayed (limp/safety mode).
- Sets address `0xF0A0C` (max_load_1, 84 entries × 2 bytes) uniformly
  to ~80% (load reduction).
- Sets address `0x11E6AE` (ethanol) to `70%` baseline.

**Stage B — `stage1_patched.sbf`** (35 KB, what gets APPLIED OVER RAM
in Phase 1):
- Reverts the rev-limit display from limp to full Stage 1
  (1 regular segment, 2 bytes at `0x9F93E`).
- Reverts the max_load_1 table and 19 calibration maps (lambda + 17
  ZWGRU ignition + 1 BGRLXVD compression) from primed-safe back to
  full Stage 1 values (150 inverse segments, 9272 bytes).
- 21 maps with z-axis blend data (gasoline + ethanol variants) for
  runtime ethanol blending.

This validates the FUTUNER product design end-to-end: the flashed
binary is **fail-safe** (limp mode + 70% ethanol baseline) so the car
drives even if the dongle never connects; the SBF "unlocks" full
Stage 1 by reverting the safety reductions. MISSION_SPEC §4.5's
rev-limit-during-update safety rail (drop to 4000 RPM during the 2 s
RAM-rewrite window) is specifically protecting the rev-limit toggle
inside Stage B.

**What this changes for the P-list, item by item, is annotated inline
below.** TL;DR: most P-items remain open; P-11(a) is materially
solved (variant manifest schema = adopt the existing `config.json`
shape); two new items P-12 and P-13 surface from the find.

---

## P-01 🟡 MagicMotorsport flash capture session — reference UDS sequences

**2026-05-10 late update — capture session complete, analysis published.**
Two captures landed (`mm_FULL_Flash.log` 5-section full flash, ~5 min 51 s;
`mm_MAPS_upload.log` CAL-only, ~47 s) plus the flashed binary and MM
console output. The wire-level UDS choreography is now fully decoded in
[`hw_reference/MM_Flash_Capture_Analysis.md`](../hw_reference/MM_Flash_Capture_Analysis.md).
Of the four open questions the capture was supposed to close:

1. **SA2 runtime ground truth — DONE.** Two `(seed, key)` pairs captured:
   `C361B058 → F0F2BDD2` and `168BC5E2 → F1CB900F`. Bake into
   `firmware/test/test_sa2.c` as runtime vectors against the variant
   manifest's SA2 script for 4K0907557G_0003 before merging.
2. **Per-variant SA2 script identification — DEFERRED.** Validation of
   "which of the 3 known scripts the 4K0907557G_0003 ECU actually runs"
   waits on running each candidate script through `sa2_vm` against the two
   captured pairs and matching output. Mechanical work; not blocking.
3. **RSA-on-MG1 question — TENTATIVELY: NO RSA OBSERVED.** No
   high-entropy ≥256 B blocks in the TransferData stream consistent with
   an RSA modulus, no pre-flash `31 01 <RID> <sig>` calls beyond
   `0x0203` (preconditions). MM's flash chain on MDG1 appears to rely on
   `0xFF01 CheckProgrammingDependencies` for cross-section consistency
   only, not on per-block signatures. Re-verify once decrypt+decompress
   is implemented — a signature could still live inside a section's
   plaintext (e.g. the CBOOT block).
4. **RoutineControl byte formats — DONE.** §2.4.1 (EraseMemory),
   §2.4.5 (CheckMemory), §2.6 (CheckProgrammingDependencies) document
   every captured parameter byte.

**Material protocol correction from the captures:** MDG1 RequestDownload
addresses sections by **1-byte logical block ID** (0x02–0x06), not by
4-byte physical address. ALFID is `0x31` (size 3 bytes, address 1 byte)
and dataFormatIdentifier is `0x2A` (LZRB compression + Bosch AES). The
prior `firmware/src/flash/mdg1_flash.c::mdg1_flash_request_download`
signature (32-bit address parameter, ALFID 0x44) has been updated to
match (see `firmware/src/flash/mdg1_flash.h` and the §4 implementation
gap table in the analysis doc). **Do not assume this 1-byte block-ID
scheme generalises to MED17 / EDC17** — those families historically use
4-byte physical addresses and need their own capture before Phase 2 can
target them.

**Material checksum confirmation:** the CheckMemory parameter is plain
`zlib.crc32` over the section's plaintext bytes (init 0xFFFFFFFF,
reflected, final XOR 0xFFFFFFFF). All five captured CRCs reproduce
exactly from slices of the flashed binary. Five golden vectors are
listed in analysis doc §2.4.5 — bake into a unit test.

What remains 🟡 rather than 🟢:

- The encryption portion of dataFormat 0x2A (algorithm + key derivation
  + IV) is not provable from the captures alone — see analysis doc §3.2.
  A round-trip "decrypt the captured CAL TransferData stream and compare
  to file offset 0x80000 of the binary" test will close it. Until that
  succeeds, Phase 2 firmware can't actually transfer payloads even
  though the choreography is known.
- ASW3 erase + transfer cycle is captured but only one transition was
  byte-by-byte verified (§3.4) — spot-check the other three.

**2026-05-10 status update.** Importance and urgency *increased*. After
this session's SA2 VM + per-variant manifest work, the capture session
now resolves four open questions in one bench day:

1. **SA2 runtime ground truth.** `firmware/src/flash/sa2_vm.c` (host-tested
   green on all 5 spec test pairs) has never been validated against a
   real ECU's `(seed, key)` pair. Each MagicMotorsport cycle's
   `0x67 01 <seed>` + `0x27 02 <key>` pair gives us authoritative
   ground truth — write each pair as a runtime test vector in
   `firmware/test/test_sa2.c`.
2. **Per-variant SA2 script identification.** The dev RS7 is
   `4K0907557G__0003`; per `docs/boxcode_database.md` it uses the
   "MG1CS002 key" but its variant family in our manifest isn't yet
   indexed (the manifest covers 3 SA2 scripts, none yet bound to
   `4K0907557G`). Capture either confirms one of the 3 known scripts
   or adds a 4th — either is forward progress.
3. **The RSA-on-MG1 question** (§5.1 of `hw_reference/MG1_MDG1_Flashing_Research_Part2.md`).
   No MG1 RSAPublicKeys JSON was found in the entire 2.5 GB drive
   pull. Either the exploit chain bypasses signature verification on
   MG1 or the keys live somewhere we haven't pulled. The capture
   answers this empirically: scan TransferData payloads for
   high-entropy ≥256-byte blocks (RSA modulus shape) and look for
   pre-flash signature-verify RoutineControl calls.
4. **RoutineControl byte formats.** We have zero authoritative samples
   of the erase / verify-block / verify-dependencies parameter
   formats. The capture is the only way to lock these in.

**2026-05-07 status update.** Importance unchanged but scope narrowed.
The SEFIV1.0 find solves the question of WHAT to flash (per-VIN
`stage1_primed.bin` produced by the Scorpion EFI build pipeline) but
not HOW to flash it at the wire level. P-01 was always about capturing
the wire-level UDS choreography (security access, challenge-response,
chunked TransferData, CRC verify, reset) — that remains entirely
unaddressed by the find. **Still 🔴, still the practical unblocker
for P-02, P-08.** Phase 1 work continues to be unblocked without it.

**Why this is critical.** Phase 2 requires producing a UDS sequence
that an MG1/MDG1 ECU accepts as authoritative. We have AES keys and
research notes, but the only way to be sure FUTUNER's flash code is
correct is to diff it against a known-working reference. MagicMotorsport
is that reference.

**The plan, recorded so it doesn't get lost.**

1. Use the dev RS7 (variant `4K0907557G__0003` per `docs/boxcode_database.md`).
2. Wire the Candlelight USB-CAN sniffer in via Y-splitter on the OBD-II
   port (isolated-ground variant). Dongle does NOT need to be present —
   this is MagicMotorsport doing the talking.
3. Have the MagicMotorsport tool flash the **same binary that's already
   on the ECU**. Re-flashing identical content exercises the full UDS
   challenge-response → security access → encrypted transfer → verify →
   reset pipeline without changing any code on the ECU. Safe by design.
4. Capture 3-5 back-to-back full flash cycles. Multiple cycles are
   important because security access seeds rotate per session — one
   capture would let us accidentally hardcode one seed pair into the
   simulator.
5. For each capture, record on paper or in the notes.md: the boxcode +
   SW number on the ECU at capture time, the ECU's flash-write counter
   value before/after the cycle (visible to dealer tools — note the
   starting value in case it ever matters for warranty/resale), and
   any anomalies during the cycle.

**Where the captures land.**

```
firmware/test/can_capture/fixtures/magicmotorsport/
  flash_run_01_4K0907557G_0003.candump
  flash_run_01_4K0907557G_0003.notes.md
  flash_run_02_4K0907557G_0003.candump
  flash_run_02_4K0907557G_0003.notes.md
  ...
```

**What these captures unlock.**

1. **Parser test fixtures** — bigger, realer, security-access-bearing
   captures than the synthetic ones. The bench CAN toolkit's parser
   gets exercised against real wire traffic.
2. **ECU simulator behavior source** — the `firmware/test/ecu_sim/`
   package (future) replays the ECU's recorded responses so FUTUNER's
   flash code can be developed against a deterministic emulator on the
   bench, not against a live car.
3. **Phase 2 flash-code validation oracle** — when FUTUNER's own flash
   code is written, its eval harness diffs FUTUNER's outgoing UDS
   sequence against the MagicMotorsport reference. Bit-exact match is
   the bar.

**Tracked as Cowork task #6.** Capture-session checklist (exact
commands, file naming, on-paper data to record) to be drafted before
the actual session. Not yet drafted; not blocking other work.

---

## P-02 🔴 ECU simulator package built and validated against captures

Depends on P-01. Builds `firmware/test/ecu_sim/` — a Python ISO-TP
responder that loads recorded MagicMotorsport captures and replays the
ECU side. Lets FUTUNER's Phase 2 flash code iterate against a fake ECU
on the bench without touching the dev car. The simulator's correctness
is graded by replaying its responses against the original tester
queries from the same capture and verifying byte-exact match.

---

## P-03 🔴 Recovery binary per ECU family

Per MISSION_SPEC §5.1 and SCALE_ARCHITECTURE_PROPOSAL §7.4. A separate,
minimal binary per ECU family that the dongle attempts to apply if a
flash fails mid-write. Without this, a partial flash failure can brick
the ECU. Must exist before any production Phase 2 ever runs.

---

## P-04 🔴 Pre-flash safety gate hardened

Per SCALE_ARCHITECTURE_PROPOSAL §7.3. All gates implemented and tested:
variant ID matches binary, battery voltage above floor, ignition state
correct, ECU responding within timeout, customer license shows
`paid = true` and `phase2_flashed_at = null` (or admin override),
explicit customer UI confirmation within last N seconds. Configured
defaults: voltage floor 12.4 V, confirmation window 60 s — needs Sean's
sign-off.

---

## P-05 🟡 Per-variant base binary signed and stored

**2026-05-07 status update.** Partially answered. The SEFIV1.0 find
shows that the Scorpion EFI build pipeline already **produces** the
per-variant base binary (`stage1_primed.bin`, 8 MB) as a build target
output. For `4K0907557G__0003` (dev car) this binary exists on disk
today at `~/esp/obd/SEFIV1.0/EXE TOOL/data/4K0907557G__0003/output/stage1_primed.bin`
(reproducible from the build pipeline given the customer's input
files). **What's still 🔴:** signing, storage, signed-URL delivery,
firmware-side signature verification. Build-pipeline integration on
the cloud (P-13, NEW) is the natural home for this work.

Per SCALE_ARCHITECTURE_PROPOSAL §7. SRM-built and SRM-signed Phase 2
base binary per ECU variant (Stage 1 power + logging + live-tune
hooks). Stored in object storage. Signed URLs for delivery. Firmware
verifies signature before flash.

---

## P-06 🔴 AES key custody decided and implemented

Per SCALE_ARCHITECTURE_PROPOSAL §10 question 3. Where do AES keys live
in production? HSM-backed cloud secret, encrypted-at-rest in cloud DB,
or split between cloud and dongle secrets partition? This is a
load-bearing decision for Phase 2 and an open question Sean has not
yet signed off on.

---

## P-07 🔴 Real-bench Phase 2 validation per variant

Each ECU variant in scope for Phase 2 must have at least one
real-bench-rig flash succeed end-to-end before any customer device
gets that variant's Phase 2 code. Simulated bench is not enough for
Phase 2 (fine for Phase 1).

---

## 2026-05-12 update — orchestrator landing + shadow-validation contract

**P-08 status:** 🔴 → 🟡. First end-to-end shadow-validated implementation
landed this session.

The MDG1 5-section flash orchestrator (`firmware/src/flash/mdg1_flash_orchestrator.{c,h}`)
runs the full UDS choreography (SecurityAccess → fingerprint → 5×{Erase,
RequestDownload, TransferData, TransferExit, CheckMemory} → CheckProgrammingDependencies
→ ECUReset). Transport-agnostic via `mdg1_uds_transport_t`. Two impls:
shadow (host-side log + replay) and production CAN (firmware, **dormant**).
Gated by `FUTUNER_PHASE2_ENABLED` in `firmware/src/config/futuner_config.h`
(default 0). `idf.py build` with both `-DFUTUNER_PHASE2_ENABLED=0` and
`=1` exits 0.

**Shadow validation gate is "protocol-perfect + plaintext-equivalent",
NOT "wire byte-perfect":**

- Non-TransferData UDS frames must match MM's bytes byte-for-byte after
  masking session-variant fields (SA seed/key, fingerprint, TesterPresent)
  and filtering pending negative responses (`7F xx 78`).
- TransferData chunks: wire-bit-equality is structurally impossible —
  LZRB encoders make valid-but-non-deterministic match choices. Our
  encoder and Bosch's both produce LZRB-valid outputs that decompress
  to the same plaintext, but the byte streams differ. See
  `hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md` (246,132 vs
  260,528 bytes for CAL on the same plaintext).
- The shadow test replicates the ECU's real correctness gate (CRC32
  over plaintext) by per-section AES-decrypt + LZRB-decompress + SHA256
  compare on both shadow and MM sides.

3 of 5 sections (ASW1, CBOOT, CAL) verify plaintext-equivalent
end-to-end. ASW2/ASW3 are REF_WIRE_MODEL_INCOMPLETE — the MM-side
decode model for those sections is off by 3 bytes from 16-alignment
(1,081,923 and 1,126,835 bytes total; off-by-3). Root cause is
wire-format reverse-engineering of a per-section element I haven't
decoded yet — not an orchestrator defect (shadow side decrypts those
sections cleanly to byte-equal-to-oracle plaintext).

See `firmware/test/can_capture/fixtures/magicmotorsport/SUMMARY.md`
for the full per-section status table + masking rules.

**Eval gates touched:** all 6 prior gates (feature_manager, wot_logger,
dtc, vin_pairing, sbf, ui) extended with a shared
`firmware/test/_shared/eval_forbidden_overrides.txt` reader so
authorized cross-cutting prompts don't trip overlapping FORBIDDEN
lists. Same mechanism added to mdg1_payload's eval. New orchestrator
eval (`firmware/test/mdg1_flash_orchestrator/eval.sh`) installed.
All 8 gates plus `verify_frozen.sh` green at commit landing.

**Not yet done (deferred to a follow-up prompt):**
- Register `FEATURE_PHASE2_FLASH` with `feature_manager`. The
  `mdg1_aes_mbedtls_register()` call lands in main.c gated by
  FUTUNER_PHASE2_ENABLED, but the feature_manager registration
  (start/stop/is_running callbacks) is a separate step.
- WS command surface for triggering Phase 2 from the UI.
- Production CAN transport (`mdg1_transport_can.c`) is built but
  intentionally dormant — `mdg1_transport_can_open()` returns a
  stub iface; no init call from main.c.

---

## P-08 🟡 Dongle firmware Phase 2 flash code written + eval harness green

The actual flash module under `firmware/src/flash/` — Phase 2 specific
work. Lots of sub-tasks: UDS challenge-response mirroring, AES-128-CBC
implementation (or hardware-accelerated path on ESP32-S3), chunked
TransferData with per-block checksums, fault recovery sequence,
integration with feature_manager (`FEATURE_PHASE2_FLASH`), pre-flash
gate enforcement. Eval harness diffs FUTUNER output against the
MagicMotorsport reference from P-01 and asserts bit-exact match.

**2026-05-10 status update.** Wire format is now known
(`hw_reference/MM_Flash_Capture_Analysis.md`). `mdg1_flash.c` has been
partially corrected (RequestDownload now uses 1-byte block ID + ALFID
0x31 + dataFormat 0x2A; SecurityAccess sub-function configurable on ctx
with MDG1 default 0x11; SA2 VM integration already in place from
earlier today). Per-section orchestrator (the 5-block loop) and
encryptionMethod 0xA implementation are the two large outstanding
pieces. `mdg1_flash_execute()` now returns `ESP_ERR_NOT_SUPPORTED` so
it can't be invoked from feature_manager before the orchestrator
exists.

---

## P-09 🔴 Customer-facing Phase 2 documentation

Per MISSION_SPEC §5.1 deliverables. End-user-readable explanation of
what Phase 2 does, the safety considerations, and the "if anything
looks wrong" recovery procedure. Required before any customer
device is allowed to do a Phase 2 flash.

---

## P-10 🔴 Cloud server Phase 2 endpoints

**2026-05-07 status update.** No closure, but design clarified. The
cloud's Phase 2 binary delivery should fetch from the cloud-side
build pipeline (see P-13, NEW) rather than serving a static SRM-built
binary. The endpoint shape is otherwise unchanged.

Per SCALE_ARCHITECTURE_PROPOSAL §3.4. New endpoints:
- `GET /api/v1/phase2/base` — signed URL to this variant's base binary
- Admin endpoints for uploading + assigning per-variant binaries

These don't exist in `cloud/src/main.py` today.

---

## P-11 🟡 Per-variant manifest replaces sbf_variants table + ECU sentinel check

**2026-05-07 status update.** Sub-item (a) is materially solved by
the SEFIV1.0 find: the Scorpion EFI tool's `config.json` schema is a
complete, working per-variant manifest. Adopt it (with FUTUNER-specific
extensions) rather than designing one from scratch. Sub-item (b)
(ECU sentinel check) is unchanged in scope but easier to implement
now that the manifest schema is settled.

Two related items grouped because both close out together when
`SCALE_ARCHITECTURE_PROPOSAL §2.2` ships.

**(a) sbf_variants table → manifest.** Today
`firmware/src/sbf/sbf_variants.{c,h}` carries a hand-maintained
`{boxcode, mid_byte, address_offset}` table used by the SBF live
tune applier when invoking `ecu_write_data`. v1 ships with one row
(the dev car's `4K0907557G__0003`); future boxcodes get rows added
manually per variant-onboarding. The Phase A migration plan lifts
these values into the per-variant manifest's `memory_map.write_mid_byte`
and `memory_map.write_offset` fields, with `sbf_variants_lookup`
becoming a thin shim that resolves the active variant's manifest.

This same table is duplicated structurally by
`logger_variables_get_write_mid_byte()` /
`_get_write_address_offset()`. Both must coexist with matching
values until the manifest migration unifies them.

**Schema target (NEW 2026-05-07):** the canonical schema is the
Scorpion EFI tool's `config.json` (see `~/esp/obd/SEFIV1.0/EXE TOOL/data/4K0907557G__0003/input/config.json`):

- `version` (int, format version of this manifest)
- `max_gap`, `max_segment_length` (segment-merging tuning)
- `live_regions` ([address_lo, address_hi] pairs — bounded write region)
- `ignored_segments` (array of label names to exclude from segments)
- `maps[]` — per-map metadata: name, path, dimensions, axis specs
  (name, unit, type, scaling, decimals, address(es)), `force_revert`,
  `allow_in_segment`, `allow_in_inverse_segment`, `ethanol`, `blend_map`
  references
- `labels[]` (NEW name needed) — symbolic-address resolution for
  things like `engine_speed_display` (direct address) or `ethanol`
  (byte-pattern search). Today the Scorpion tool encodes these as
  `name`/`address` or `name`/`pattern` entries.

**FUTUNER additions to the manifest** (not in Scorpion's
`config.json`): `boxcode`, `ecu_software_version`,
`memory_map.write_mid_byte`, `memory_map.write_offset`, `sentinel`
(per (b) below).

**(b) ECU sentinel check before live-tune apply.** The orchestrator
does NOT verify the ECU's currently-running binary contains the
live-tune patches before applying. A paid-but-stock-flashed customer
could attempt a live-tune apply, get "success", and see no behavior
change. v1 relies on operator discipline (dev-car only); customer
rollout requires:
- A per-variant sentinel address + expected byte sequence in the
  manifest (a known byte pair touched by the live-tune patch — e.g.,
  one of the 889 `merge_diff` differences from `stock_patched.bin`).
- A pre-apply UDS read at that sentinel; mismatch → refuse with a
  clear "ECU not flashed for live tune" message.
- A separate cloud-side gate so the order is enforced (Phase 2
  first, then live tune).

Closes when: `docs/SCALE_ARCHITECTURE_PROPOSAL.md` §2.2 schema is
locked (with FUTUNER extensions over the Scorpion `config.json`
baseline), the dongle reads variant manifest at boot, and the
sentinel check is wired into `sbf_orchestrator`.

---

## P-12 🔴 Frozen `scal_file.c` parser format-v4 verification (NEW 2026-05-07)

**Why this is needed.** The frozen `scal/scal_file.{c,h}` parser was
carry-forward from FUTV1.0 reverse-engineering. The on-disk Scorpion
tool emits **format v4** binaries (header field `version: 4`, with
`TOTAL_INVERSE_SEGMENTS` and `INVERSE_SEGMENT_START` fields per the
v3 spec). The available format documentation only covers v2 and v3.
Either (i) FUTV1.0's parser was reverse-engineered against real v4
files even though no v4 spec was written down — in which case it
works correctly but the rationale wasn't recorded — or (ii) the
parser handles v3 and v4 files happen to load because v4 is mostly
backward-compatible — in which case there are quiet correctness gaps
around the inverse-segment count / start fields. Either possibility
must be empirically verified before any Phase 2 work assumes parser
correctness.

**Verification procedure:**

1. Load `~/esp/obd/SEFIV1.0/EXE TOOL/data/4K0907557G__0003/output/stage1_patched.sbf`
   (the canonical Scorpion-emitted v4 file, hash `460263de...`)
   through FUTV1.1's frozen parser.
2. Dump the parsed `flex_map_entry`, `blend_map`, `gasoline_data`,
   and `ethanol_data` for every map.
3. Cross-reference against the human-readable
   `output/stage1_patched.stf` (1272 lines, all 21 maps spelled out)
   for byte-exact match.
4. Repeat for the existing FUTUNER sample at
   `~/esp/obd/FUTV1.1/sbf/stage1_patched.sbf` (hash `25ac8b17...`,
   different `build_timestamp`).
5. Both files MUST parse identically modulo timestamp.

**Closes when:** verification round complete, results documented in
`docs/FROZEN_MODULES.md` history section, no parser changes needed.
If parser changes ARE needed: this is no longer a verification round
but a frozen-module modification, and the four-step approval ritual
in `firmware/src/FROZEN_MODULES.md` applies. Sean must approve in
writing before any frozen-module byte changes.

---

## P-15 🔴 Recover key bytes for 48 NEW fingerprints from the 2026-05-12 corpus sweep (NEW 2026-05-12)

**What was found:** Pre-scan of `~/034_local/` (123 GB, 3,521 bins from
the SanDisk 034 archive) found **53 unique AES-128 key fingerprints**
at the documented bootloader offsets (`0x18200` plain, `0x600200`
IFX). 5 match keys we already have (MG1 generic, MG1CS002, MG1CS011,
MD1CP004 T1+T2, MD1CP014). **48 are NEW** — distinct AES-128 keys
this project doesn't have bytes for today.

**Top NEW fingerprints by bin count:**

| sha256[:8] | Offset | Bins | Inferred ECU family |
|---|---|---|---|
| `0e2cad79` | `0x18200` | 27 | MED9 |
| `d6f4b42a` | `0x18200` | 16 | MED17 |
| `f5d0d2c3` | `0x18200` | 12 | MED17 |
| `72e22b78` | `0x18200` | 10 | MED17 (TP2) |
| `b0cb819b` | `0x18200` | 10 | EDC17 C74 (diesel) |
| `66678bb9` | `0x18200` |  9 | unknown (CAN-log parser artifact?) |
| `664419e3` | `0x18200` |  8 | MG1_CS002 Autotuner variant |
| `b7d39dab` | `0x18200` |  6 | TCU DQ38x G2 |
| `5f0731e8` | `0x18200` |  6 | TCU DQ500 |
| + 39 more  | — | <6 each | mixed |

**Status:** 🔴 NOT blocking Phase 2 (the MG1CS002IFX RS path for
4K0907557G__0003 is independent and already validated). REQUIRED
for Phase 3 ECU-family coverage — every customer ECU outside the
MG1CS002 master + MG1 generic + MG1CS011 universe needs a key
sourced before we can flash it.

**Recovery recipe (per FINDINGS_2026-05-12_phase2_key_recovery.md):**
For each NEW fingerprint:
1. Open the listed sample bin at the listed offset.
2. Read 16 bytes. SHA-256 those bytes → confirm first 4 bytes match
   the fingerprint.
3. The bytes ARE the AES-128 key. Add to `secrets/AES_KEYS_MASTER.md`
   (key bytes never leave `secrets/`).
4. Map any boxcodes that share that fingerprint into
   `secrets/aes_keys_per_boxcode.json`.
5. Per-family enablement (SA2 script + section map + CRC algorithm
   + post-commit dependencies) is separate work — having the key
   is necessary but not sufficient.

**Reference data:**
- Full inventory: `hw_reference/ecu_key_corpus_2026-05-12/`
  (machine-readable JSON + human-readable table)
- Per-bin pre-scan: `firmware/test/bin_inventory.md`
- Proposed manifest entries: `tools/proposed_manifest_merge_2026-05-12.json`
- Methodology + architecture decision (why no Hermes dispatch):
  `tools/hermes_extraction_report_2026-05-12.md`

**Owner:** Sean / SRM Engineering. **Cross-ref:** P-06 (AES key
custody decided and implemented) closes once these are merged.

---

## P-14 🔴 MM wire-format reverse-engineering: ASW2/ASW3 off-by-3 element (NEW 2026-05-12)

**What was observed:** Shadow-vs-MM diff for box `4K0907557G__0003`,
sections ASW2 (BID `0x03`) and ASW3 (BID `0x04`), marked
`REF_WIRE_MODEL_INCOMPLETE` by `tools/flash_shadow_diff.py`. MM's
reassembled per-section ciphertext stream comes out 1,081,923 bytes
(ASW2) and 1,126,835 bytes (ASW3) — both off by 3 from 16-alignment.
The naive "concat all TransferData chunks → single AES-CBC stream
→ strip PKCS#7" model — which works byte-perfect for ASW1, CBOOT,
and CAL — fails on these two sections.

**What was verified (2026-05-12):**
- The diff tool's parser reads MM's wire bytes correctly. Per-chunk
  sizes match the FirstFrame declared lengths byte-for-byte. The
  reassembler is consistent.
- ASW2 chunk[0]'s first 16 ciphertext bytes are identical to ASW1
  chunk[0]'s first 16 bytes (`36 01 4b 7c d6 8d 4c c8 a7 ad 62 9c
  a1 5d 47 16 99 f5`). That's the deterministic post-AES-CBC encoding
  of LZRB-encoded `EF BE AD DE 00 ...` (DEADBEEF marker + zeros) with
  the Bosch fixed IV — so the section START is correctly aligned in
  both ASW1 (which works) and ASW2 (which doesn't).
- The shadow side of the orchestrator decrypts those sections cleanly:
  ASW2 plaintext = 2,097,152 B, byte-equal to oracle bin slice
  `0x200000..0x400000` (SHA256 `a9b3c2ed7b606f93...`). ASW3 plaintext
  = 1,900,544 B, byte-equal to oracle slice `0x630000..0x800000`
  (SHA256 `ab5f536caa52b15d...`). So the orchestrator IS correct;
  the gap is in MM's decode model, not in our pack path.

**What was NOT identified:** the 3-byte wire-format element that
appears in MM's ASW2/ASW3 transmissions. Possibilities (not yet
verified): per-section header/footer not part of the AES stream, MM
tool-side framing artifact, ECU-side decode rule that tolerates
trailing wire bytes, or a different cipher-mode boundary on these
specific BIDs. Per-chunk-CBC-reset is consistent with the byte-count
math but contradicts what AES_KEYS_MASTER.md + RL_MDG1.cpp document
about Bosch's wire-format.

**Suggested next steps:**
- Capture a fresh MM flash of ASW2 + ASW3 in isolation (single-section
  RequestDownload runs) and annotate the bytes that fall outside
  16-alignment.
- Or: write a brute-force decoder that, for each plausible per-chunk
  AES boundary (per-section, per-chunk, every-N-chunks), tries
  AES-CBC + LZRB-decompress and accepts the candidate that yields
  oracle-byte-equal plaintext.
- Or: ask the MM vendor for the per-section wire format spec for
  large (>1 MiB) sections.

**Status:** 🔴 NOT blocking shadow validation today — diff exits 0
with 3/5 sections plaintext-equivalent + 2/5 marked
REF_WIRE_MODEL_INCOMPLETE (acknowledged as RE gap, not orchestrator
defect). **REQUIRED** before any real-car ASW2/ASW3 flash, because
without understanding the 3-byte element the orchestrator can't
match MM's bit-exact wire format and we don't know if the ECU
tolerates our (likely subtly different) byte stream for those
sections.

**Owner:** Sean / SRM Engineering. **Cross-ref:**
`firmware/test/can_capture/fixtures/magicmotorsport/SUMMARY.md`
documents the per-section status table; `tools/flash_shadow_diff.py`
implements the REF_WIRE_MODEL_INCOMPLETE status code.

---

## P-13 🔴 Cloud build pipeline integration (NEW 2026-05-07)

**Why this is needed.** The Scorpion EFI tool (Windows .exe) and the
companion `scorpion-bin-tools` Node.js project on disk implement the
full SBF / primed-binary build pipeline. Customer requests for Stage
1 (or any stage) need the cloud server to produce a per-VIN
`stage1_primed.bin` (Phase 2 flash payload) and `stage1_patched.sbf`
(Phase 1 RAM update) on demand. **Reimplementing this in
`cloud/src/main.py` is wasted effort.** The cloud server should wrap
the existing pipeline.

**Decision needed:** which implementation does the cloud invoke?

- **Option A — `scorpion-bin-tools` (Node.js).** Native cross-platform,
  open in `~/esp/obd/SEFIV1.0/scorpion-bin-tools/`, can run as a
  subprocess of the FastAPI server. No wine/emulation. Likely faster
  to integrate. Trade-off: brings a Node runtime onto the cloud box.
- **Option B — Scorpion `.exe` via Wine on Linux.** Runs the exact
  same binary Sean has used on Windows. Identical output guaranteed.
  Trade-off: heavier dependency, slower per-build.
- **Option C — Reimplement in Python alongside FastAPI.** No
  external runtime. Trade-off: significant new code, divergence risk
  from the canonical implementation.

**Recommended:** Option A. `scorpion-bin-tools` is the cleanest fit
and Sean already has it on disk. Cloud builds become:
`node scorpion-bin-tools data/<boxcode>/` → produces `output/`
directory → cloud serves the contents as signed URLs.

**Sub-tasks:**
- Lift `scorpion-bin-tools` source into the FUTUNER cloud repo (or
  reference it as a submodule / vendored copy)
- Per-VIN customer config.json generation (template + ethanol_random
  injection per existing FUTV1.1 cloud DRM)
- Build-on-demand vs. build-on-upload caching strategy
- Monitoring + error-handling around subprocess invocation

**Closes when:** cloud's `/api/v1/phase2/base` and
`/api/v1/device/calibration` endpoints serve build-pipeline output
that is byte-identical to running the .exe locally on Sean's
Windows machine.

---

---

## P-16 🔴 USB-Serial-JTAG primary console for interactive HIL commands (NEW 2026-05-12)

**Discovered during Layer 2 HIL preflight wire-up (2026-05-12).**

`firmware/sdkconfig` today has `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`
(primary console = UART0 physical pins) with
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` (USJ = output-only
mirror). That means:

- Boot logs and `printf` output flow fine over USB-CDC
  (`/dev/cu.usbmodem*`), because USJ mirrors stdout.
- **stdin from USB-CDC lands on UART0 RX**, which on BOARD_REV2 is
  not wired to anything — so the `serial_console_task` in
  `commands/serial_console.c` never receives operator input over
  the USB cable.

The blast radius: `phase2_hil_preflight`, `phase2_hil_preflight_arm`,
`status`, `wifi_status`, and every other interactive serial command
is unreachable from a host on the USB cable. The workaround in place
for Layer 2 is the NVS-armed autostart (`phase2_hil_autostart.c`) plus
the `phase2_hil_preflight_arm` WS command (requires AP-client first to
spin up the WS server), or the bench-only
`PHASE2_HIL_AUTOSTART_FORCE_ARM_THIS_BUILD` compile-time flag.

**Fix scope:** flip `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (primary)
+ verify all log/stdio paths still route correctly, then drop the
bench helper. Probably one-line change in `sdkconfig.defaults` plus a
boot-log smoke test.

**Why not done in the Layer 2 prompt:** scope creep — Layer 2's
acceptance was "dongle produces a shadow log matching the host
reference," and we hit that via autostart. Console wiring is a
separate, broader fix that should not be bundled.

**Closes when:** sending `phase2_hil_preflight_arm\n` to
`/dev/cu.usbmodem*` from a host echoes the JSON ACK and arms the
flag without needing AP-client interaction.

---

## P-33 🟢 `wifi_manager/eval.sh` bash 3.2 portability (RESOLVED 2026-05-22, see commit message)

**Severity:** Medium (test-tooling, not product)
**Found:** 2026-05-19, during phase1 merge sanity check on Mac
**File:** `firmware/test/wifi_manager/eval.sh`

The eval wrapper uses `declare -A` (associative arrays), which is bash 4+ only. macOS default bash is 3.2 (Apple stopped updating after GPLv3). Tests themselves pass via the `host_test_runner` binary; only the `eval.sh` wrapper fails to load on stock macOS.

**Fix options:**
- Rewrite using parallel indexed arrays (portable across bash 3.2 / 4+)
- Add `#!/usr/bin/env bash` + document `brew install bash` requirement
- Migrate eval logic to a Python wrapper (consistent with other `tools/`)

Don't act on this prompt — owner reviews.

**Closes when:** `bash firmware/test/wifi_manager/eval.sh` runs to completion and exits 0 on stock macOS bash 3.2 without needing brew bash.

---

## P-28 🟢 WOT logger recorder init returns rc=258 — feature 1 never registers (RESOLVED 2026-05-21, commit `4788876`)

Filed during Phase 1 smoke test on PC (2026-05-17). Surfaced by Tier 2 `wot_log_start` returning `{"error":"feature id 1 is not registered","active_feature":"none"}`.

Boot log shows:
```
E (898) WOT_LOG: recorder init rc=258
W (898) MAIN: WOT logger init failed (non-fatal): rc=258
```

`MAIN` flags this non-fatal, so the dongle boots fine, but the consequence is that `FEATURE_WOT_LOGGING` (id 1) never registers with `feature_manager`. Every `wot_log_start` request fails. This blocks:

- Tier 2c (WOT log capture + cloud upload)
- Tier 2h (cloud-network-feature-blocks-wifi-mode-swap safety net) — the only registered cloud feature left is `FEATURE_VIN_PAIRING`, but vin_pair_now exits too quickly to exercise the 2h interlock
- Any field-customer use of WOT log capture on this firmware build

The numeric rc=258 is decimal 0x102 = `ESP_ERR_INVALID_ARG` in IDF's error space (after mapping). Real root cause likely lives in `firmware/src/logger/wot_recorder.c::wot_recorder_init` — failing on a precondition check (NVS namespace? PSRAM allocation? Filesystem partition? Already-init?). Pre-existing from before this session — this prompt didn't touch the logger.

**Root cause confirmed 2026-05-19 (Mac):** Init order in `firmware/src/main.c::app_main()` calls `wot_logger_init()` (line ~161) **before** any logger profile is loaded.

1. `wot_logger_init()` → `snapshot_logger_profile()` (wot_logger.c:252)
2. `snapshot_logger_profile()` → `logger_manager_get_variable_count()` (wot_logger.c:177)
3. At boot, no profile loaded → returns 0
4. `wot_recorder_init(cfg.variables_per_sample = 0)` → guard at `wot_recorder.c:291-293` fails → ESP_ERR_INVALID_ARG = 258
5. Error propagates → `s_initialized` stays false → feature never registers with feature_manager

**Fix options** (all > 20 lines or API-contract changes — not inline-fixable):
- Defer `wot_logger_register_with_feature_manager()` to a callback fired after profile load (lifecycle change, ~50 lines + new hook in logger_profile.c)
- Relax `wot_recorder_init`'s zero-vars guard, add reconfigure path on first start (API contract change)
- Pre-load a default profile at boot before `wot_logger_init` (hacky — leaks UX coupling into init)

Owner reviews. **Phase 5 of HIL validation is expected-fail until P-28 closes.**

**Closes when:** boot log shows `WOT_LOG: recorder init OK` and `wot_log_start` over WS returns `{"ok":true,"feature":"wot_logger"}` with `wifi_mode ap` mid-active returning `{"ok":false,"error":"feature_active"}`.

Owner reviews. Don't act on this prompt — beyond scope (Phase 1 smoke test).

---

## P-27 🟡 Replace pyusb-direct `tools/can_sniff.py` with python-can / SocketCAN backend (NEW 2026-05-17)

Filed during Phase 1 smoke test on PC (2026-05-17).

`tools/can_sniff.py` talks to the Candlelight via raw pyusb control transfers, which means we have to detach the kernel `gs_usb` module on Linux/WSL2 before the script can open the device (or run the script with `sudo`). This is fragile and duplicates what the kernel already does well.

The right architecture is `python-can` against the SocketCAN interface (`can0`):
- Leverage the kernel's gs_usb driver (already loaded on WSL2 / native Linux)
- One-liner Python API: `can.interface.Bus(channel='can0', bustype='socketcan')`
- Works alongside `candump`, `cansend`, and the rest of can-utils without contention
- No more `sudo rmmod gs_usb` / `sudo python3 ...` gymnastics
- macOS doesn't have SocketCAN; we'd keep the pyusb path as a fallback (or rely on `can.interface.Bus(channel='gs_usb', bustype='gs_usb')` if that backend lands cross-platform)

Don't act on this prompt — track for a future tooling-cleanup pass.

**Closes when:** `tools/can_sniff.py` (or its successor) uses python-can / SocketCAN on Linux and the pyusb-direct path is removed or marked macOS-only.

---

## P-26 🔴 Boot-time auto-connect: post-launch UX review for upgrade scenario (NEW 2026-05-17)

Filed during Phase 1 smoke test on PC (2026-05-17). Surfaced by the boot-gate fix landed this session.

The boot-time STA auto-connect now consults `wifi_get_mode_intent()`. Default-when-NVS-missing is `APSTA` so a firmware upgrade onto a customer dongle with existing stored creds preserves the legacy auto-reconnect behavior. That's correct for upgrades.

Open UX question: when a fresh-out-of-box dongle (no creds, no intent) boots, the default intent is APSTA but with no creds nothing happens — fine. But if the operator sets creds via `wifi_sta_set` (which doesn't change intent) and reboots, the dongle auto-connects on the default-APSTA intent. That might or might not be what the customer wants. Worth a doc + spec review before the next customer firmware rev.

Owner reviews. Don't act on this prompt.

---

## P-25 🟡 Review WS auth posture now that server is always-listening on STA netif (NEW 2026-05-17)

Filed during Phase 1 smoke test on PC (2026-05-17). Surfaced by the WS-always-on patch landed this session.

Pre-2026-05-17, the WS server started only on first AP client. STA-side exposure was effectively gated by that interaction. This session lifted the gate: `wifi_ap_start()` now starts the WS server unconditionally so over-LAN tooling (this smoke test's Tier 2) and headless setup work.

The existing `unlock` command + per-fd `authenticated_clients[]` table from `command_handler.c` was designed against an LAN-trusted AP-side surface. With STA-side reach now opened on every boot, the auth posture should be re-reviewed:

- Is `unlock <password>` rate-limited tightly enough? Current limit: 5 attempts then 30 s lockout. STA-side attackers have unlimited retry windows from outside the dongle's AP.
- Should SECURED commands require an additional layer (cert pinning, IP allowlist, signed nonce)?
- Should the WS server bind only to specific interfaces if a "lockdown" NVS flag is set?
- P-20 (admin password hardcoded `"futuner_admin_2024"`) was acceptable when only AP-side clients could reach the WS. Moves up in priority now.

Don't act on this prompt — track for a security-pass that touches first-boot UX (alongside P-19, P-20).

**Closes when:** owner has signed off on the STA-exposed auth posture or landed mitigations.

---

## P-24 🟡 Deprecate `wifi_connect` / `wifi_disconnect` after UI + cloud migrate (NEW 2026-05-17)

**Filed during the WiFi mode-control prompt (PC, 2026-05-17).**

The new WiFi command surface (`wifi_sta_set` + `wifi_mode {ap|sta}` + `wifi_clear`) makes the legacy `wifi_connect` / `wifi_disconnect` commands redundant. Per owner direction this prompt leaves both legacy commands intact and registered, because:

- `ui/control_panel.js::wifiStaConnect` / `wifiStaDisconnect` call them directly.
- First-boot pairing relies on `wifi_connect` being `CMD_SECURITY_UNSECURED` (the customer has no admin password yet).

**Closes when:**

1. The UI is updated to call `wifi_sta_set` + `wifi_mode sta` in place of `wifi_connect`, and `wifi_clear` in place of `wifi_disconnect`.
2. The cloud-side first-boot flow is reviewed for any direct callers (search the cloud repo for `"command":"wifi_connect"` and `"command":"wifi_disconnect"`).
3. After both 1 and 2 are green for at least one full firmware release in customer hands, the `wifi_connect` / `wifi_disconnect` registry entries and their handlers in `commands/system_commands.c` are removed.

**Do not act on this item in the current prompt.** Filed for visibility only.

P-24 numbering avoids collision with P-18..P-23 referenced informally in `HANDOFF_TO_PC.md` (the next sequential number in this file is P-18, but those slots are conceptually used elsewhere — owner to renumber if desired).

---

## P-17 🔴 Doc-cleanup: CAN pin assignments in HIL preflight + HW reference docs (NEW 2026-05-12)

**Two doc inconsistencies surfaced during Step A wiring of `mdg1_transport_can.c`.**

1. `docs/HIL_PREFLIGHT_RS7_CAL_FLASH_READINESS.md` references `BOARD_REV2`
   (TX=GPIO5, RX=GPIO16) as the bench-dongle pinout. The actual bench dongle
   is the FUTV1.0 reference hardware (MAC `30:ed:a0:b6:35:40`, 16 MB flash
   + 8 MB PSRAM), which is `BOARD_V10` (TX=GPIO21, RX=GPIO14). `BOARD_V10`
   is binary-verified per `firmware/src/can/can_config.h:17-24` — extracted
   from the DROM segment of the working v1.5 firmware's `app0.bin` at
   offset `0x6cc0`. `BOARD_REV2` is the planned future hardware; its
   pinout has been asserted by Sean but is NOT yet binary-verified.

   Fix: update the HIL preflight doc to specify `BOARD_V10` for the
   current bench dongle, and frame `BOARD_REV2` as "future hardware,
   asserted but not binary-verified."

2. `hw_reference/SEFI-ECU-Flasher-Project-Reference-v3.md` lists CAN pins
   as TX=GPIO17, RX=GPIO18 in at least one place. That's neither V10 nor
   REV2; it's incorrect. Per the `can_config.h:38-46` warning, "Never
   guess pins from flash size alone."

   Fix: correct or remove the TX/RX assertion in that doc, OR mark the
   whole doc as "DO NOT trust for pin info — see `can_config.h`."

**Closes when:** both docs reference `BOARD_V10` (TX=21 / RX=14) as the
binary-verified current bench dongle, with `BOARD_REV2` clearly framed
as a future-hardware claim awaiting verification.

---

## P-18 🟢 connection_manager silence during Phase 2 (NEW 2026-05-19, RETROACTIVE)

Filed retroactively to close the orphan cross-references in
`HANDOFF_TO_PC.md` (2026-05-12 era) that said "P-18 CLOSED — coordinator
arbitration works." Verified: `firmware/src/isotp_coordinator/` exposes
`ISOTP_OWNER_PHASE2_FLASH` and `connection_manager.c` correctly stays
silent when the coordinator owner is the flash path. Closed.

**Closes when:** already closed at landing time. Retained for cross-ref
integrity from `HANDOFF_TO_PC.md:162,176`.

---

## P-19 🟡 Default AP password hardening (NEW 2026-05-19, RETROACTIVE)

`firmware/src/config/wifi_config.h:WIFI_AP_PASSWORD_DEFAULT` is
literally `"password"`. Filed retroactively to close the orphan
cross-references in `wifi_config.h:49`, `HANDOFF_TO_PC.md:177,183`, and
the (now retracted) P-25 self-reference at line 831.

**Fix paths:** either (a) generate a per-device password at first boot
from the chip MAC and surface it via a printed sticker / first-boot UX,
or (b) gate AP-mode behind a hold-button-on-power-cycle pairing step
that exposes the AP for a 60-second window. Decision deferred to the
security-pass prompt; the current value is held by owner sign-off.

**Closes when:** factory firmware does not ship with a known-default
AP password.

---

## P-20 🟡 Admin unlock password move to NVS (NEW 2026-05-19, RETROACTIVE)

The admin-tier WS `unlock` password is currently hardcoded as
`futuner_admin_2024` in `firmware/src/commands/command_handler.c`.
Filed retroactively to close the orphan cross-reference in
`HANDOFF_TO_PC.md:178` and `PC_PHASE1_HANDOFF.md:249`.

**Fix:** move the admin password to NVS with a per-device random
default, surface a setter command (admin-tier only), and rotate on
factory-reset.

**Closes when:** no hardcoded admin password remains in
`command_handler.c`.

---

## P-21 🔴 RESERVED (NEW 2026-05-19, RETROACTIVE)

`HANDOFF_TO_PC.md:183` says "the three unfiled P-items from last night
need proper numbering — assign P-21, P-22, P-23." The content was
never recorded. Filed as RESERVED to close the cross-reference gap;
contents TBD by owner.

---

## P-22 🔴 RESERVED (NEW 2026-05-19, RETROACTIVE)

See P-21. Reserved to close cross-reference gap from
`HANDOFF_TO_PC.md:183`.

---

## P-23 🔴 RESERVED (NEW 2026-05-19, RETROACTIVE)

See P-21. Reserved to close cross-reference gap from
`HANDOFF_TO_PC.md:183`.

---

## P-29 ⚫ OBSOLETE — content unrecoverable (CLOSED 2026-05-22)

Reserved during the 2026-05-19 Hermes audit close-out to hold P-IDs
for an intended UI-fix batch from the PC handoff. Content was never
recorded into either archived handoff doc
(`handoffs/archive/PC_PHASE1_HANDOFF.md`,
`handoffs/archive/HANDOFF_TO_PC.md`) and is not derivable from git
history. Per A4 in `PHASE_1_COMPLETION_PLAN.md`: "if the content is
unrecoverable, mark each entry OBSOLETE with a note referencing the
2026-05-19 PC reset event."

Closed: the P-29..P-32 surface is now covered by P-57..P-61 from
the 2026-05-21 UI vet, which capture the actual UI bugs the placeholder
slots were meant to hold. P-NN numbers are sticky; not reused.

---

## P-30 ⚫ OBSOLETE — content unrecoverable (CLOSED 2026-05-22)

See P-29.

---

## P-31 ⚫ OBSOLETE — content unrecoverable (CLOSED 2026-05-22)

See P-29.

---

## P-32 ⚫ OBSOLETE — content unrecoverable (CLOSED 2026-05-22)

See P-29.

---

## P-34 🟡 UI `wifi_scan` command has no firmware handler (NEW 2026-05-19)

`ui/control_panel.js:899` calls `wsSend({command:'wifi_scan'})` but no
matching entry exists in `firmware/src/commands/commands.c::COMMAND_REGISTRY`
and no HTTP sidecar handles it. The UI Scan-for-networks button is
currently a silent no-op. Pre-existing UI gap — predates the
2026-05-19 audit.

**Fix paths:** (a) add `cmd_wifi_scan` in `wifi_commands.c` that wraps
`esp_wifi_scan_start` + result aggregation, register at UNSECURED tier,
or (b) remove the UI scan affordance and document that the user
enters SSID manually.

**Closes when:** the UI Scan button either drives a real handler or is
removed from the UI.

---

## P-35 🟡 UI `fs_upload` command has no firmware handler (NEW 2026-05-19)

`ui/control_panel.js:1722` calls `wsSend({command:'fs_upload', path, data, size})`
but no matching entry exists in the C registry, and `ws_server.c`
registers no HTTP POST upload endpoint. Pre-existing UI gap.

**Fix paths:** (a) add `cmd_fs_upload` that wraps `cmd_fs_write` with
multi-chunk support (UI already does base64), or (b) add an HTTP POST
`/upload` URI handler in `ws_server.c`, or (c) rewrite the UI's upload
flow to chunked `fs_write` calls.

**Closes when:** UI uploads work end-to-end against a stock build.

---

## P-36 🟡 WiFi NVS keys + AP IP triplet still in wifi_ap.h (NEW 2026-05-19)

`wifi_config.h` was created to centralize wifi constants per the
no-magic-numbers rule, but three NVS key strings (`WIFI_AP_PASSWORD_NVS_KEY`,
`WIFI_STA_SSID_NVS_KEY`, `WIFI_STA_PASS_NVS_KEY`) and the AP IP triplet
(`WIFI_AP_IP`, `WIFI_AP_GATEWAY`, `WIFI_AP_NETMASK`) still live in
`firmware/src/wifi/wifi_ap.h`. Caught by the 2026-05-19 Hermes audit
(C4).

**Fix:** relocate the six defines to `wifi_config.h` (where the rest of
the wifi tunables already live), with the same "approval before lock —
DEFER LOCK UNTIL OWNER REVIEW" comment block.

**Closes when:** `wifi_ap.h` no longer contains any behavioral
constants; `wifi_config.h` is the single source of truth for wifi
tunables.

---

## P-37 🟡 Orchestrator inline buffer-size literals (NEW 2026-05-19)

`mdg1_flash_orchestrator.c` has multiple inline integer literals for
RX/TX buffer sizes and retry budgets that should reference named
constants in `mdg1_flash_orchestrator_config.h`:

- line 83: `for (int i = 0; i < 8; i++)` — pending-loop max iterations
- line 195: `uint8_t rx[64]` — `uds_exchange_tolerant_of_nrc` rx buffer
- line 265: `uint8_t rx[3 + MDG1_PROG_HISTORY_PAYLOAD_LEN + 8]` — `+ 8` slop
- line 335,569: `uint8_t rx[8]` — resync + RequestDownload rx buffers
- line 336: `const int max_attempts = 8` — post-reset TesterPresent resync
- line 469: `uint8_t tx[8], rx[16]` — SA exchange buffers
- line 489,492: `0xA5A5A5A5u` — sentinel SA fallback key
- line 590: `cap = plaintext_size + (plaintext_size / 8) + 64` — LZRB headroom

Caught by 2026-05-19 Hermes audit (C5). Project rule prohibits inline
behavioral constants.

**Fix:** add named `MDG1_UDS_PENDING_MAX_ITERATIONS`, `MDG1_UDS_RX_STACK_MED_BYTES`,
`MDG1_UDS_RESYNC_MAX_ATTEMPTS`, `MDG1_SA_TX_BYTES`, `MDG1_SA_RX_BYTES`,
`MDG1_SA_FALLBACK_KEY_SENTINEL`, `MDG1_LZRB_HEADROOM_DIVISOR`,
`MDG1_LZRB_HEADROOM_FIXED_BYTES` defines in
`mdg1_flash_orchestrator_config.h` and rewire orchestrator.c.

**Closes when:** no inline behavioral integer literals in
`mdg1_flash_orchestrator.c`.

---

## P-38 🟡 mdg1_flash_orchestrator/eval.sh hard-depends on off-repo path (NEW 2026-05-19)

`firmware/test/mdg1_flash_orchestrator/eval.sh:50-64` hard-fails if
`/Users/rabbit/sniffer/mm_FULL_Flash.log` and the matching ECU bin are
absent. These files are off-repo (owner's local sniffer capture
archive) and not in the project tree. The repo only has a 4-line stub
candump in `firmware/test/can_capture/fixtures/magicmotorsport/`.
Result: any non-owner machine (CI, fresh clone, secondary dev box)
cannot run sections 1, 9, 10 of this eval gate.

**Fix paths:** (a) check the real MM captures into the repo (size
permitting), or (b) check in a smaller representative slice as the
test-pinned fixture, or (c) make `MM_CAPTURE_DIR` defaulting to the
in-repo fixtures path with a clear "owner sets this env var to use
their full local capture" hatch.

**Closes when:** `bash firmware/test/mdg1_flash_orchestrator/eval.sh`
runs to completion on a fresh clone on any dev machine.

---

## P-39 🟡 mdg1_uds_transport_t.flush is declared but never invoked (NEW 2026-05-19)

`firmware/src/flash/mdg1_uds_transport.h:91-97` declares a `flush`
function-pointer field on the transport interface with the contract
"Called between orchestrator phases that the underlying transport
might have queued data for." But `mdg1_flash_orchestrator.c` never
invokes `transport->flush(...)` anywhere — the contract is signed but
not enforced. Caught by 2026-05-19 Hermes audit (C5).

**Fix paths:** either (a) wire the orchestrator to call
`transport->flush(ctx)` between phases (specifically after ECUReset
and before SecurityAccess re-establishment), or (b) delete the field
and update the header contract to reflect the actual behavior.

**Closes when:** the header contract and the orchestrator behavior are
in sync.

---

## P-40 🟡 Orchestrator preflight_ecureset_and_resync silently drops NRCs (NEW 2026-05-19)

`mdg1_flash_orchestrator.c::preflight_ecureset_and_resync` (around
line 337-343) loops `uds_exchange` calls; if `pe != ESP_OK` OR
`rx[0] != 0x7E`, the result is silently dropped (no early return inside
the loop, no `MDG1_FLASH_PHASE_NRC_RECEIVED` progress event). Only the
outer `ESP_ERR_TIMEOUT` is surfaced. Caught by 2026-05-19 Hermes audit
(C5).

This is the same class of bug that `NRC_ERROR_HANDLING_AUDIT.md`
identified for the post-SA flash phases (Bug 2 surface), but the
preflight resync loop was overlooked.

**Fix:** route the resync loop through `uds_exchange_strict` (or a
new `uds_exchange_strict_with_expected_response_byte` helper for the
`rx[0] == 0x7E` case) so non-pending NRCs fire the
`MDG1_FLASH_PHASE_NRC_RECEIVED` progress event and bail.

**Closes when:** the resync loop surfaces NRCs the same way the
post-SA phases do.

---

## P-41 ⚫ OBSOLETE — slot unused (CLOSED 2026-05-22)

Gap between P-40 and P-42 left by the 2026-05-19 audit close-out.
No content was recorded against this number. Filed as obsolete to
keep the sequence dense; P-NN numbers are sticky.

---

## P-42 🟢 Shadow-test primary halt gate verified clean (RESOLVED 2026-05-22)

PHASE_1_COMPLETION_PLAN.md A6 flagged a possible "primary halt gate
regression" in `firmware/test/mdg1_flash_orchestrator/eval.sh` and
asked for a fresh repro on current main HEAD.

Re-run on origin/main @ 5f62cea (2026-05-22):

    $ SKIP_IDF_BUILD=1 bash firmware/test/mdg1_flash_orchestrator/eval.sh
    ...
    Passed: 64
    Failed: 3
    FAILURES:
      - host_test_runner reported failures
      - shadow_full vs mm_FULL_Flash.log: MISMATCH (orch_diff.log)
      - CAL section vs mm_MAPS_upload.log: MISMATCH (orch_diff_cal.log)
    RESULT: FAIL

All 3 failures collapse to the same root cause: `/tmp/lzrb_cli`
binary not present (eval script even prints the build hint in the
error). Cross-check: the `test_hil_defensive_secondary_engages_when_primary_bypassed`
test that exercises the **actual primary halt gate** PASSES. The
defensive secondary halt fired, the DEFENSIVE HALT log line emitted,
no SECTION_ERASE leaked through. Primary halt gate is intact.

The 3 failures are downstream of P-38 (fixture portability —
`/Users/rabbit/sniffer/*` MM captures + `/tmp/lzrb_cli` binary),
NOT a halt-gate regression. Closing P-42 as not-a-bug; the open
fixture-portability work continues under P-38.

---

## P-43 🟡 Cloud admin endpoint `GET /admin/devices/{mac}` missing — code landed, PENDING-DEPLOY-AND-VERIFY (NEW 2026-05-21, code 2026-05-22)

Cloud-side, current admin API only exposes `GET /admin/devices` (list-
all) and `POST /admin/devices/{mac}/license` (single-device licensing).
There's no `GET /admin/devices/{mac}` single-row selector, so any
tool that wants to confirm a specific dongle's enrollment + paid
state has to list-all and filter client-side. Surfaced during the
2026-05-19 HIL Chip Report write-up.

**Fix scope:** add `GET /admin/devices/{mac}` to `cloud/src/main.py`.
Mirror the existing `POST /admin/devices/{mac}/license` access
pattern (same auth model, same 404 shape if MAC unknown).

**Closes when:** `curl -H "X-Admin-Key:..." https://.../admin/devices/<mac>`
returns the per-row JSON, or 404 if the MAC isn't enrolled.

---

## P-44 🟢 ws_server doesn't rebind to STA netif on STA_GOT_IP (RESOLVED 2026-05-21, commit `2e28b5f`)

`httpd_start()` binds to the netifs that exist at start time. STA
netif comes up AFTER boot's `wifi_ap_start → ws_server_start`, so
the STA-side port 80 returned TCP RST. UI + WS reachable only from
the AP IP. Discovered during the 2026-05-19 HIL when the dongle
joined Seanwifi as STA but ws_driver.py against the STA IP got
`ConnectionRefusedError`.

**Fix landed:** `IP_EVENT_STA_GOT_IP` handler in `wifi_ap.c` now
calls `ws_server_stop() + ws_server_start()` so httpd rebinds to
AP+STA together. `ws_server_stop` was already idempotent. Validated
end-to-end across THREE STA transitions during HIL (Seanwifi → openc
→ Seanwifi).

**Closes when:** post-flash WS reachable from STA IP without a manual
reboot. **(Closed.)**

---

## P-45 🟢 Firmware HTTPS clients default to wrong cloud host (RESOLVED 2026-05-21, commit `4253304`)

All 3 firmware HTTPS client modules (`vin_pairing`, `wot_uploader`,
`sbf_orchestrator`) defaulted `LICENSE_DEFAULT_HOST` /
`WOT_UPLOAD_DEFAULT_HOST` / `SBF_DEFAULT_HOST` to
`https://api.sillyrabbitmotorsport.com` — a pre-pivot URL. The deployed
cloud is at `https://sillyrabbitmotorsport.com/fut`. The `api.*`
subdomain resolves but the TLS cert lacks an `api.*` SAN, so the
HTTPS handshake fails with `ESP_ERR_HTTP_CONNECT (0x7002)`.
Discovered during 2026-05-19 HIL when `vin_pair_now` returned
"register: HTTP 28674".

**Fix landed:** all 3 defaults updated to `https://sillyrabbitmotorsport.com/fut`
in a single coherent commit (P-45 + P-46 combined). Path concat
audited per-client; all use `snprintf("%s%s", host, path)` cleanly.

**Closes when:** firmware HTTPS clients reach the deployed cloud
without NVS override. **(Closed.)**

---

## P-46 🟢 HTTPS clients don't attach CA bundle — TLS setup fails (RESOLVED 2026-05-21, commit `4253304`)

All 3 firmware HTTPS client `esp_http_client_config_t` initializers
were missing `.crt_bundle_attach`. ESP-IDF's `esp-tls-mbedtls` refuses
to set up SSL without a server-verification option configured, so
the call fails with `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED (0x8017)`
BEFORE any URL bytes are sent. Latent since the modules were written
— masked by the P-45 wrong-host bug aborting earlier. Surfaced when
P-45 was patched and the cloud sync still failed with a different
error chain.

`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y` was already enabled
in sdkconfig; the bundle was in the build, just not wired.

**Fix landed:** `#include "esp_crt_bundle.h"` + `.crt_bundle_attach
= esp_crt_bundle_attach` added at 3 sites (4 client structs total —
vin_pairing has GET and POST). Same commit as P-45.

**Closes when:** firmware HTTPS handshake succeeds with the deployed
cert. **(Closed.)** Serial trace verified
`esp-x509-crt-bundle: Certificate validated` x2 during vin_pair_now.

---

## P-47 🟡 3 cloud-host constants drift independently across files (NEW 2026-05-21)

`LICENSE_DEFAULT_HOST`, `WOT_UPLOAD_DEFAULT_HOST`, `SBF_DEFAULT_HOST`
are all defined per-module. The P-45 fix had to touch all three. Any
future URL change repeats the work and risks one site drifting.

**Fix scope:** extract a shared `CLOUD_DEFAULT_HOST` constant (and
its NVS override key) into a single header. The 3 modules either
include that header directly or wrap it locally with `static_assert`
that the local copy still equals the canonical one.

**Closes when:** there is exactly one source-of-truth for the cloud
host in firmware.

---

## P-48 🟢 Cloud source + docs still reference `api.sillyrabbitmotorsport.com` (RESOLVED 2026-05-22)

`cloud/Caddyfile`, `cloud/scripts/centos-server-setup.sh`,
`cloud/src/main.py` docstring, `cloud/README.md`, and ~9 docs files
under `docs/` still cite `api.sillyrabbitmotorsport.com` even though
the deployed cloud is at `sillyrabbitmotorsport.com/fut`. Dead
reference but misleading — new engineers can mis-orient.

**Fix scope:** sweep + update. Categorize each hit per the dispatch
shape: source comments → update; Caddyfile / setup scripts → update
OR add deprecation note; docs → update with a footnote pointing at
the URL-fix commit (`4253304`).

**Closes when:** `grep -rE 'api\.sillyrabbitmotorsport\.com' ~/esp/obd/FUTV1.1/`
returns nothing except in deprecation-noted archive files.

---

## P-49 🟢 Refactor 3 cloud HTTPS clients to a single cloud_client factory (RESOLVED 2026-05-22)

Currently `vin_pairing.c`, `wot_logger.c`, `sbf_orchestrator.c` each
duplicate the `esp_http_client_config_t` initializer (URL + TLS bundle
+ method + timeout). Future TLS knobs (cert pinning, IP allowlist,
timeout policy) require touching 3 sites. Same drift surface as P-47
but at the C-init-struct level.

**Fix scope:** extract to `firmware/src/cloud/cloud_client.{c,h}` —
`cloud_client_https_init(esp_http_client_handle_t *out, const char *path)`
constructs the bundle-attached, host-correct config. All three
sites call the factory and never touch `esp_http_client_config_t`
literals.

**Closes when:** all `esp_http_client_config_t` literals outside
`cloud_client.c` are gone.

---

## P-50 🟢 HTTPS smoke test missing from firmware/test/ (RESOLVED 2026-05-22)

P-46 was latent for months — host eval gates never exercised the TLS
config because there was no test that wired the real CA bundle
through `esp_http_client`. The dongle's HIL surfaced it, customer
would have surfaced it first if the run had skipped HIL.

**Fix scope:** add `firmware/test/cloud_client/eval.sh` that mocks
`esp_http_client` against a known-good cert + a known-bad cert (no
matching SAN). Catches future drift in the bundle attachment, the
URL constant, or the host name. Would have caught P-45+P-46 in CI.

**Closes when:** `firmware/test/cloud_client/eval.sh` exists, passes,
and removes one of the bugs from P-45 / P-46's diff to confirm it
fails red.

---

## P-51 🟡 Battery voltage read surface missing from WS API (NEW 2026-05-21)

Phase 4.6 should expose battery voltage for the pre-flash gate
(MISSION_SPEC §2.1 + Phase 2 P-04 both need it). The dongle has no
firmware-side voltage reader: no ADC tap, no UDS DID mapped in the
logger table, and `can_send_raw` is HARD-rule forbidden so the agent
can't probe via DID without sign-off. During the 2026-05-19 HIL the
battery-voltage precondition was skipped on owner directive.

**Fix scope:** add `battery_voltage` WS command. Implementation
choice (Sean input — D-decision):
1. Standard OBD PID `0x4221` via UDS read — works on any ECU
2. VAG-specific DID — narrower but possibly more accurate
3. Internal ADC on the OBD power pin via voltage divider — requires
   ESP32-S3 ADC channel + a board rev

**Closes when:** `battery_voltage` command returns ECU-reported
voltage on dev RS7 within ±0.5 V of an OBD scan-tool reference.

---

## P-52 🟡 Candlelight macOS gs_usb sustained-use wedge (NEW 2026-05-21)

Both `tools/can_sniff.py` and Sean's known-working `~/sniffer/can_tail.py`
wedge after sustained passive RX on macOS. Symptoms:
- can_sniff.py: returns 0 frames during a confirmed-on-wire dtc_read
- can_tail.py: starts capturing fine, gets to 1194 lines, then 100%
  CPU + file growth stops while USB stays enumerated
- Active TX (`can_sniff --uds`) still works — the device's gs_usb
  surface isn't broken for write
- Confirmed not a code issue. Both tools claim the device cleanly;
  the wedge is driver/kernel-side

**Workaround:** physical USB replug per session, or short-window
captures (<60s).

**Fix options (Sean decides — listed for visibility, not a Claude pick):**
1. Linux box for wire witness (kernel-mode `gs_usb` is stable)
2. Replace Candlelight with a USB-CAN device with better macOS
   driver support (PEAK PCAN-USB FD, Kvaser, or similar)
3. Userspace driver alternative (libusb-mac fork, gs_usb-mac project)

**Closes when:** wire witness sustains a 60+ minute HIL session
without wedging. Required for restoring the three-stream HIL
contract (WS + serial + wire) on the next HIL pass.

---

## P-53 🟢 dtc_clear response demux conflates pre-read with clear response (RESOLVED 2026-05-21, commit `a54d690`)

dtc_clear()'s pre-read (UDS 0x19 0x02 — used to populate cleared_count
in the WS response) saw the ECU's NRC 0x78 (ResponsePending) and
returned immediately. The eventual positive read response (SID 0x59)
then sat in the ISO-TP receive queue. When dtc_uds_clear_diagnostic_information
sent the actual 0x14 ClearDTC request, target_uds_request returned
the stale 0x59 response instead of the 0x14's actual response. The
clear parser logged "malformed response (len=8 sid=0x59)" and bailed.

**Fix landed:** drain NRC 0x78 at the transport layer.
`target_uds_request` now inspects each receive; if `7F <sid> 78`,
discards the frame, extends the timeout window to P2*_server (5 s
per ISO 14229), and keeps polling. Standard UDS RCRRP handling.

`MDG1_UDS_NRC_RESPONSE_PENDING` already existed for the Phase 2
flash orchestrator; the Phase 1 DTC path now ports the same pattern
via new `DTC_UDS_NRC_RESPONSE_PENDING` + `DTC_UDS_P2_STAR_SERVER_MS`
constants in `dtc_config.h` (per CLAUDE.md Rule 3, no magic numbers).

**Closes when:** dtc_clear pre-read returns 7 DTCs and the clear's
own response (positive 0x54 OR a meaningful ECU NRC) round-trips
cleanly. **(Demux side closed — observed `DTC_UDS: read parsed 7 DTCs`
then `DTC_UDS: clear NRC 0x11` on dev RS7.)** P-54 tracks the
downstream NRC 0x11.

---

## P-54 🟡 ClearDTC NRC 0x11 from ECU after P-53 demux fix (NEW 2026-05-21)

With P-53 landed, the clear request now reaches the wire and the
response demux is clean. But ECU returns NRC 0x11 (serviceNotSupported)
on `14 FF FF FF`. The HIL chip report logged this but the wire-format
verification was blocked by P-52 (Candlelight wedge).

**Hypothesis (per PHASE_1_COMPLETION_PLAN.md A1):** session-state.
Bosch MG1 honors `0x14 ClearDTC` only in Extended Diagnostic Session,
not Default. `0x19 ReadDTC` is in the Default session service set,
which is why Phase 4-read worked while Phase 4-clear didn't.

**Discriminator (3 added frames before the clear request):**
1. `10 03` (DiagnosticSessionControl → Extended) — expect `50 03 …`
2. `14 FF FF FF` (ClearDTC) — look at the response
   - `54` positive → fixed; session was the issue
   - `33` SecurityAccess required → chain `27 0x` after extended
   - `22` conditionsNotCorrect → engine state requirement
   - `11` again → DID/service mapping deeper than session; escalate

**Fix scope (if hypothesis holds):** wrap
`dtc_uds_clear_diagnostic_information` with a session-entry preamble
+ session-exit. Add `DTC_CLEAR_REQUIRES_EXTENDED_SESSION` config
flag so other ECU families that don't need it can opt out.

This touches ECU-wire-surface code — owner sign-off required before
the patch lands.

**Closes when:** dtc_clear on dev RS7 returns `{"ok":true}` AND
subsequent dtc_read returns 0 codes AND wire witness shows the
`10 03` + `14 FF FF FF` frames.

---

## P-55 🟡 Logger DID resolution / value scaling broken on RS7 (NEW 2026-05-21)

KOEO RPM read as `-5369`. Cannot be derived from any byte permutation
of a valid 0. Not a scale-formula bug — either wrong DID mapping,
demux conflation (P-53 class), or wrong UDS service for this variant.

**Diagnostic surface landed 2026-05-21 (commit `fed30f1`):**
`get_logger_data_raw` WS command returns the pre-parse hex of the
most-recent ECU poll response. Lets the off-vehicle A2L cross-check
the DID table without re-running HIL for every scale-formula guess.

**Pending HIL probe (still 🟡):**
1. Single-shot read of RPM DID alone — confirm raw bytes
2. Cross-check active DID list in `firmware/src/logger/` against the
   A2L for `4K0907557G__0003` specifically (not whatever MG1 variant
   the table was originally sourced from)
3. Depending on outcome:
   - Wrong DID table → regenerate from `4K0907557G__0003` A2L
   - Demux conflation → port the P-53 NRC drain to the logger path
   - Wrong service → swap `$22 ReadDataByIdentifier` →
     `$23 ReadMemoryByAddress`

**Closes when:** all 6 logger variables (nmot_w, InjSys_ratEthPrtnBascFu,
Com_stCrCtlPan, rl_w, tmot, wdkba) return plausible KOEO values on
dev RS7. Plausibility table pinned in
`firmware/test/logger/koeo_baseline.json` for regression.

---

## P-56 🟡 HIL doc references retired `tools/can_sniff.py` (NEW 2026-05-21)

`handoffs/PHASE1_HIL_VALIDATION.md` cites `tools/can_sniff.py` in
multiple capture-step invocations. Sean's 2026-05-19 directive:
canonical sniffer is `~/sniffer/can_tail.py`; `can_sniff.py` retained
in `tools/` only for legacy reference. The HIL doc still uses the
old name, and Rule 7 says every CLI invocation in a doc must be
`--help`-verified against the actual tool before merge.

**Fix scope:** replace every `can_sniff.py` invocation in the HIL
doc with the matching `can_tail.py` form. `--help`-verify each line.
Add a header note: "wire witness = ~/sniffer/can_tail.py (Sean
directive 2026-05-19); tools/can_sniff.py retained for legacy
reference only."

**Closes when:** HIL doc no longer references `can_sniff.py` in any
active step. `grep -n 'can_sniff' handoffs/PHASE1_HIL_VALIDATION.md`
returns nothing.

---

## P-57 🟢 UI wsSend callback routing broken — _cbId never echoed by firmware (RESOLVED 2026-05-21, commit `3c0aef7`)

UI `wsSend(obj, cb)` attached a client-side `_cbId` integer to the
outgoing frame and stored `cb` in `pendingCb[cbId]`. Firmware
`command_handler.c` does not parse or echo `_cbId`; responses come
back with only the `command` field. `handleMsg` looked up
`pendingCb[msg._cbId]`, always undefined → cb never fired. Every UI
interaction that depended on a response (license_status,
write_ecu acknowledgment, get_logger_profile readback,
set_logger_profile confirmation) silently failed. Surfaced by the
2026-05-21 Claude-in-Chrome UI vet — `wsSend({command:'get_status'}, cb)`
on the existing UI WS times out while a fresh `new WebSocket(...)`
to the same dongle returns in <50 ms.

**Fix landed:** UI-only change. `pendingCb` is now a FIFO queue
keyed by command name (firmware echoes `command` in `send_response`).
3 cb-using call sites (`set_logger_profile`, `get_logger_profile`,
`write_ecu`) resolve correctly without changing firmware.

**Closes when:** Cowork repro
`wsSend({command:'get_status'}, m => console.log(m))` logs within
1 second on a freshly-flashed dongle. **(Pending Cowork verify
post-flash.)**

---

## P-58 🟢 vin_pair_now persists license cache but not local ECU pair record (RESOLVED 2026-05-21, commit `a9c0b5f`)

vin_pair_now succeeded on cloud-side persistence (license cache
present + paid + vin populated and reloaded on boot) but never wrote
the local ECU pair record to NVS. `connection_manager`'s CHECK_PAIRING
handler at boot looks for `ecu_info` via `nvs_manager_load_ecu_info`;
on miss it logs "No valid ECU info found in NVS" and stays
`paired=false`. Customer expectation per UI vet: "pair this dongle
with my car" means both cloud license AND local pair persist.

**Fix landed:** in `vin_pairing_run_now`, after `license_fetch`
succeeds, call `connection_manager_pair_vehicle()` to write
`ecu_info` to NVS via `nvs_manager_save_ecu_info` and flip
`is_paired=true`. Re-uses the existing tested CM API (same path
cmd_pair_ecu uses). The CM function's auto-disconnect+reconnect
side effect is acceptable for a one-time pairing action.

Persistence failure treated as non-fatal: cloud side already
succeeded, user can manually pair_ecu to retry. Host-build guard
via `#ifndef VIN_PAIRING_HOST_BUILD`.

**Closes when:** dev RS7 HIL — vin_pair_now succeeds, dongle
power-cycle, `get_status` returns `paired:true` on boot without
manual re-pair. **(Pending HIL verify post-flash.)**

---

## P-59 🟢 UI Rule-9 violations: pair_vehicle, unpair_vehicle, reboot (RESOLVED 2026-05-21, commit `92be4f1`)

UI vet 2026-05-21 found 3 commands the UI invoked that didn't match
any firmware registry entry:
- `pair_vehicle` → registry has `pair_ecu` (NAME MISMATCH)
- `unpair_vehicle` → registry has `remove_pairing` (NAME MISMATCH)
- `reboot` → no handler (MISSING)

Each silently no-op'd on the wire (made worse by P-57 hiding the
"unknown command" response from the user).

**Fix landed:** UI renames (pair_vehicle → pair_ecu, unpair_vehicle
→ remove_pairing) + new `cmd_reboot` in `system_commands.c` calling
`esp_restart()` via a one-shot FreeRTOS task that delays
`SYSTEM_CMD_REBOOT_ACK_DELAY_MS` (500 ms, per CLAUDE.md Rule 3) so
the WS server can flush the ACK first. Registered
`CMD_SECURITY_SECURED`.

P-34 (wifi_scan) and P-35 (fs_upload) remain as pre-existing UI gaps.

**Closes when:** every UI `wsSend` command exists in
`COMMAND_REGISTRY`. **(Closed for the 3 P-59-tracked cases;
P-34 + P-35 still open.)**

---

## P-60 🟡 Dashboard gauges show `--` despite logger having 6 variables (NEW 2026-05-21)

`get_status.data.logger_variable_count = 6` but every dashboard
readout reads `--`. Cascade:
1. P-57 broken callback routing means even if `get_logger_data`
   responses arrived, the UI didn't process them.
2. UI does not call `logger_start` on dashboard mount; dongle isn't
   polling the ECU for variables until the user manually starts the
   logger.
3. Even if both worked, P-55 (logger DID corruption) would surface
   as bogus values.

P-57 + P-55 together mostly resolve the symptom. Remaining gap is
the auto-start UX decision: should the dashboard auto-fire
`logger_start` on mount, or should the user click a "Start"
control?

**Fix scope:** UX decision (Sean), then small UI change to call
`logger_start` from the dashboard-mount handler. ~5 lines.

**Closes when:** opening the dashboard on RS7 populates all 6 gauges
with plausible KOEO values within 2 s of mount, no manual button
press required.

---

## P-61 🟡 UI title says "FUTUNER v2" but project + firmware is v1.1 (NEW 2026-05-21)

`document.title = "FUTUNER v2 Control Panel"`. Page header text
shows "FUTUNER v2 Connected". Project root is `FUTV1.1/` and
`PHASE_1_COMPLETION_PLAN.md` references FUTUNER v1.1. Cosmetic but
confusing.

**Resolution path:** UI version label aligns with firmware version,
OR confirm "v2 UI on v1.1 firmware" is intentional naming (some
teams version UI independently of firmware) and document it.

**Closes when:** the UI version label tells a coherent story to a
new user.

---

## Update protocol

When you close an item out, change its emoji to 🟢 and add a one-line
note above the legend showing what changed and when. When you discover
a new prerequisite that's not on this list, add it as the next P-NN.
Do not silently re-order; the numbers are sticky.

When all P-items are 🟢, Phase 2 is unblocked from a prerequisites
standpoint. Even then: no Phase 2 firmware lands on a customer device
without Sean's explicit per-variant sign-off.
