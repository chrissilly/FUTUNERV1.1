# FUTUNER — Scale Architecture Proposal
## Supporting 100+ ECU Software Versions with Server-Backed Licensing, Telemetry, and Calibration Delivery

> Status: draft v0.1 — 2026-05-05
> Author: Chris (BIG Daddy) / SRM Engineering
> Audience: Sean Cyr, SRM eng team
> Supersedes: nothing yet — this is the first scaling-focused doc on top of MISSION_SPEC.md
>
> **Reading order**: §1 frames the problem. §2 defines the canonical identity model that everything else hangs off. §3–§7 cover server, dongle, calibration delivery, licensing, and Phase 2 binary handling. §8 covers test/QA at scale (this is where most products fail). §9 is the operational rollout plan.

---

## 1. Problem framing

### 1.1 What "100 different software versions" actually means

A "software version" in FUTUNER terms is an ECU **variant** — uniquely identified by the combination of:

- **Boxcode** (e.g. `4K0907557G`)
- **Software number** (e.g. `__0003`)
- **ECU family** (MG1CS002 vs. MG1CS008 vs. MD1CP004 etc.)
- **Endianness** + **memory map signature** (write_mid_byte, write_offset, calibration block locations)

Two variants with the same boxcode but different SW numbers can have different RAM addresses for the same ECU variable. That means **a wrong address write at scale = a fleet-wide brick risk**, which is the single most important constraint shaping this design.

The current FUTV1.1 boxcode_database.md captures 36 variants with 4 fully supported. Going to 100 means roughly tripling the coverage — but more importantly, the *operational* surface (per-variant test, validation, support, and subscription enforcement) has to grow with it without becoming linear in Sean's calendar.

### 1.2 The cross product that actually scales

The thing that's hard at 100 variants isn't 100 — it's the cross product:

```
   N ECU variants
 × M dongle firmware builds
 × K calibration stages (1, 2, 3, custom)
 × J ethanol blends per stage (E0, E10, E50, E85, …)
 × L customers per variant
 × 2 license states (lifetime / recurring active)
```

That product has to be testable, deliverable, billable, and revertible — and it has to fail closed when any dimension is unknown.

### 1.3 Design principles

These come directly from the user-stated preferences and the mission spec, restated for this doc:

1. **Fail closed.** Any unknown variant, missing license, mismatched signature, or out-of-bounds write request results in *no action*. The dongle's default state is inert.
2. **Modular and replaceable.** Variant data, calibration packs, license logic, and transport layer are all behind clean interfaces so a single piece can be swapped without rebuilding firmware.
3. **No magic numbers in code.** Every constant — thresholds, timeouts, addresses, key references — lives in a versioned configuration the dongle pulls from server (or the variant manifest baked into firmware), not as an integer literal in a `.c` file.
4. **One source of truth per fact.** ECU memory layout lives in *one* manifest. Subscription state lives in *one* table. Firmware-to-variant compatibility lives in *one* matrix. Duplication is forbidden because it's the #1 way 100-variant products go wrong.
5. **Phase 1 and Phase 2 are independently deployable and revocable.** Live tune (Phase 1) can be revoked without de-flashing the base binary. Phase 2 can be rolled out variant-by-variant without touching Phase 1 of unaffected variants.

---

## 2. Canonical ECU variant identity

The single most important artifact in this design is the **variant manifest**. Every other system (firmware, server, billing, support tooling) consumes it. Get this right and everything else follows.

### 2.1 Variant identifier

Define a stable string ID that uniquely identifies a variant. Proposal:

```
ecu_variant_id = sha1(
    boxcode || "/" ||
    sw_number || "/" ||
    ecu_family || "/" ||
    endian_marker
)[:12]    # 12 hex chars, e.g. "a3f9b21e7c04"
```

Why a hash and not just the boxcode? Because the dongle has to detect the variant from CAN at runtime. The hash is computed by the dongle from values it queries from the ECU (boxcode via UDS read-by-ID, SW number via the same, endianness inferred from a known marker word) — and that hash directly indexes the variant manifest. No fuzzy matching, no string parsing of vendor identifiers at runtime.

The 12-hex-char form is also short enough to log, paste into support tickets, and use as a filename component.

### 2.2 Variant manifest schema

One JSON file per variant, stored canonically in the cloud's variant catalog and shipped to dongles as part of their startup bundle. Schema (illustrative — actual field set should be reviewed before locking):

```jsonc
{
  "schema_version": 1,
  "ecu_variant_id": "a3f9b21e7c04",
  "boxcode": "4K0907557G",
  "sw_number": "0003",
  "ecu_family": "MG1CS002IFX",
  "vehicle_examples": ["Audi RS7 C8 4.0L V8 TFSI"],
  "endianness": "little",
  "memory_map": {
    "write_mid_byte": "0x80",
    "write_offset":   "0x000000",
    "calibration_region": { "start": "0x...", "end": "0x..." }
  },
  "addresses": {
    "ethanol":       "0x11E6AE",
    "speed_display": "0x09F93E",
    "vin_field":     "0x..."
  },
  "logger_variables": [
    { "name": "rpm",     "addr": "0x...", "type": "u16", "scale": 1.0 },
    { "name": "load",    "addr": "0x...", "type": "u16", "scale": 0.1 }
    // ...
  ],
  "writable_regions": [
    { "name": "ign_timing_main",  "start": "0x...", "end": "0x..." },
    { "name": "load_target",      "start": "0x...", "end": "0x..." }
  ],
  "aes_key_ref":         "MG1CS002",
  "phase2_base_binary":  "binaries/4K0907557G__0003__base_v3.bin.signed",
  "capabilities": {
    "live_tune": true,
    "phase2_flash": true,
    "ble_ethanol": true
  },
  "tested_firmware_min": "1.4.0",
  "released_at":         "2026-05-12T00:00:00Z",
  "release_notes_url":   "/docs/variants/a3f9b21e7c04.md"
}
```

Three things matter here:

1. **`writable_regions` is the safety boundary for end-user SBFs.** The SBF builder, the server validator, and the dongle all reject any SBF that asks to write outside these regions. This is non-negotiable when customers build their own tunes.
2. **`aes_key_ref` is a string handle, not the key itself.** Keys live in a separate, narrowly-distributed key store (firmware secrets partition or HSM-backed cloud secret). The variant manifest is freely distributable; the keys never are.
3. **`tested_firmware_min`** lets the cloud refuse to deliver a variant manifest to a dongle on too-old firmware. Compatibility enforcement starts in the catalog, not in the firmware update path.

### 2.3 Authoring & validation pipeline for variants

A new variant entering the catalog goes through:

1. **Capture**: dongle in dev mode dumps boxcode, SW number, marker word, calibration block boundaries from a real car or bench ECU.
2. **Augment**: tuner imports XDF / A2L, maps RAM addresses for the variable list and writable regions.
3. **Lint**: server-side validator runs schema check, address sanity check (no overlap with write_mid_byte block), AES key reference exists, all logger variables fit inside calibration region.
4. **Bench test**: variant is exercised against a recorded ISO-TP responder for that ECU dump; logger sequence, ethanol read, and a no-op live-tune cycle must succeed.
5. **Release**: variant flagged `released_at`. Until that timestamp is set, it's invisible to the production catalog endpoint.

The point: getting from "we have a BIN dump" to "supported variant" is a checklist, not Sean staring at Ghidra alone for a weekend per variant.

---

## 3. Cloud server architecture

### 3.1 Why the current SQLite single-file design needs to evolve

**Not for throughput reasons.** Honest math: at 30K dongles each phoning home on boot plus a daily heartbeat, sustained request rate is well under 1 req/sec. Phase 2 8 MB binary downloads happen once per VIN, lifetime. WOT logs are 3–4 KB gzipped, on demand. Even at 100K dongles the workload sits inside what a single FastAPI process on a small VPS handles without breathing hard. The existing SQLite-backed server is throughput-fine through the foreseeable product lifetime.

The reason to evolve is **schema complexity**, not load. Once you have:

- Variants × stages × ethanol blends × firmware builds
- Customer accounts × VINs × subscriptions × dongles
- License events × audit log × support tickets × telemetry uploads

you want foreign keys, transactions, schema migrations (Alembic), and a real query planner. That's the move from SQLite to PostgreSQL — driven by complexity, not load.

### 3.2 Proposed service decomposition (logical, not necessarily separate processes)

Within one FastAPI app, separate modules with clean boundaries. We can split them across processes later if any single one becomes hot. Modules:

**a. Identity & licensing.** Customer accounts, VIN ownership, subscription state, dongle enrollment, token management. The single source of truth for "is this dongle allowed to do thing X right now?".

**b. Variant catalog.** Variant manifests, signed base binaries, AES key references (not keys), firmware-to-variant compatibility matrix. Read-heavy, infrequently written. Cacheable behind a CDN / edge cache.

**c. Calibration delivery.** SBF library: SRM presets + per-customer custom builds. Implements the existing per-VIN DRM patch (ethanol_random injection). Validates SBF against the target variant's `writable_regions` before delivery.

**d. Telemetry / log ingest.** WOT logs, register events, fault code reports, license events. Append-only. Eventually retention-policied by customer subscription tier.

**e. Admin & support.** Dashboard for Sean and support staff: device state, license state, telemetry health, variant rollout status, manual overrides.

**f. Tuner builder backend.** API for the SBF builder web tool — read XDFs, validate proposed SBFs, save to per-customer SBF library. Gate-kept by writable_regions in the variant manifest.

### 3.3 Storage

| Store | What lives here | Why |
|-------|-----------------|-----|
| PostgreSQL | All relational data: devices, customers, VINs, subscriptions, variant catalog metadata, audit log, support tickets | ACID, FKs, migrations, real query planner. Single instance is enough until ~100K devices. |
| Object storage (S3-compatible) | Firmware binaries, Phase 2 base binaries (signed), SBF files, telemetry log uploads | Binary blobs don't belong in a relational DB. Signed URLs for delivery. |
| Secret store (HSM-backed if budget allows; otherwise sealed at rest) | AES keys per ECU family, signing keys, admin API keys, billing-provider keys | Narrow access, audit-logged, never logged or exposed to the variant manifest endpoint. |

### 3.4 API surface (additions over what's already there)

Keep all existing endpoints from `cloud/src/main.py`. Add:

```
# Variant catalog
GET  /api/v1/variants                      → list released variants (low-detail, public-ish)
GET  /api/v1/variants/{ecu_variant_id}     → full manifest, gated by device token

# Licensing
GET  /api/v1/license                       → current entitlements for this dongle
POST /api/v1/license/refresh               → force re-check (after billing change)
POST /api/v1/license/heartbeat             → periodic check-in for offline grace tracking

# Phase 2 base binary
GET  /api/v1/phase2/base                   → signed URL to this variant's base binary

# Tuner builder
GET  /api/v1/builder/xdf/{ecu_variant_id}  → XDF for builder UI
POST /api/v1/builder/validate              → server-validates a proposed SBF, returns OK or violation list
POST /api/v1/builder/save                  → saves a validated SBF to customer's library

# Telemetry
POST /api/v1/telemetry/log                 → upload gzipped log (≤ ~4KB per spec §1.3)
POST /api/v1/telemetry/event               → typed event (license_check, ethanol_transition, fault_clear, etc.)
```

All device-facing endpoints stay token-gated as already implemented.

---

## 4. Dongle firmware architecture for variant scaling

### 4.1 Variant detection and binding

On boot:

1. CAN init, ISO-TP up.
2. Read boxcode + SW number via UDS (already implemented).
3. Compute `ecu_variant_id` locally.
4. Look up variant manifest in flash (last-known-good cached copy from previous boot) — if present and matches detected ID, proceed. If not present, request from server on next Wi-Fi connection.
5. If no variant manifest available (first boot, no Wi-Fi yet), enter **safe idle**: log gauges allowed (read-only UDS), no live-tune writes, no flash writes. UI surfaces "awaiting variant manifest from server".

The dongle never falls back to "best guess" addresses across variants. That's the brick path.

### 4.2 Modular firmware structure (already largely in place in FUTV1.1/firmware/src/)

The existing module split is good and should be enforced going forward:

```
src/
├── main.c                    # boot, top-level state machine
├── state_machine/            # explicit ON/OFF gating per project rule (no implicit running)
├── can/                      # raw CAN driver
├── isotp_coordinator/        # ISO-TP segmentation
├── transport/  (NEW)         # abstract send_frame/recv_frame interface, CAN + Ethernet impls
├── uds/                      # UDS service layer over transport
├── variant/    (NEW)         # variant manifest parsing, ID computation, lookup cache
├── scal/, bdef/, ecu_write/  # live tune RAM update path (already proven in v1.0)
├── flash/                    # Phase 2 flash writer (UDS 0x27/0x34/0x36/0x37)
├── flex_fuel/                # ethanol constraint logic, hysteresis, dwell, rev-limit window
├── logger/                   # WOT log capture (≤60s, gzipped)
├── commands/                 # ws/serial command dispatch (start/stop logging, start tune, etc.)
├── websocket/                # browser UI streaming
├── wifi/                     # AP→STA pairing, hotspot join
├── nvs/                      # device token, license cache, last-known variant manifest
├── ota/                      # firmware update (dual-bank already present)
└── error/                    # central fault recovery + safe-idle entry
```

### 4.3 The ON/OFF discipline (per project instructions)

Every feature module exposes `start()` / `stop()` and reports its run state into a central registry. The state machine enforces:

- Only one "active" feature at a time (logging, live-tune, Phase 2 flash, etc.).
- Attempting to start a second active feature when one is running → warn user, cleanly stop running feature, *then* start requested one.
- All features default to inactive at boot. No silent background telemetry. Every action is user-initiated via web UI or serial.

This is a project-wide rule and the proposal preserves it.

### 4.4 Configuration over constants

Per the user-stated rule against magic numbers, the dongle reads its constants from a layered config:

1. Variant manifest (variant-specific addresses, writable regions, capability flags).
2. Per-device config blob (license cache, subscription state, sensor pairing IDs).
3. Per-firmware-build defaults (ethanol hysteresis %, dwell time, rev limit during update window, log size limit, gauge stream rate).

All numeric thresholds in §1.5 of MISSION_SPEC (the ethanol hysteresis, dwell, WOT lockout, rev-limit window, stabilization window) become config keys. None are integer literals in source. Default values are proposed in `firmware/config/defaults.json` and require explicit approval to change.

---

## 5. Calibration delivery — the SBF system at scale

### 5.1 SBF library structure on the server

Two kinds of SBFs coexist:

- **SRM presets**: built and signed by SRM. Trusted. Customers see them in a curated catalog.
- **Customer custom SBFs**: built via the builder tool. Validated server-side against the target variant's `writable_regions`. *Not* signed by SRM — UI clearly marks them as "your custom tune".

Both kinds get the per-VIN ethanol_random patch at delivery time (existing mechanism in `patch_sbf_for_device`).

### 5.2 SBF builder tool — the trust boundary

**Important update**: per Sean, the SBF builder is hosted on the SRM website, not distributed to customers. That collapses most of the threat surface I had originally written for. Mitigations therefore reduce to:

1. **Whitelisted addresses only — enforced in the builder UI itself.** The user never sees raw addresses or raw SBF JSON. The UI exposes map values bounded by the variant's writable_regions and clamps to safe ranges defined in the variant manifest. Out-of-bounds is not a value the customer can express.
2. **Origin signing.** Every SBF the builder emits is signed with an SRM key. The dongle refuses to load any SBF whose signature doesn't verify against SRM's public key embedded in firmware. Customer-built SBFs from the official builder verify; any hand-crafted JSON from a forum does not.
3. **Hard safety invariants live in firmware, not SBF.** Rev limit absolute max, max ignition advance, min AFR floor — firmware-enforced and override anything in an SBF. The customer cannot disable them. (Defense in depth even when the SBF is trusted.)
4. **Telemetry on every custom SBF activation** so SRM can audit what tunes are out there and pull a problematic builder preset if needed.

The validation gate runs in the builder code path on submit, not at delivery time. Delivery just streams the already-validated, already-signed SBF and adds the per-VIN ethanol_random patch.

### 5.3 Delivery flow

```
Browser builder UI ─ POST /api/v1/builder/save ─→ Server validates → store
                                                                       │
Dongle ─ GET /api/v1/calibration ─→ Server pulls customer's active SBF
                                  → Patches ethanol_random for this VIN
                                  → Streams patched SBF to dongle
Dongle ─ stores SBF, awaits explicit "apply" from web UI ─→ live-tune RAM update
```

No SBF is ever auto-applied. The dongle holds it pending an explicit user action per the project's ON/OFF discipline.

---

## 6. Licensing — VIN lifetime only (v1)

**Decision (2026-05-05):** simplify to a single VIN-lifetime entitlement. No recurring subscription in v1. The data model leaves a hook for future tiering but ships nothing for it.

### 6.1 Single flag per VIN

- `paid` (one-time, lifetime per VIN): when true, the dongle is licensed for everything FUTUNER offers on this VIN — Phase 2 base binary install, live-tune SBF delivery, telemetry, gauges, fault clear. When false, the dongle is in tethered idle: it can read gauges and fault codes (read-only diagnostics), and that's it. No live tune, no flash.

That's the entire model.

### 6.2 Server-side state

```
customer
  ├── account_id, name, email
  └── vins[]
        ├── vin
        ├── ecu_variant_id
        ├── dongle_mac
        ├── paid: bool, paid_at: ts
        ├── phase2_flashed_at: ts | null   ← informational, not a separate gate
        ├── revoked: bool, revoked_reason: string
        └── tier: string = "lifetime_v1"   ← reserved field for future tiering
```

`tier` exists today as a string column with the single value `"lifetime_v1"` so that if SRM later wants to introduce, e.g., a "pro live-tune" tier or a "track day pack," it's a server-side change and a database value change. The dongle reads tier from the license token but ignores anything other than "is paid true." That keeps firmware unchanged when the product evolves.

### 6.3 Enforcement on the dongle

The dongle pulls a license token on first online sync and caches it persistently. The token is signed by SRM and contains: VIN, ecu_variant_id, paid, tier, revoked. No expiry. Once paid is true, that token is good forever from the dongle's perspective unless explicitly revoked.

Why no expiry? Because we're not gating recurring revenue. A lifetime entitlement that requires the customer to phone home every 24h to keep working is hostile UX — customers store cars over winter, drive in dead zones, take trips. Forever-cached, refreshed-on-availability is the right shape.

### 6.4 Revocation flow

The only reasons to revoke a paid VIN are: fraud (chargeback), safety (a customer's specific car is implicated in an incident and we want to roll them back), or theft (dongle reported stolen and tied to a different VIN). All of these are admin-driven, not customer-initiated.

1. Admin sets `revoked = true` and provides a reason in DB.
2. Next time the dongle has Wi-Fi and re-syncs the license, it sees the revoke flag.
3. Dongle clears any active SBF from RAM, surfaces a revocation UI message with the reason, refuses further live-tune commands.
4. Phase 2 base binary is *not* automatically reverted — that needs a physical recovery flash. The dongle simply refuses any further Phase 1 changes, which is the safe minimum.

Because the dongle is forever-cached, revocation is not instantaneous — it's eventually-consistent on next Wi-Fi sync. That's acceptable for the failure modes revocation is meant to address (none of them are "stop within 5 minutes"). If a future scenario demands faster revocation, we can add a low-rate heartbeat then.

### 6.5 What this simplification removes from v1 scope

- Stripe (or any) recurring billing integration
- Subscription period tracking, renewal, dunning
- Offline grace window logic (no expiry to grace)
- Two-flag enforcement in firmware
- Subscription "lapsed → unload SBF" UX
- Period-end notifications
- Failed-payment retry policy

All of these are *deferrable*, not lost. The `tier` reserved field is the only piece of forward-compat scaffolding.

---

## 7. Phase 2 base binary — distribution at scale

### 7.1 Per-variant binary inventory

Given Sean's design (one patched base binary per ECU variant: Stage 1 power + logging + live-tune hooks), the inventory is:

```
~100 variants × ~8 MB = ~800 MB total
```

That's small. Object storage with signed URLs. A few hundred dollars a year of bandwidth for a typical aftermarket niche product.

### 7.2 Per-binary integrity

Every binary is:

1. **Signed by SRM** — separate signing key from the device tokens. Firmware verifies signature before flash.
2. **Hashed (SHA-256)** — server returns the expected hash to the dongle on the same response that returns the signed URL. Dongle verifies after download, before flash.
3. **Versioned per variant** — `4K0907557G__0003__base_v3.bin`. Bumping the version per variant is independent of bumping firmware version.

### 7.3 Pre-flash safety gate

Per MISSION_SPEC §5.1, Phase 2 is dangerous. Proposed pre-flash gate (all must pass before dongle begins flash):

- Variant ID matches the binary's intended variant
- Battery voltage above configured floor (proposed, configurable: 12.4V — needs approval)
- Ignition state ON, engine OFF
- ECU responding to UDS within configured timeout
- Customer license shows `phase2_flashed = false` on this VIN (i.e. not already done) OR explicit re-flash command from admin
- Customer has explicitly confirmed in UI within last N seconds (proposed 60s — needs approval)

Any miss → abort with clear UI message. No partial flashes.

### 7.4 Recovery posture

Maintain a separate, minimal **recovery binary** per ECU family (not per variant) that the dongle can attempt to apply if a flash fails mid-write. This is the fault recovery playbook from §5.1 of MISSION_SPEC — needs a dedicated design doc, but the catalog needs to know about it from day one.

---

## 7a. Telemetry / WOT log retention — bounded server storage

**Decision (2026-05-05):** simple per-VIN byte quota with FIFO eviction. No starring, no TTL, no eviction notices.

### 7a.1 The policy

| Setting | Default | Behavior |
|---------|---------|----------|
| `LOG_QUOTA_BYTES_PER_VIN` | 10 MB per VIN | When a new log upload would push the VIN over quota, the oldest log(s) on the server are deleted until the new log fits. FIFO. |

That's the entire policy. One configurable number. The default is 10 MB, configurable per the no-magic-numbers rule; needs explicit approval before lock.

### 7a.2 What 10 MB buys in practice

At ~4 KB per gzipped WOT log (per MISSION_SPEC §1.3), 10 MB holds roughly 2,500 logs — somewhere on the order of 20+ hours of cumulative WOT pull data. For a typical enthusiast pulling 100–500 WOTs/year, that is effectively lifetime retention. Only hardcore track-day customers will ever hit the cap, and FIFO eviction is the correct answer for them.

### 7a.3 Total server storage bound

```
ceiling = LOG_QUOTA_BYTES_PER_VIN × active_vin_count
       = 10 MB × 30K  ≈ 300 GB
       = 10 MB × 100K ≈ 1 TB
```

Both are cheap on object storage and the size is bounded by configuration, not by usage. No surprise scaling.

### 7a.4 Dongle-side behavior

The dongle queues logs locally when offline (existing behavior in MISSION_SPEC §1.3). Once the cloud confirms receipt, local copy is deleted. The server handles its own quota; dongle does not enforce server-side limits.

If the server rejects an upload (revoked VIN, server in maintenance, etc.), the dongle keeps the log locally and surfaces a UI warning so the customer can pull it to their phone/laptop via the dongle's web UI.

### 7a.5 What's *not* logged long-term on the server

Aggregate statistics for variant health (ethanol-transition counts, fault code frequencies, live-tune update success rates) are extracted at ingest into anonymized rolling counters that drive the §8.4 variant-coverage dashboard. Once those counters are updated, the underlying log file is just the customer's own data — subject only to the 10 MB FIFO. Aggregate variant health survives even after individual logs evict.

---

## 8. Test strategy at 100 variants

This section is where most products of this type fail. Manual per-variant testing does not scale.

### 8.1 Bench fleet vs. simulated bench

Two-tier:

**a. Real bench rigs** for a small subset (~5–10 variants spanning every ECU family). Hardware-in-the-loop, real ECU on a workbench. Used for high-confidence pre-release validation.

**b. Simulated bench** for the long tail (~90 variants). Built from a recorded BIN dump per variant + an ISO-TP responder running on a Linux box. The dongle talks to it as if it were a real ECU. Catches address-mismatch and protocol regressions; *cannot* catch real-engine misbehavior, which is fine because we're not flashing engines in CI.

### 8.2 Per-PR regression

Every firmware PR runs against:

- All simulated benches (fast: minutes).
- A nightly full real-bench suite (slow: hours).
- A canary on the dev car for any change that touches live-tune or Phase 2 code paths.

### 8.3 Fleet canary in production

Per-variant rollout of new firmware:

1. New firmware tagged "canary".
2. Assigned to 1% of devices on each variant.
3. Telemetry ingest watches: license check failures, fault code spikes, log upload failures, live-tune RAM update latency outliers (>2.5s = regression on the spec's 1.5–2s budget).
4. Auto-promote to 10% → 50% → 100% on green metrics. Auto-rollback on red.
5. Sean has a manual override ("hold rollout for variant X").

This is the *only* sustainable way to ship firmware to 100 variants without a 100-day regression cycle.

### 8.4 The variant-coverage dashboard

A single page showing, per variant: last known-good firmware build, last test result on simulated bench, last real-bench result, current rollout %, count of active devices, count of devices on stale firmware. Sean's daily glance.

---

## 9. Rollout plan — getting from FUTV1.1 today to this design

This is intentionally not big-bang. The current FUTV1.1 cloud server + firmware is already running and has a real customer (Sean's dev car). The goal is to evolve in place.

### Phase A — Foundations (next 2–3 weeks)

A1. Define and lock the variant manifest schema. Convert the existing 4 fully-supported boxcodes into manifests as the format check.
A2. Migrate the cloud DB from SQLite to PostgreSQL with a real schema. Keep the existing endpoint contracts so the dongle doesn't have to change yet.
A3. Set up object storage for firmware + binaries + SBFs. Cut over delivery from local FS to signed URLs.
A4. Stand up a secret store for AES keys; remove keys from any in-repo location and reference by `aes_key_ref`.

### Phase B — Variant catalog & SBF builder (2–4 weeks)

B1. Variant catalog endpoints + admin UI for variant manifests.
B2. SBF builder tool (web) with server-side validation. Internal-only first.
B3. Convert existing SRM presets into the new SBF schema.
B4. Telemetry ingest (gzipped log upload + typed events).

### Phase C — Licensing (1–2 weeks, parallelizable with B)

C1. Customer accounts + VIN ownership in DB.
C2. One-time payment flow (any processor — Stripe Checkout, Square, even an invoiced bank transfer for the boutique start). One-time payments are vastly simpler than recurring; pick whichever has the cleanest API.
C3. License token issue / revoke flows. No refresh — tokens are forever-cached.
C4. Dongle license cache (persistent in NVS) + revoke handling on Wi-Fi sync.

### Phase D — Variant scale-out (continuous)

D1. Bring online the next 10 highest-priority variants per the boxcode_database "Next Steps" section (8W0907559G__0008 first per the spec — A2L + bin available).
D2. Build out simulated bench for each new variant.
D3. Rollout per Phase 8 plan above.

### Phase E — Phase 2 at scale (after Phase 1 fully proven)

E1. Per-variant base binary build pipeline.
E2. Recovery binary per ECU family.
E3. Pre-flash safety gate hardening.
E4. Real-bench Phase 2 validation per variant before any customer device gets it.

### Phase F — Operations

F1. Admin dashboard (variant coverage, fleet health, license state).
F2. Support portal / ticket integration (the customer-support plugin available in this workspace).
F3. On-call playbook + incident runbook.

---

## 10. Open questions and explicit asks

These need sign-off from Sean before implementation begins on the relevant section:

1. **Population per variant** — *not* needed to size infrastructure (the math says single-VPS FastAPI handles 30K+ dongles fine). Needed to prioritize per-variant test investment, support staffing, and risk budget. Variants with 500 customers deserve more bench time than variants with 5.
2. **Payment processor for one-time VIN-lifetime purchase** — Stripe Checkout is the easy default. Square, PayPal, or invoice-only for the boutique start are all viable. Whichever is cleanest, since we're not building recurring billing.
3. **Key custody** — HSM-backed cloud secret store, encrypted-at-rest in cloud DB, or split between cloud (for delivery-time use) and dongle secrets partition (for on-dongle UDS auth)?
4. ~~Offline grace window for live-tune token~~ — *removed.* Lifetime tokens don't expire; no grace logic needed.
5. **Pre-flash voltage floor and confirmation window** — proposed 12.4V and 60s respectively. Need approval.
6. **Ethanol constraint thresholds** — already in MISSION_SPEC §4.5 (±3%, 60s dwell, 4000 RPM rev-limit during update, 30s stabilization). Confirming these become server-pushable config rather than firmware constants.
7. **Should customer custom SBFs be share-able** between customers (with consent), or strictly per-account? Affects builder UX.
8. **WOT log retention** — settled per §7a. 10 MB per VIN, FIFO eviction, single configurable number. Total server bound: ~300 GB at 30K VINs, ~1 TB at 100K VINs.

---

## 11. What this proposal explicitly does NOT cover

- Detailed UI/UX design for the SBF builder web tool.
- Exact firmware code-level changes to plumb the variant manifest through `scal/`, `bdef/`, `ecu_write/`. (Will follow once the manifest schema is locked.)
- Phase 2 challenge-response algorithm specifics (lives in `secrets/` and the existing reverse-engineering docs; not appropriate for this architecture-level doc).
- Marketing / pricing strategy — that's Sean's call, not engineering's.

---

## 12. Summary

The path from "36 variants, 4 fully supported, single SQLite cloud" to "100+ variants, end-user buildable tunes, recurring subscription, server-backed licensing" doesn't require a re-architecture of the firmware (it's already modular) or a re-architecture of the cloud transport (FastAPI is fine). What it requires is:

1. A canonical, versioned **variant manifest** as the single source of truth for per-ECU memory layout, capabilities, and writable regions — driving the dongle, the SBF builder, the validator, and the catalog.
2. A real **PostgreSQL** schema behind the existing FastAPI app to handle the relational complexity of variants × subscriptions × telemetry.
3. A simple **single-flag VIN-lifetime licensing model** (`paid` per VIN, forever-cached on dongle, server-side revoke for fraud/safety only). Reserved `tier` field leaves the door open to add tiered offerings later without firmware changes.
4. Strict **server-side validation of customer-built SBFs** against the variant's writable regions — the only safe way to do end-user buildable.
5. A **fleet canary** rollout system so 100 variants × N firmware builds doesn't become 100×N regression weekends.
6. **Operational tooling** (admin dashboard, support portal, on-call runbook) without which 100 variants becomes 100 separate support nightmares.

Everything else is execution along this skeleton.

---

*Reviewers: please flag anything in §10 that needs to be locked before Phase A starts.*
