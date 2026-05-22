# Phase 3 Prerequisites — Live Tuning Ecosystem

> Living document. Phase 3 silos all live-tune work (RAM writes,
> ethanol blending, SBF/STF builder, map switch UI, rev limiter
> safety) into its own phase, mirroring how Phase 2 silos the full
> binary flash.
>
> Owner: Sean / SRM Engineering. Created: 2026-05-22.
>
> **Companion docs:**
> - `MISSION_SPEC.md` (Phase 3 = §6 after the 2026-05-22 restructure)
> - `PHASE_1_COMPLETION_PLAN.md` (Phase 1 close gates Phase 3 start)
> - `PHASE_2_PREREQUISITES.md` (Phase 2 prereqs — separate destructive
>   flash workstream)
> - `SCALE_ARCHITECTURE_PROPOSAL.md` (server / scaling architecture)

---

## Gating rule

`FUTUNER_PHASE3_ENABLED` stays `0` in `firmware/src/config/futuner_config.h`
until every item in this doc is GREEN AND Phase 1 close is signed off.
Live tune is RAM-only (recoverable by reboot) but subscription-gated
+ license-gated + customer-experience risk surface. No carve-outs.

Phase 3 enable is NOT a substitute for Phase 1 close. Customer cars
must have Phase 1 PERFECT before Phase 3 features touch them, since
Phase 3 builds on the Phase 1 license / pair / logger / cloud
surfaces.

---

## Status legend

- 🔴 NOT STARTED
- 🟡 IN PROGRESS / PARTIALLY ANSWERED
- 🟢 DONE
- 🛑 BLOCKED — needs Sean's input
- ⚫ OBSOLETE / CLOSED-AS-UNRECOVERABLE — numeric slot kept reserved
  but content unrecoverable; not reused

---

## Phase 3 deliverables (high-level)

| § | Feature | Status |
|---|---------|--------|
| 6.1 | SBF live cal switching (RAM-write apply path) | 🟡 host gate PASS; never customer-experience validated |
| 6.1 | SBF/STF/FBF builder tool (XDF + A2L + bin + manifest → SBF) | 🛑 BLOCKED on `stuck/B1_sbf_builder.md` |
| 6.2 | Ethanol live-update constraint engine (§4.5 §1-5: threshold, dwell, WOT lockout, rev-limit drop, stabilization) | 🔴 NOT BUILT |
| 6.2 | Rev limiter safety reduction during write window | 🔴 NOT BUILT |
| 6.3 | 9 map switch slots UI (Live Tune tab) | 🔴 NOT BUILT (spec only) |
| 6.4 | Pre-apply safety gate (user-initiated apply safety logic) | 🔴 NOT BUILT (spec only) |
| 4.4b | Ethanol BLE bridge LIVE-TUNE FEED (separate from Phase 1's §4.4a logging-only) | 🔴 NOT BUILT |

Phase 1's `§4.4a` ethanol BLE LOGGING-ONLY remains in Phase 1 scope —
the logged-variable path doesn't intersect the live-tune feed.

---

## P-items (Phase 3)

Note on numbering: P-IDs are sticky per the workspace convention.
Phase 3 P-items use a `P3-NN` prefix to disambiguate from the global
`P-NN` Phase-2-prereqs list. Cross-references between docs use the
fully-qualified ID.

---

## P3-01 🛑 SBF/STF/FBF Builder Tool

**What:** Browser GUI + headless CLI that reads an XDF, A2L, base
binary, and per-variant manifest, produces a valid SBF (binary
format-v4 SCPN) the firmware accepts. Two workflows:

- **Create-from-scratch:** advanced operator builds a new variant
  manifest entry, picks an XDF, points at a base bin, sets ethanol
  variants, exports STF (human-readable) + SBF (firmware-loadable)
- **Edit-existing:** operator loads an existing STF, edits maps in
  the GUI (or per-map), re-exports

**Stages:**
- Stage 1 — STF round-trip parser (read + write byte-perfect) —
  status TBD; check git for what landed
- Stage 2a — Headless CLI that wraps the existing scorpion-bin-tools
  Node.js reference impl (or native Python port if the Node tool
  schema drift can't be patched cleanly — see `stuck/B1_sbf_builder.md`)
- Stage 2b — GUI shell (browser, served from dongle or standalone)
- Stage 2c — GUI map editor (per-map cell editing, ethanol anchor
  switching, dimension preview)
- Stage 3 — STF → SBF binary export (byte-identical to canonical
  Scorpion output, modulo timestamp)

**Block:** `stuck/B1_sbf_builder.md` 2026-05-22 — scorpion-bin-tools
shipped on disk has a schema drift (reads `target.base_file`,
canonical config supplies `target.input_file`). Patch vs port vs
regenerate-canonical decision is held for Sean.

**Closes when:** GUI can produce a byte-identical (modulo timestamp)
SBF to `~/esp/obd/SEFIV1.0/EXE TOOL/data/4K0907557G__0003/output/3stage1_patched.sbf`,
AND firmware loads the GUI-produced SBF cleanly, AND the new SBF
applies on dev RS7 (live RAM updates land per MISSION_SPEC §6.1).

---

## P3-02 🟡 Live cal switching — SBF apply path on RS7

**What:** Verify the existing firmware SBF orchestrator with a real
`stage1.sbf` uploaded to `/storage/sbf/` on dev RS7, KOEO. Today
host gate PASSes (94/0) but no on-car exercise has happened.

**Pre-work:**
- P3-01 produces `stage1.sbf` for `4K0907557G__0003`
- Upload to dongle via fs_upload WS or admin path
  (depends on P-35 resolution — Phase 1 P-item)

**Test sequence:**
1. Boot dongle on RS7, KOEO
2. Confirm `live_tune_start` enqueues without "no SBF file found"
3. Issue `live_tune_apply {stage:1, ethanol_pct:0}` over WS
4. Wire-witness the RAM write transactions (UDS WriteMemoryByAddress
   or vendor-specific live-write service — per `sbf_variants` table)
5. Read back a known sentinel address; verify pre/post values match
   the SBF spec
6. Restart, confirm RAM-only behavior (writes don't persist across
   ECU reset)

**Closes when:** All 6 steps PASS. Update latency observed at
1.5-2.0 s (per MISSION_SPEC §6.1). No NRC during apply. Sentinel
verifies. No persistence across ECU reset.

---

## P3-03 🔴 Ethanol live-update constraint engine

**What:** Five hysteresis / safety constraints making ethanol
live-tune safe. Critical for vehicle protection. Cannot ship live
tune without this.

**Constraints (per MISSION_SPEC §6.2 — moved from §4.5):**

1. **Change threshold** — Update only triggers if ethanol changes
   by ±3%. Example: at E50, update only on E≤47 or E≥53.
2. **Dwell time** — After threshold crossed, wait 60 s minimum
   before updating. Prevents oscillation if value bounces. Timer
   resets if value drops back below threshold.
3. **WOT lockout** — Live updates fully blocked during wide-open
   throttle. Ethanol frozen at current value. Prevents
   destabilization at full power.
4. **Rev limiter reduction during update** — Limiter drops to
   4000 RPM while update is in progress. Protects engine while
   maps are being rewritten.
5. **Stabilization window** — After new value lands in RAM, wait
   30 s before restoring full RPM range.

**Where it lives:** `firmware/src/ethanol_constraints/` — new module
(does not exist yet).

**Modular scope:**
- `ethanol_constraints_threshold.c` — change-detection + hysteresis
- `ethanol_constraints_dwell.c` — dwell timer state machine
- `ethanol_constraints_wot_lockout.c` — WOT detection + suppress flag
- `rev_limiter_safety.c` — RPM cap apply/restore via UDS calibration
  write (or vendor-specific safety service)
- `ethanol_constraints_config.h` — thresholds + timers as named
  defines (no magic numbers — per workspace rules)

**Configurable defaults (per workspace rule, no magic numbers in source):**
- `ETHANOL_CONSTRAINT_THRESHOLD_PCT_DEFAULT 3`
- `ETHANOL_CONSTRAINT_DWELL_SECONDS_DEFAULT 60`
- `REV_LIMITER_SAFE_RPM_DEFAULT 4000`
- `STABILIZATION_WINDOW_SECONDS_DEFAULT 30`
- All overridable via NVS or build-time config

**Closes when:** On dev RS7 with engine running, sweep ethanol input
(via BLE or manual override) across the ±3% threshold. Verify each
of the 5 constraints fires per spec timing. Wire-witness the rev
limiter clamp during the update window and the restoration after.
Worst-case observed update window ≤ 2.0 s.

---

## P3-04 🔴 Rev limiter safety RAM address (per variant)

**What:** Spec says rev limiter drops to 4000 RPM during update.
Which RAM address controls the rev limiter on `4K0907557G__0003`?
Likely `engine_speed_display` symbol at `0x9F93E` per Scorpion
`config.json` for the dev RS7, but needs A2L verification.

Per-variant — every supported boxcode needs its own address (Phase 3
manifest schema).

**Closes when:** Per-variant manifest carries verified rev-limiter
RAM addresses. P3-03 references this manifest entry instead of
hardcoding.

(Was Phase 1 decision D5; moved here with the §4.5 silo.)

---

## P3-05 🔴 SBF storage path on dongle

**What:** Confirmed `/storage/sbf/` per existing firmware path, OR
different home? Different from `/cal/profiles/` (logger profiles).

**Closes when:** P3-01 GUI uploads SBF to the path P3-02's
`live_tune_start` reads from. Both agree on the storage location.

(Was Phase 1 decision D6; moved here with the §4.2 silo.)

---

## P3-06 🔴 Ethanol BLE bridge LIVE-TUNE FEED

**What:** Companion to Phase 1's §4.4a ethanol BLE LOGGING-ONLY
path. Phase 3 wires the BLE-derived ethanol % into the live-tune
constraint engine (P3-03) so the dongle can fire ethanol-triggered
RAM updates.

**Spec:** MISSION_SPEC §4.4b (split during 2026-05-22 restructure).

**Modular scope:** depends on Phase 1's `ethanol_ble_*` module
landing first. P3-06 adds a consumer (ethanol_constraints reads
from `ethanol_source` abstraction).

**Closes when:** with a BLE ethanol sensor present + Phase 1 §4.4a
GREEN, swapping fuel (E0 → E50) triggers a live-tune update per
P3-03's constraint logic. Wire-witness the RAM write window;
verify rev limiter drop + stabilization.

---

## P3-07 🔴 Pre-apply safety gate (user-initiated apply)

**What:** MISSION_SPEC §4.5 (now §6.2 in restructured spec) covers
ETHANOL-triggered apply safety. But user-initiated apply (operator
clicks "Apply Stage 2" in the UI) has its own safety surface that
isn't spec'd yet:

- Pre-apply checks: engine state (KOEO vs idle vs WOT), DTC
  presence (refuse if check-engine?), battery voltage
  (P-51 once landed), license valid, paired, ECU patched
- Confirmation gate: dialog confirmation in UI before destructive
  RAM write
- Rollback path: if apply fails mid-write, what does the dongle do?
  (probably issue stock-equivalent RAM write to revert)

**Closes when:** spec section drafted (probably becomes MISSION_SPEC
§6.4), implementation modular scope decided, exit criteria written.

This was the "withdrawn A16" referenced in the 2026-05-22 dispatch.

---

## P3-08 🔴 9 map switch slots UI

**What:** New Phase 3 spec deliverable. The Live Tune UI tab
exposes 9 RAM slots that correspond to the slots in the patched
ECU binary (DEADBEEF-sentinel-marked per the SEFI patch pattern).

**Slot model:**
- 9 RAM slots reserved in the patched binary
- Each slot can hold one calibration variant (e.g., Stage 1, Stage 2,
  Stage 3, E0 baseline, E85 max, per-customer custom)
- Slot 0 is reserved as "passthrough" / stock-equivalent — no live
  tune active

**UI surface (Live Tune tab):**
- 9 slot buttons/cards, each showing:
  - Slot index (0-8)
  - Loaded SBF filename (or empty)
  - Stage level (1/2/3) + ethanol anchor (E0/E10/E50/E85/custom)
  - Last-applied timestamp
  - Active indicator (which slot is currently fired into RAM)
- Operator actions per slot:
  - View: open details (per-map breakdown, ethanol curve, scale
    reference)
  - Switch active: fire the slot's content via live RAM update
    (P3-02 path)
  - Upload: replace slot's SBF (file picker or drag-drop)
  - Mark "favorite" / "default boot" / "danger" (customer
    preference, persisted per-dongle in NVS)
- Slot 0 cannot be uploaded over (always passthrough/stock-equiv).

**Sentinel marker:** Spec calls for DEADBEEF (0xDEADBEEF or
equivalent) markers in the patched binary. Exact marker is TBD —
not visible in `~/esp/obd/SEFIV1.0/EXE TOOL/data/4K0907557G__0003/output/stage1_patched.stf`
text grep on 2026-05-22 audit. Likely the marker is in the
`.sbf` binary structure, not the human-readable .stf. Confirm
during P3-01 builder work by examining the SCPN-format-v4 header
+ per-segment headers.

**Modular scope:**
- `ui/control_panel.{html,js}` — Live Tune tab gains the 9-slot
  card grid
- `firmware/src/sbf/` — per-slot metadata (already partially
  exists via `sbf_variants`); add slot index + favorite flag NVS
  storage
- New WS commands:
  - `list_sbf_slots` → returns 9-element array of slot metadata
  - `switch_sbf_slot {index: N}` → fires P3-02 apply against slot N
  - `set_sbf_slot_favorite {index: N, favorite: bool}` → NVS write
- License gate: `license_can_run_feature(FEATURE_LIVE_TUNE)` per
  slot switch

**Closes when:** Operator on dev RS7 can view all 9 slots, upload
SBFs to empty slots, switch active slot, mark favorite, all
wire-witnessed via P3-02 apply path.

---

## P3-09 🔴 Live tune feature registration with feature_manager

**What:** `FEATURE_LIVE_TUNE` already exists as an enum value in
`feature_manager.h`. The orchestrator (`sbf/sbf_orchestrator.c`)
already calls `feature_manager_request_start(FEATURE_LIVE_TUNE, ...)`.
Phase 3 close gates on:
- Feature actually registers at boot (mirror P-28's wot_logger
  fix — register descriptor with feature_manager)
- License gate `license_can_run_feature(FEATURE_LIVE_TUNE)`
  blocks start when license invalid (verify enforcement)
- Active-feature display correctly shows `live_tune` in
  `get_status.active_feature`

**Closes when:** Three-stream witness for live_tune_start: WS log
shows ok + active_feature, UI gauges reflect the apply state,
wire shows the UDS RAM-write transactions.

---

## P3-10 🛑 Per-variant manifest schema lock

**What:** P-11 in `PHASE_2_PREREQUISITES.md` (originally tracked
manifest work for Phase 2 + later phases). Now relevant to Phase 3
since live tune is per-variant.

Phase 3 needs the manifest to carry:
- Logger DID table per boxcode (overlap with P-55)
- SBF storage layout per boxcode (P3-05)
- Rev limiter RAM address per boxcode (P3-04)
- Slot-count per boxcode (some ECUs may have fewer than 9 slots)
- Stage 1/2/3 calibration validity per boxcode

**Closes when:** Manifest schema reviewed + locked. Schema
versioned (mismatch detection at boot).

---

## P3-11 🛑 FUTUNER_PHASE3_ENABLED config flag

**What:** New compile-time flag mirroring `FUTUNER_PHASE2_ENABLED`.
All Phase 3 code paths (live tune apply, ethanol constraint engine,
slot switcher, rev limiter clamp) must be gated behind this flag.
Default 0. No customer device ships with `=1` until Phase 3 close.

**Status:** Added in Stage 7 of the 2026-05-22 silo restructure.

**Closes when:** flag defined in `firmware/src/config/futuner_config.h`,
build PASSes at both `=0` (default) and `=1`, all live-tune call
sites guarded.

---

## Sequencing

Phase 3 cannot start until Phase 1 is PERFECT (per
PHASE_1_COMPLETION_PLAN.md exit criteria). Phase 3 close itself
sequences:

1. **P3-01 SBF builder** — unblocks producing variant SBFs
2. **P3-02 SBF apply on RS7** — proves the apply path E2E
3. **P3-03 + P3-04 + P3-06** — ethanol constraint engine + rev
   limiter address + BLE feed (all parallel after P3-02)
4. **P3-07** — pre-apply safety gate (spec first, then implementation)
5. **P3-08 + P3-09** — slot UI + feature registration (UI-layer
   work, parallel with constraint engine)
6. **P3-10** — manifest schema lock (cross-cuts all of the above)
7. **P3-11** — flag flip to `1` gates Phase 3 enable per-device

---

## Decision points needing owner input

| ID | Question | Affects |
|----|----------|---------|
| P3-D1 | SBF builder Node-patch vs Python-port vs regenerate-canonical | P3-01 |
| P3-D2 | DEADBEEF sentinel actual format (TBD until P3-01 builder lands) | P3-08 |
| P3-D3 | Slot 0 = passthrough OR slot 0 = "stock SBF" (different semantics) | P3-08 |
| P3-D4 | Pre-apply safety gate exact spec — engine state, DTC presence, voltage, etc. | P3-07 |
| P3-D5 | Manifest schema versioning approach (semver vs hash vs schema doc) | P3-10 |
| P3-D6 | Subscription tier model — does Phase 3 require its own subscription separate from Phase 1 license? | P3 GA |

---

## Phase 3 EXIT criteria — binary checklist

When every line below reads `🟢`, Phase 3 is PERFECT and customer
Phase 3 firmware (with `FUTUNER_PHASE3_ENABLED=1`) is shippable.

- [ ] All Phase 1 P-items GREEN (Phase 1 close)
- [ ] P3-01 SBF builder produces byte-identical-to-canonical SBF
- [ ] P3-02 SBF apply E2E on dev RS7 (RAM writes wire-witnessed)
- [ ] P3-03 ethanol constraint engine — all 5 constraints fire per spec timing
- [ ] P3-04 rev-limiter RAM address validated per variant (manifest)
- [ ] P3-05 SBF storage path confirmed + GUI ↔ firmware agree
- [ ] P3-06 BLE ethanol → live tune apply on RS7 (with sensor)
- [ ] P3-07 pre-apply safety gate spec'd + implemented + exercised
- [ ] P3-08 9-slot UI: view + upload + switch + favorite on RS7
- [ ] P3-09 FEATURE_LIVE_TUNE registers + license-gates correctly
- [ ] P3-10 per-variant manifest schema locked
- [ ] `FUTUNER_PHASE3_ENABLED` can flip to `1` per-device under
      Sean's explicit per-variant sign-off (no auto-enable)

---

## Update protocol

When an item closes, change its emoji to 🟢 and add a one-line
note in today's `status-YYYY-MM-DD.md`. New Phase 3 items append
as the next `P3-NN`. Numbers are sticky.

When all P3-items are 🟢, Phase 3 is unblocked from a
prerequisites standpoint. Even then: no Phase 3 firmware ships to a
customer device without Sean's explicit per-variant sign-off.

---

## Owner directive log

### 2026-05-22 — Phase 3 silo established

> **Sean / SRM Engineering.** Phase 3 (live tuning ecosystem) gets
> its own phase mirroring how Phase 2 silos the destructive flash.

Phase 1 deferrals from 2026-05-21 (SBF live cal switching, ethanol
constraints + rev limiter, SBF builder) move to Phase 3 instead of
"DEFERRED-with-a-later-phase-TBD."

What stays in Phase 1:
- Non-destructive customer features per `PHASE_1_COMPLETION_PLAN.md`
- §4.4a ethanol BLE LOGGING-ONLY (the logged-variable path)

What is now Phase 3:
- §6.1 SBF live cal switching (RAM-write apply path)
- §6.1 SBF/STF/FBF builder tool
- §6.2 ethanol constraint engine + rev limiter
- §6.3 9 map switch slots UI (new spec deliverable)
- §6.4 pre-apply safety gate (new spec deliverable)
- §4.4b ethanol BLE live-tune feed (separate from §4.4a)

`FUTUNER_PHASE3_ENABLED=0` is the default. Customer cars stay on
Phase 1 until Phase 3 close is signed off.

Phase 1 close STILL gates Phase 3. Phase 3 builds on Phase 1's
license / pair / logger / cloud surfaces.
