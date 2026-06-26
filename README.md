# FUTV1.1 — FUTUNER Mission Build

> Mission-focused subset of FUTV1.0, organized to deliver Phase 1 and Phase 2 of `docs/MISSION_SPEC.md`.

## Layout

```
FUTV1.1/
├── README.md                 ← this file
├── .gitignore                ← excludes secrets/
│
├── firmware/                 ← ESP32-S3 firmware (cherry-picked working tree)
│   ├── src/                  ← C source: CAN, ISO-TP, UDS, logger, ECU write,
│   │                            scal/bdef live-tune (from v1.0, identical),
│   │                            wifi, websocket, commands, filesystem
│   ├── futuner_control_panel.html  ← embedded UI (single-page, β plan)
│   ├── partitions.csv, sdkconfig.defaults, CMakeLists.txt
│   ├── build.sh, flash.sh, monitor.sh
│   └── prebuilt/             ← last known-good binaries
│
├── ui/
│   ├── control_panel.html    ← active SPA (red/black theme, mission features)
│   └── _reference/           ← ScorpionEFI compiled bundle (visual reference only)
│
├── cloud/                    ← FastAPI backend (sillyrabbitmotorsport.com)
│   ├── src/                  ← endpoints: VIN pairing, SBF download, log upload
│   ├── tests/                ← pytest suite (53 tests passing)
│   └── docker-compose.yml
│
├── sbf/                      ← Sample SBF/FBF files + JSON
├── tools/                    ← sbf_to_json.py, can_sniff.py
│
├── docs/                     ← mission-essential reference
│   ├── MISSION_SPEC.md       ← THE spec — Phase 1 features + Phase 2 flash
│   ├── CAN_UDS_PROTOCOL.md   ← every UDS service the dongle uses
│   ├── boxcode_database.{md,json}  ← 36 boxcodes + per-ECU config
│   ├── ecu_variable_db.json  ← 53 mapped ECU RAM variables
│   ├── uds_v1_protocol_albin.md    ← v1.0 UDS sequence reference
│   └── HISTORY_uds_regression.md   ← what broke between v1 and v2
│
├── hw_reference/             ← XDFs, RE docs, working dongle dumps, sample ECU
│
└── secrets/                  ← AES-128 keys (GITIGNORED, never commit)
    ├── AES_KEYS_MASTER.md    ← key table per ECU variant
    └── aes_keys_per_boxcode.json  ← per-boxcode key/IV mapping
```

## Build & Flash (firmware)

The `firmware/` wrapper scripts are the supported entry points. `build.sh`
bundles the split UI sources into `futuner_control_panel.html` *before*
`idf.py build` — a bare `idf.py build` would embed a stale UI.

```bash
cd firmware
# one-time: ESP-IDF v5.5 at ~/esp/esp-idf (or export IDF_PATH=...), then:
. ~/esp/esp-idf/export.sh && idf.py set-target esp32s3
./build.sh                        # bundle UI, then idf.py build
./flash.sh                        # build + app-only flash to first /dev/cu.usbmodem*
./flash.sh /dev/cu.usbmodemXXXX   # explicit port
./flash.sh --full                 # first flash per dongle: bootloader + part-table + app
./flash.sh --prebuilt             # flash prebuilt/*.bin, skip building
./monitor.sh                      # tail serial @115200 (-t 60 for a timed capture)
```

**Just testing — no toolchain?** `TESTING.md` has the flash-from-prebuilt
path (`./flash.sh --prebuilt`) and a Phase 1 walkthrough.

## Mission Status

See `docs/MISSION_SPEC.md`. Phase 1 (RAM-only, non-destructive) is the priority.

| Section | Status |
|---------|--------|
| 4.1 VIN pairing & licensing | needs build |
| 4.2 SBF live calibration | partial — scal/bdef/ecu_write working from v1.0; orchestrator needs flesh-out |
| 4.3 Live gauges + WOT logging | working (12.4 Hz proven); WOT trigger needs implementation |
| 4.4 BLE ethanol sensor bridge | needs build |
| 4.5 Ethanol constraint logic | needs build |
| 4.6 DTC read/clear | stubs exist (`commands/dtc_commands.c`) |
| 4.7 Transport abstraction | CAN works; abstraction layer + Ethernet pending |
| 5.1 Phase 2 — Full binary flash | research complete (`hw_reference/MG1_MDG1_Flashing_Research_Part1.md`); keys in `secrets/`; needs implementation |

## Critical Operational Rules

1. **CAN ID 0x7E0 ONLY.** Never broadcast on 0x7DF, never address 0x710 / 0x7E1-7. The C8 J533 gateway will lock out diagnostic access for 10+ minutes if you scan addresses.
2. **Standard UDS services only.** `0x3E`, `0x22`, `0x10` with known subfunctions.
3. **No `can_send_raw` probing.** Period.
4. **Board pinout:** BOARD_REV2 (GPIO 5 TX, GPIO 16 RX, 500 kbps).

## Origin
- Built 2026-05-04 from cherry-picked assets in `~/esp/obd/FUTV1.0/`
- Source baseline: git commit `7b4e525` (last verified working, 12.4 Hz logger on car)
- Live-tune logic preserved byte-perfect from Sean's v1.0 (`ALBIN/ALBIN_WIP/ALBIN_WIP/sefi_firmware/`)
