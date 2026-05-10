# Phase 2 — Prerequisites Checklist

> Living document. Anything that MUST be true, captured, validated, or
> built before any Phase 2 (full 8 MB ECU binary reflash) code lands
> on a customer device.
>
> Owner: Sean / SRM Engineering. Updated as items close out.
> Created: 2026-05-05. Last revision: 2026-05-07.

---

## Status legend

- 🔴 NOT STARTED
- 🟡 IN PROGRESS / PARTIALLY ANSWERED
- 🟢 DONE

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

## P-01 🔴 MagicMotorsport flash capture session — reference UDS sequences

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

## P-08 🔴 Dongle firmware Phase 2 flash code written + eval harness green

The actual flash module under `firmware/src/flash/` — Phase 2 specific
work. Lots of sub-tasks: UDS challenge-response mirroring, AES-128-CBC
implementation (or hardware-accelerated path on ESP32-S3), chunked
TransferData with per-block checksums, fault recovery sequence,
integration with feature_manager (`FEATURE_PHASE2_FLASH`), pre-flash
gate enforcement. Eval harness diffs FUTUNER output against the
MagicMotorsport reference from P-01 and asserts bit-exact match.

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

## Update protocol

When you close an item out, change its emoji to 🟢 and add a one-line
note above the legend showing what changed and when. When you discover
a new prerequisite that's not on this list, add it as the next P-NN.
Do not silently re-order; the numbers are sticky.

When all P-items are 🟢, Phase 2 is unblocked from a prerequisites
standpoint. Even then: no Phase 2 firmware lands on a customer device
without Sean's explicit per-variant sign-off.
