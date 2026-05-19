# FUTUNER — Project Context for Claude Code

> This file is loaded automatically by Claude Code in every session.
> Read it before doing any work in this repo. Treat its rules as binding.

---

## What this project is

FUTUNER is a subscription-gated aftermarket ECU tuning dongle (ESP32-S3) that performs real-time calibration switching, live ethanol blending, and full binary reflashing on Bosch MG1 / MDG1 / MED17 ECUs over UDS / ISO-TP via CAN (Ethernet support coming). The product monetizes through VIN-locked lifetime licensing.

Vendor: Silly Rabbit Motorsport. Owner: Sean Cyr.

The canonical product spec is `docs/MISSION_SPEC.md`. The scaling / server architecture is `docs/SCALE_ARCHITECTURE_PROPOSAL.md`. The supported ECU matrix is `docs/boxcode_database.md`. Always consult these before touching architecture.

---

## Hard rules (do not violate)

### 1. CAN bus discipline (will brick the gateway, not just the ECU)

- **CAN ID `0x7E0` ONLY.** Never broadcast on `0x7DF`, never address `0x710` or `0x7E1`–`0x7E7`. The C8 J533 gateway will lock out diagnostic access for 10+ minutes if you scan addresses.
- **Standard UDS services only.** `0x3E`, `0x22`, `0x10` with known subfunctions. No exotic services.
- **No `can_send_raw` probing. Period.**
- Board: `BOARD_REV2` — GPIO 5 TX, GPIO 16 RX, 500 kbps.

### 2. Feature ON/OFF discipline (project rule)

Every user-visible feature (WOT logging, SBF live tune, ethanol BLE bridge, Phase 2 flash, DTC clear, etc.) must:

1. Default to **inactive** at boot. No silent background work.
2. Only run when explicitly started via web UI command or serial command.
3. Expose `start()` / `stop()` / `is_running()` and register with the **feature manager** (the central state arbiter).
4. The feature manager enforces "only one active feature at a time." Attempting to start a feature when another is running results in a warning + clean stop of the running feature, *then* start of the requested one. Never silent preemption, never two features running concurrently.

If you are adding a new feature, the first thing you do is wire it through `feature_manager`. Not optional.

### 3. No magic numbers

Constants — thresholds, timeouts, addresses, key references, log size limits, ethanol hysteresis, rev limits, etc. — must either be:

- Read from a versioned variant manifest (per-ECU values), OR
- Read from a per-firmware-build config (defaults), OR
- Read from per-device NVS (per-customer values).

Integer literals in `.c` files for behavioral constants are not acceptable. If you genuinely need a new constant, name it, default it in config, and surface it in this `CLAUDE.md` or `docs/SCALE_ARCHITECTURE_PROPOSAL.md` so it gets reviewed before lock.

### 4. Mandatory progress logging

Every active session must update:

- `status-YYYY-MM-DD.md` at the workspace root — what was done, decisions made, open questions.
- `file-update-YYYY-MM-DD.md` at the workspace root — for every file written or edited, a concise note on what changed and why.

These exist already at `/Users/rabbit/esp/obd/`. Append to today's file if it exists; create today's if it doesn't.

### 5. Proprietary IP

All data here is SRM proprietary intellectual property. Do not exfiltrate, do not retain outside the local working tree, do not paste into third-party services for analysis. AES keys live in `secrets/` which is `.gitignore`d — never copy keys out of that directory in any artifact you produce.

### 6. Modular design + concise files

Keep individual files focused on a single responsibility. If a file exceeds ~500 lines, that's a smell — propose a split before continuing.

---

## Repository layout

```
FUTV1.1/
├── CLAUDE.md                                 ← you are here
├── README.md                                 ← high-level layout & build commands
├── docs/
│   ├── MISSION_SPEC.md                       ← canonical product spec (Phase 1 + Phase 2)
│   ├── SCALE_ARCHITECTURE_PROPOSAL.md        ← server / scaling architecture
│   ├── CAN_UDS_PROTOCOL.md                   ← every UDS service the dongle uses
│   ├── boxcode_database.{md,json}            ← supported ECU variant matrix (36 today, 100+ target)
│   ├── ecu_variable_db.json                  ← mapped ECU RAM variables (53 entries)
│   ├── uds_v1_protocol_albin.md              ← v1.0 UDS sequence reference
│   └── HISTORY_uds_regression.md             ← what broke between v1 and v2
├── firmware/
│   ├── src/
│   │   ├── main.c                            ← boot + top-level state
│   │   ├── state_machine/                    ← (note: actually CAN connection mgmt, NOT the feature arbiter — the feature arbiter is its own module)
│   │   ├── can/                              ← CAN driver
│   │   ├── isotp_coordinator/                ← ISO-TP segmentation
│   │   ├── commands/                         ← command dispatch (serial + websocket); ~14 command modules
│   │   ├── scal/, bdef/, ecu_write/          ← live tune RAM update path (proven from v1.0, do not modify casually)
│   │   ├── logger/                           ← gauge stream + WOT log capture
│   │   ├── flash/                            ← Phase 2 binary flash writer
│   │   ├── flex_fuel/                        ← ethanol constraint logic
│   │   ├── websocket/                        ← browser UI streaming
│   │   ├── wifi/                             ← AP→STA pairing
│   │   ├── nvs/                              ← persistent storage
│   │   ├── ota/                              ← firmware update (dual-bank)
│   │   ├── error/                            ← fault recovery + safe-idle
│   │   └── filesystem/, config/              ← support modules
│   ├── futuner_control_panel.html            ← single-page UI (loaded onto dongle)
│   ├── partitions.csv, sdkconfig.defaults, CMakeLists.txt
│   └── build.sh, flash.sh, monitor.sh
├── cloud/                                    ← FastAPI + SQLite cloud (api.sillyrabbitmotorsport.com)
├── ui/                                       ← active SPA (red/black theme)
├── sbf/                                      ← sample SBF/FBF files
├── tools/                                    ← sbf_to_json.py, can_sniff.py
├── hw_reference/                             ← XDFs, RE docs, working dongle dumps
└── secrets/                                  ← AES-128 keys (GITIGNORED, never commit)
```

---

## Build & flash

```bash
cd firmware
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3   # one-time
./build.sh
./flash.sh -p /dev/cu.usbmodemXXXX
./monitor.sh
```

Source baseline: git commit `7b4e525` is the last verified working firmware (12.4 Hz logger on car). Live-tune logic preserved byte-perfect from `ALBIN/ALBIN_WIP/ALBIN_WIP/sefi_firmware/`. Do not regress these.

---

## Current implementation status (as of 2026-05-05)

| Mission section | Status |
|-----------------|--------|
| 4.1 VIN pairing & licensing | not built |
| 4.2 SBF live calibration | scal/bdef/ecu_write proven; orchestrator needs flesh-out |
| 4.3 Live gauges | working (12.4 Hz proven on dev car) |
| 4.3 WOT logging | trigger + 60s cap + gzip + queue not implemented |
| 4.4 BLE ethanol bridge | not built |
| 4.5 Ethanol constraint logic + rev limit window | not built |
| 4.6 DTC read/clear | command stubs exist (`commands/dtc_commands.c`) |
| 4.7 Transport abstraction | CAN works; abstraction + Ethernet not built |
| 5.1 Phase 2 full binary flash | research complete, keys on file, not implemented |
| **Feature manager (state arbiter)** | **does not exist — required before further feature work** |

---

## Working agreements

- Ask clarifying questions when requirements are ambiguous. Do not assume.
- Critically evaluate any approach proposed. Flag scalability or maintainability concerns.
- Build and test features individually. The ON/OFF rule is enforced through the feature manager — do not bypass it.
- Update `status-YYYY-MM-DD.md` and `file-update-YYYY-MM-DD.md` for every session.
- Reference `docs/MISSION_SPEC.md` as the source of truth for product behavior. Reference `docs/SCALE_ARCHITECTURE_PROPOSAL.md` for server / scaling decisions.
