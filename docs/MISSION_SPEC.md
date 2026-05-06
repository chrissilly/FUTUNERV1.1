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

### 4.2 SBF/FBF File System and Live Calibration Switching

**SBF (Stage Binary File):** JSON-formatted configuration file containing:
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
4. User selects ethanol content (sensor or manual).
5. Dongle performs live RAM updates to all relevant maps.
6. **Update completes in 1.5–2 seconds maximum.**
7. User can switch stages or ethanol blends by re-loading SBF sections.

**Maps updated during live switching:**
- Load targets
- Ignition timing
- Air-fuel ratios
- Duty cycles (as needed)
- Any ethanol-dependent parameter

**SBF/FBF Builder Tool:**
- Reads XDF calibration files.
- Lets users define custom tune profiles (stage boundaries, map selections).
- Generates valid SBF JSON without firmware disassembly or recompilation.
- Advanced users can override predefined ethanol variants.

**Technical notes:**
- All updates occur in ECU RAM, **not flash** (non-destructive).
- Updates are live during driving, not just at boot.
- No firmware recompilation needed (replaces old Ghidra/patching workflow).

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

**Hardware:**
- External ethanol content sensor in engine bay.
- Communicates via BLE to OBD dongle under dash.

**Workflow:**
1. Sensor detects fuel blend composition.
2. Sensor broadcasts ethanol % over BLE.
3. Dongle receives BLE packets, extracts ethanol value.
4. Dongle feeds ethanol content into live-tune calculations.
5. If BLE drops, UI displays warning + falls back to manual input.

**Fallback:**
- Manual entry available in UI.
- Manual takes precedence over sensor on user override.
- Connection status (paired vs. manual) logged in data logs.

**Notes:** BLE range 10–30m. One-time pairing; auto-reconnect after that. Update rate user-configurable (1Hz / 10Hz / on-change).

---

### 4.5 Ethanol Live-Update Constraints and Rev Limiter Safety Logic

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

## 6. Development Roadmap

### Phase 1 Deliverables
1. VIN pairing and licensing — backend + dongle firmware.
2. SBF file format spec and JSON parser.
3. SBF/FBF builder tool (desktop or web-based).
4. Live gauge UI (WebSocket server on dongle + browser frontend).
5. Data logging and cloud sync pipeline.
6. Ethanol sensor BLE bridge firmware.
7. Ethanol live-update constraint logic and rev limiter.
8. OBD fault code reader/clearer.
9. Transport abstraction layer (CAN driver finalized; Ethernet driver skeleton).

### Phase 2 Deliverables
1. Full binary reflash workflow (UDS / AES-128).
2. Challenge-response authentication mirroring.
3. Fault recovery playbook and implementation.
4. Extensive testing on development vehicle.
5. Customer-facing documentation and safety.

---

## 7. Testing & Validation Strategy

| Phase | Vehicle | Risk | Sign-off |
|-------|---------|------|----------|
| Phase 1 unit + bench | Dev bench | Low | Engineering |
| Phase 1 on-car | Dev vehicle | Medium (RAM-only, recoverable by reboot) | Engineering |
| Phase 1 limited beta | 5–10 customer cars | Medium | Sean Cyr |
| Phase 1 GA release | Public | — | Sean Cyr |
| Phase 2 dev flash test | Dev vehicle only | **HIGH** (brick risk) | Sean Cyr + engineering |
| Phase 2 limited beta | Hand-picked customers | High | Sean Cyr |
| Phase 2 GA release | Public | — | Sean Cyr |

---

## 8. Contact

**Sean Cyr** — Owner and CEO, Silly Rabbit Motorsport
- M: (702) 907-0935
- D: (702) 901-2795
- E: sean@sillyrabbitmotorsport.com
- A: 5090 W Patrick Ln Ste #102, Las Vegas, NV 89118
