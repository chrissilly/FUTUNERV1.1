# FUTUNER Dongle — Product Specification

**Vendor:** Silly Rabbit Motorsport
**Owner:** Sean Cyr — sean@sillyrabbitmotorsport.com
**Document version:** 1.0

---

## 1. Executive Summary

FUTUNER is a subscription-gated aftermarket ECU tuning dongle (ESP32-S3 hardware) that enables real-time calibration switching, live ethanol blending, and full binary reflashing on Bosch MG1 / MDG1 / MED17 ECUs via UDS/ISO-TP over CAN (with Ethernet support coming).

The product monetizes through per-flash subscriptions and live-tune access, with VIN-locked licensing to prevent device sharing.

---

## 2. Core Technical Assets

| Asset | Status |
|-------|--------|
| AES-128 encryption keys for 8MB binary reflash | On file |
| Bosch Bash PDFs and technical whitepapers | On file |
| XDF calibration map files for tune profile generation | On file |
| Proven UDS/ISO-TP challenge-response authentication & mirroring | Validated |
| Fault recovery logic to prevent ECU bricking during flash failures | Designed |
| ESP32-S3 firmware with CAN support | Working |
| Ethernet hardware | Arriving soon |
| Development vehicle with patched binary (logging + live-tuning patches) | Live |

---

## 3. Architecture Overview

```
┌──────────────────┐  Wi-Fi   ┌─────────┐
│ Customer phone/  │ ◄─────► │ FUTUNER │ ──CAN/Ethernet──► ECU
│ laptop (browser) │  WS     │ Dongle  │
└──────────────────┘         └────┬────┘
                                  │ HTTPS
                                  ▼
                           ┌──────────────┐
                           │ SRM Cloud    │
                           │ (licensing,  │
                           │  SBFs, logs) │
                           └──────────────┘
```

**Hardware:** ESP32-S3 + CAN transceiver (Ethernet PHY coming).
**Comms:** UDS/ISO-TP application layer over transport-agnostic interface.
**UI:** Browser-based, WebSocket streaming from dongle. No native app required.

---

## 4. Phase 1 — Safe Feature Rollout (Non-Destructive)

All Phase 1 features deploy and validate **before** touching full binary flash. Phase 1 is RAM-only — no ECU flash writes.

### 4.1 VIN Pairing and Licensing

**Purpose:** bind a dongle to a single VIN to prevent device sharing.

**Customer workflow:**
1. Customer plugs dongle into OBD-II port.
2. Dongle boots in Wi-Fi Access Point (AP) mode.
3. Customer connects to dongle's AP, enters home/phone hotspot SSID + password.
4. Dongle switches to Station (STA) mode, joins customer's network.
5. Dongle reads VIN from ECU via OBD (PID `0x0902`).
6. Dongle phones home to SRM server, receives VIN-locked licensing token.
7. Token permanently binds dongle behavior to that VIN.

**Server-side:**
- Creates DB entry: `VIN + ECU software version`.
- Returns unique token tied to VIN.
- Token prevents dongle use on other vehicles (enforced by firmware).

**Notes:** Dongle hardware itself is not locked — only the software behavior. A returned dongle can be re-licensed to a different VIN by SRM.

---

### 4.2 SBF/FBF File System and Live Calibration Switching — MOVED TO PHASE 3

> **🚚 Moved to Phase 3 §6.1 (2026-05-22).** Live RAM calibration switching
> + the SBF/STF/FBF builder tool are siloed in Phase 3 mirroring how
> Phase 2 silos the destructive full-binary flash. See `MISSION_SPEC.md`
> §6.1 for the full deliverable, and `docs/PHASE_3_PREREQUISITES.md`
> for prereqs and P3-NN tracking. The SBF parser + scal/bdef RAM-write
> path + SBF orchestrator firmware code stays in the tree under the
> new `FUTUNER_PHASE3_ENABLED` compile flag (default 0).

---

### 4.3 Live Gauge Display and Data Logging

**UI architecture:**
- Dongle runs WebSocket server.
- Streams real-time ECU parameters to browser (phone or laptop).
- Browser is display-only; dongle does all heavy lifting.

**Live gauge parameters:**
- Boost pressure
- Air-fuel ratio (lambda)
- Ignition timing
- Engine temps (coolant, intake air)
- RPM
- Throttle position
- Load
- Any UDS-readable ECU parameter

**Logging:**
- Auto-logs all gauge parameters during wide-open throttle pulls.
- Maximum log duration: 60 seconds (hard limit).
- Log size: ~3–4 KB gzipped.
- Session ends at 60s or on user stop; new log begins.

**Cloud sync:**
1. Logs compress locally (gzip) on dongle.
2. Logs upload to cloud when network available.
3. On cloud-confirmed receipt, dongle deletes local copy.
4. Customer views/downloads from cloud dashboard.

**Notes:** WebSocket allows real-time streaming without polling. Logging triggers automatically on WOT detection (manual control also available). Small log size means storage is not a constraint — multiple logs can queue before upload.

---

### 4.4 Ethanol Sensor Bluetooth Bridge

> **Split 2026-05-22:** Phase 1 keeps §4.4a (logging-only ethanol % to
> WOT log rows). Live-tune feed (§4.4b) moves to Phase 3 §6 with the
> rest of the live-tune ecosystem.

#### 4.4a Logging-only (Phase 1)

> **🟡 DEFERRED-PENDING-HARDWARE (owner directive 2026-05-22).** No
> ethanol sensor on the dev RS7 today, so this section is not
> currently a Phase 1 close gate. Same posture as §4.7 Ethernet —
> the design below stays in spec, the firmware module can be
> drafted, but on-car HIL verification waits until a sensor is
> physically on the car. Re-opens automatically when the sensor
> arrives.

**Hardware:**
- External ethanol content sensor in engine bay.
- Communicates via BLE to OBD dongle under dash.

**Workflow:**
1. Sensor detects fuel blend composition.
2. Sensor broadcasts ethanol % over BLE.
3. Dongle receives BLE packets, extracts ethanol value.
4. Ethanol value is logged as a variable in WOT log rows alongside
   RPM / boost / AFR. Cloud log shows ethanol % per data point.
5. If BLE drops, UI displays warning + falls back to manual input
   for the LOGGED value (no live tune in Phase 1).

**Fallback:**
- Manual entry available in UI.
- Manual takes precedence over sensor on user override.
- Connection status (paired vs. manual) logged in data logs.

**Notes:** BLE range 10–30m. One-time pairing; auto-reconnect after that. Update rate user-configurable (1Hz / 10Hz / on-change).

#### 4.4b Live-tune feed — MOVED TO PHASE 3

> **🚚 Moved to Phase 3 §6 (2026-05-22).** The same BLE-derived ethanol
> value feeds the §6.2 constraint engine in Phase 3 to drive
> ethanol-triggered RAM updates. Tracked as P3-06 in
> `docs/PHASE_3_PREREQUISITES.md`. Phase 1 ships without this feed.

---

### 4.5 Ethanol Live-Update Constraints and Rev Limiter Safety Logic — MOVED TO PHASE 3

> **🚚 Moved to Phase 3 §6.2 (2026-05-22).** Constraint engine (5
> constraints: threshold, dwell, WOT lockout, rev-limiter drop,
> stabilization) + rev-limiter RAM clamp ship as a single package
> with §6.1 live cal switching. See `MISSION_SPEC.md` §6.2 for the
> deliverable detail and `docs/PHASE_3_PREREQUISITES.md` P3-03 /
> P3-04 for prereqs.

---

### 4.6 OBD Fault Code Read and Clear

**Functionality:**
- Read all active and pending DTCs from ECU.
- Display fault codes in live gauge UI with human-readable descriptions.
- Clear fault codes via standard OBD UDS command (service `0x14`).
- Log all reads and clears in data logs.

**User workflow:**
1. User navigates to **Faults** section in gauge UI.
2. Dongle queries ECU for all DTCs (UDS service `0x19`).
3. UI displays code, description, status (active vs. pending).
4. User selects individual codes or all, taps **Clear**.
5. Dongle sends clear command; ECU confirms.
6. UI refreshes.

**Notes:** Standard OBD/UDS query. Descriptions pulled from DB keyed on ECU type (MG1, MDG1, MED17 variants). All clears logged with timestamp for compliance/diagnostics.

---

### 4.7 Transport Layer Abstraction (CAN and Ethernet)

**Current state:** CAN supported. Ethernet hardware arriving soon.

**Design requirement:** UDS/ISO-TP stack must be **transport-agnostic** — same application logic runs over CAN or Ethernet without modification.

**Implementation:**
- Define abstract transport interface: `send_frame()`, `receive_frame()`, `get_status()`.
- CAN driver implements interface (hardware: ESP32-S3 TWAI / SN65HVD230).
- Ethernet driver implements same interface (hardware: W5500 or equivalent).
- UDS/ISO-TP layer calls abstract methods only.
- Active transport selected at boot via configuration.

**Notes:**
- ISO-TP (ISO 15765-2) handles message fragmentation over both transports.
- Frame sizes differ (CAN: 8 bytes; Ethernet: much larger), but ISO-TP abstracts this.
- Failover: if primary transport unavailable, fall back to secondary.

---

## 5. Phase 2 — Full Binary Flash (Dangerous, Tested Last)

### 5.1 Full 8MB ECU Binary Reflash via UDS/AES-128

**What it does:** completely rewrites ECU's 8MB flash with a new binary image. Uses AES-128 encryption and challenge-response authentication. Includes fault recovery to prevent bricking on mid-flash failure.

**Why it's dangerous:**
- Failed or interrupted flash can leave ECU unbootable.
- Requires verified challenge-response protocol to gain write access.
- Must be tested extensively on development vehicle before release.

**Workflow (high level):**
1. User requests full flash from server.
2. Server delivers full 8MB patched binary (distinct from SBF — this is complete firmware).
3. Dongle performs UDS handshake with ECU (AES-128 challenge-response).
4. Dongle authenticates, gains write access to flash region.
5. Dongle erases target flash sectors.
6. Dongle writes new binary in chunks over UDS.
7. Dongle verifies written data matches source.
8. On any failure, fault recovery sequence executes to leave ECU in safe state.
9. On success, ECU reboots with new binary.

**Challenge-response authentication:**
- Dongle has the AES-128 keys on file.
- Mirrors the OEM challenge-response pattern to unlock write access.

**Fault recovery:**
- Pre-defined safe-exit UDS commands if flash write fails mid-process.
- ECU revert to previous binary or recovery mode (architecture-dependent — TBD per Bosch family).
- Detailed recovery playbook documented separately.

**Testing approach:**
1. Validate all Phase 1 features on development vehicle.
2. Perform full flash test on isolated dev vehicle (not customer-facing).
3. Log all UDS commands and responses.
4. Validate new binary boots and runs correctly.
5. Only after proven safe, release in Phase 2.

**Notes:**
- Full binary is pre-built and signed by SRM (not customer-generated).
- Distinct from SBF (live RAM updates).
- One full flash per subscription period, or charged separately (TBD).
- Rollback procedure documented in case customer wants to revert.

---

## 6. Phase 3 — Live Tuning Ecosystem (RAM-write, subscription-gated)

Phase 3 silos all live-tune work — RAM writes, SBF builder, ethanol
constraint engine, map switch UI — into its own phase, mirroring how
Phase 2 silos the destructive full-binary flash.

Phase 3 features are **RAM-only** (recoverable by reboot), but they
gate behind a subscription tier separate from Phase 1 license + need
all Phase 1 prereqs GREEN before customer enablement.

Compile flag: `FUTUNER_PHASE3_ENABLED` in
`firmware/src/config/futuner_config.h`, default `0`. No customer
device ships with `=1` without Sean's explicit per-variant sign-off.

Phase 3 prereqs (P3-NN items) live in `docs/PHASE_3_PREREQUISITES.md`.

### 6.1 SBF/FBF File System and Live Calibration Switching

**SBF (Stage Binary File):** binary configuration file (SCPN
format-v4) containing:
- Calibration map definitions (start/end addresses, byte order, data type)
- Multiple tune stages in one file (Stage 1, 2, 3)
- Ethanol-variant map sets (E0, E10, E50, E85, etc.)

**Example structure:**
```
stage_one.sbf:
  - Stage 1 tune (load targets, AFR)
  - Stage 2 map definitions (ready to load)
  - Stage 3 map definitions (ready to load)
  - Ethanol-blend variants (E0, E10, E50, E85)
```

**User workflow:**
1. User requests tune from server (specifies stage 1/2/3).
2. Server identifies VIN + ECU SW version, looks up matching SBF.
3. SBF downloads to dongle storage.
4. User selects ethanol content (sensor per §4.4 / §6 split, or manual).
5. Dongle performs live RAM updates to all relevant maps.
6. **Update completes in 1.5–2 seconds maximum.**
7. User can switch stages or ethanol blends by re-loading SBF sections.

**Maps updated during live switching:**
- Load targets
- Ignition timing
- Air-fuel ratios
- Duty cycles (as needed)
- Any ethanol-dependent parameter

**SBF/FBF Builder Tool (Phase 3 deliverable P3-01):**
- Browser GUI + headless CLI
- Reads XDF + A2L + base binary + per-variant manifest
- Lets operators define custom tune profiles (stage boundaries, map
  selections)
- Generates valid SBF (SCPN format-v4 binary) — byte-identical to
  canonical Scorpion output modulo timestamp
- Advanced users can override predefined ethanol variants
- Status as of 2026-05-22: BLOCKED on scorpion-bin-tools schema
  drift (see `stuck/B1_sbf_builder.md`)

**Technical notes:**
- All updates occur in ECU RAM, **not flash** (non-destructive).
- Updates are live during driving, not just at boot.
- No firmware recompilation needed (replaces old Ghidra/patching workflow).

### 6.2 Ethanol Live-Update Constraints and Rev Limiter Safety Logic

**Problem:** ethanol content fluctuates naturally (pump blending, tank settling). Without constraints, the dongle would trigger constant micro-updates, destabilizing the tune and causing oscillation.

**Solution: hysteresis + safety lockouts.**

| # | Constraint | Description |
|---|-----------|-------------|
| 1 | **Change threshold** | Update only triggers if ethanol changes by **±3%**. Example: at E50, update only on E≤47 or E≥53. |
| 2 | **Dwell time** | After threshold crossed, wait **60s minimum** before updating. Prevents oscillation if value bounces. Timer resets if value drops back below threshold. |
| 3 | **WOT lockout** | Live updates **fully blocked** during wide-open throttle. Ethanol frozen at current value. Prevents destabilization at full power. |
| 4 | **Rev limiter reduction during update** | Limiter drops to **4000 RPM** while update is in progress. Protects engine while maps are being rewritten. |
| 5 | **Stabilization window** | After new value lands in RAM, wait **30s** before restoring full RPM range. |

**Update sequence example:**
```
T=0s:     Sensor reads E54 (was E50, +4% > threshold)
T=0–60s:  Dwell window
T=60s:    Dwell satisfied, update begins
T=60–62s: Live map update in progress (1.5–2s), rev limit at 4000 RPM
T=62s:    Update complete, ethanol locked to E54 in RAM
T=62–92s: Stabilization window (30s)
T=92s:    Full rev range restored
```

**Notes:** All timers/thresholds tunable via config file. Constraints apply regardless of input source (sensor or manual). All transitions and constraint triggers are logged.

Tracked as P3-03 (constraint engine) + P3-04 (per-variant rev
limiter RAM address) in `docs/PHASE_3_PREREQUISITES.md`.

### 6.3 9 Map Switch Slots UI

**What:** The Live Tune UI tab exposes 9 calibration slots that
correspond to slots reserved in the patched ECU binary (per the
SEFI patch pattern — slots are prefixed/marked with a sentinel,
typically `0xDEADBEEF` or equivalent; exact marker is confirmed
during P3-01 builder work by examining the SCPN-format-v4 header).

**Slot model:**
- 9 RAM slots reserved in the patched binary
- Each slot can hold one calibration variant
  (Stage 1 / Stage 2 / Stage 3 / E0 baseline / E85 max / per-customer custom)
- Slot 0 is reserved as "passthrough" / stock-equivalent — no live
  tune active

**UI surface (Live Tune tab):**
- 9 slot buttons/cards, each showing:
  - Slot index (0–8)
  - Loaded SBF filename (or empty)
  - Stage level (1/2/3) + ethanol anchor (E0/E10/E50/E85/custom)
  - Last-applied timestamp
  - Active indicator (which slot is currently fired into RAM)
- Operator actions per slot:
  - **View:** open details (per-map breakdown, ethanol curve, scale reference)
  - **Switch active:** fire the slot's content via live RAM update (§6.1 path)
  - **Upload:** replace slot's SBF (file picker or drag-drop)
  - **Mark "favorite" / "default boot" / "danger"** (customer preference, persisted per-dongle in NVS)
- Slot 0 cannot be uploaded over (always passthrough/stock-equiv).

**New WS commands:**
- `list_sbf_slots` → 9-element array of slot metadata
- `switch_sbf_slot {index: N}` → fires §6.1 apply against slot N
- `set_sbf_slot_favorite {index: N, favorite: bool}` → NVS write

**License gate:** `license_can_run_feature(FEATURE_LIVE_TUNE)` per
slot switch. Subscription tier (Phase 3) gates this surface.

Tracked as P3-08 in `docs/PHASE_3_PREREQUISITES.md`.

### 6.4 Pre-Apply Safety Gate (user-initiated apply)

**What:** §6.2 covers ETHANOL-triggered live-tune apply safety
(the constraint engine fires automatically when ethanol crosses
thresholds). USER-initiated apply (operator clicks "Apply Stage 2"
in the UI) needs its own safety surface:

**Pre-apply checks:**
- Engine state — KOEO vs idle vs cruising vs WOT (refuse apply during WOT)
- DTC presence — optionally refuse if check-engine active
  (customer-configurable strictness)
- Battery voltage — minimum threshold (uses P-51 battery_voltage
  WS surface once landed)
- License valid + paired + ECU patched (`get_status.connected/patched/paired`)
- Logger configured (so the apply outcome can be observed live)

**Confirmation gate:**
- UI dialog confirmation before destructive RAM write
- Confirmation includes preview of what's changing (load target
  delta, ignition timing delta, etc.)

**Rollback path:**
- If apply fails mid-write (NRC, timeout, ISO-TP error), the dongle
  issues stock-equivalent RAM writes to revert affected maps
- All write transactions logged with timestamps for post-mortem
- Customer-visible "Apply failed — rolled back to previous slot" toast

Tracked as P3-07 in `docs/PHASE_3_PREREQUISITES.md`.

---

## 7. Development Roadmap

### Phase 1 Deliverables (non-destructive, customer-facing)
1. VIN pairing and licensing — backend + dongle firmware.
2. Live gauge UI (WebSocket server on dongle + browser frontend).
3. Data logging and cloud sync pipeline.
4. Ethanol sensor BLE bridge firmware (§4.4a logging-only).
5. OBD fault code reader/clearer.
6. Transport abstraction layer (CAN driver finalized; Ethernet driver skeleton).

### Phase 2 Deliverables (destructive flash, dev-vehicle-only until proven)
1. Full binary reflash workflow (UDS / AES-128).
2. Challenge-response authentication mirroring.
3. Fault recovery playbook and implementation.
4. Extensive testing on development vehicle.
5. Customer-facing documentation and safety.

### Phase 3 Deliverables (live tuning ecosystem)
1. SBF file format spec + parser (already in firmware tree).
2. SBF/STF/FBF builder tool (P3-01: GUI + CLI, Stage 1/2a/2b/2c/3).
3. Live cal switching on-car E2E (P3-02).
4. Ethanol constraint engine + rev limiter safety (P3-03, P3-04).
5. Ethanol BLE live-tune feed (P3-06; companion to §4.4a logging).
6. 9 map switch slots UI (P3-08).
7. Pre-apply safety gate (P3-07).
8. Per-variant manifest schema (P3-10).
9. `FUTUNER_PHASE3_ENABLED` config flag + guard audit (P3-11).

---

## 8. Testing & Validation Strategy

| Phase | Vehicle | Risk | Sign-off |
|-------|---------|------|----------|
| Phase 1 unit + bench | Dev bench | Low | Engineering |
| Phase 1 on-car | Dev vehicle | Medium (RAM-only, recoverable by reboot) | Engineering |
| Phase 1 limited beta | 5–10 customer cars | Medium | Sean Cyr |
| Phase 1 GA release | Public | — | Sean Cyr |
| Phase 2 dev flash test | Dev vehicle only | **HIGH** (brick risk) | Sean Cyr + engineering |
| Phase 2 limited beta | Hand-picked customers | High | Sean Cyr |
| Phase 2 GA release | Public | — | Sean Cyr |
| Phase 3 unit + bench | Dev bench | Low | Engineering |
| Phase 3 on-car dev | Dev vehicle | Medium (RAM-only, recoverable by reboot; subscription-gated + license-gated; customer-experience risk if SBF malformed) | Sean Cyr + engineering |
| Phase 3 limited beta | Hand-picked customers + subscription | Medium-High (live RAM writes during driving) | Sean Cyr |
| Phase 3 GA release | Public, per-variant sign-off | — | Sean Cyr |

---

## 9. Contact

**Sean Cyr** — Owner and CEO, Silly Rabbit Motorsport
- M: (702) 907-0935
- D: (702) 901-2795
- E: sean@sillyrabbitmotorsport.com
- A: 5090 W Patrick Ln Ste #102, Las Vegas, NV 89118
