# FUTUNER v2 firmware

> The active firmware project for the SRM/FUTUNER OBD-II dongle.
>
> Source consolidated 2026-05-04 from `~/esp/obd/SEFIv1/` (legacy folder
> name). Includes the working tree as of that date — UDS regression
> reverted, JSON shape fix in `cmd_get_logger_data`, `BOARD_V10` (GPIO
> 21/14) added for the v1.0 SEFI dongle, 0x5001 SRAM logger variables
> temporarily disabled.

---

## Quick start

```bash
# Build (uses ~/esp/esp-idf, override IDF_PATH if needed)
./build.sh

# Flash to a dongle on USB (auto-detects /dev/cu.usbmodem*)
./flash.sh                # app-only (default, fast)
./flash.sh --full         # bootloader + partition table + app (first time)
./flash.sh --prebuilt     # use prebuilt/futuner_v2.bin without building

# Tail serial
./monitor.sh
```

---

## Directory layout

```
firmware_v2/
├── README.md            ← this file
├── CMakeLists.txt       ← root project file
├── partitions.csv       ← 16 MB layout (app0/app1/cal/data)
├── sdkconfig.defaults
├── dependencies.lock
│
├── build.sh             ← idf.py build wrapper
├── flash.sh             ← build + esptool write_flash wrapper
├── monitor.sh           ← serial tail wrapper
│
├── src/                 ← firmware source (~80 .c/.h files)
│   ├── main.c
│   ├── bdef/            ← BDEF binary calibration parser
│   ├── can/             ← CAN bus + ISO-TP
│   │   └── can_config.h ← BOARD_V10/REV1/REV2 pin selection
│   ├── commands/        ← WebSocket JSON command handlers
│   ├── ecu_write/       ← UDS RAM write (0x3E + 0x39)
│   ├── error/           ← centralized error tracker
│   ├── filesystem/      ← LittleFS manager
│   ├── flex_fuel/       ← real-time map blend (currently stubbed)
│   ├── isotp_coordinator/
│   ├── logger/          ← logger config + poll + variable DB
│   ├── nvs/             ← non-volatile storage
│   ├── ota/             ← firmware OTA update logic
│   ├── scal/            ← SCAL calibration parser
│   ├── state_machine/   ← 22-state ECU connection manager + UDS
│   ├── websocket/       ← HTTP + WebSocket server
│   └── wifi/            ← WiFi AP (STA reconnect removed)
│
├── components/          ← isotp-c (git submodule, vendored copy)
├── main/                ← IDF main wrapper
├── include/, lib/, test/  ← per ESP-IDF project conventions
│
├── prebuilt/            ← latest known-good binaries (skip build to flash)
│   ├── futuner_v2.bin
│   ├── bootloader.bin
│   ├── partition-table.bin
│   └── ota_data_initial.bin
│
└── build/               ← idf.py output (gitignored, regenerated)
```

---

## Current firmware state

| Property | Value |
|---|---|
| Target | ESP32-S3, 16 MB flash, 8 MB PSRAM |
| Active board | `BOARD_V10` (CAN GPIO 21/14) — selected in `src/can/can_config.h` |
| ESP-IDF | v5.5 |
| Logger | 6 active variables (RPM, ethanol, load, coolant, throttle, cruise) |
| WiFi | AP only (`FUTUNER_<6hex>` / `password`) |
| AP IP | `192.168.10.1` |
| WebSocket | port 80, password `futuner_admin_2024` |

The 4 logger variables in the `0x5001` SRAM region (`rlp_w`, `pvdg_w`,
`zwoutzyl_w`, `frm_w`) are commented out in `src/logger/logger_variables.c`
because they cause truncated poll responses in the current ECU state.
Re-enable one at a time after capturing a candleLight CAN trace to
identify the bad address.

---

## Recent changes (uncommitted in the working tree as of 2026-05-04)

| File | Change |
|---|---|
| `src/can/can_config.h` | Added `BOARD_V10` for the v1.0 SEFI dongle (GPIO 21/14), corrected misleading flash-size-based pin doc |
| `src/commands/logger_data_commands.c` | Rewrote `cmd_get_logger_data` JSON shape — UI expects `{success: true, data: {...}}` not `{status: "success", variables: [...]}` |
| `src/logger/logger_variables.c` | Disabled 4 variables in `0x5001` SRAM region for boxcode `4K0907557G__0003` |
| `src/wifi/wifi_ap.c` | (Already staged before consolidation) Removed STA aggressive-reconnect that starved CAN RX |
| `src/state_machine/connection_manager.c` | (Already staged) Removed `fast_poll_loop()` and reverted `vTaskDelay` |

---

## How this fits into the broader FUTV1.0 folder

```
~/esp/obd/FUTV1.0/
├── firmware/         ← v1.0 SEFI dongle's byte-perfect baseline (Sean's binaries)
├── firmware_v2/      ← THIS FOLDER (active FUTUNER firmware project)
├── ui/               ← recovered ScorpionEFI React UI (visual baseline)
├── sbf/              ← Sean's calibration binaries + JSON decodes
├── tools/            ← SBF decompiler, can_sniff.py
├── recovery/         ← restore scripts (diagnose, erase cal, force boot, etc.)
├── cloud_server/     ← sillyrabbitmotorsport.com server (replaces api.dynoscorpion.com)
├── docs/             ← project knowledge base + RE writeups
├── hw_reference/     ← XDF, working dongle dumps, MDG1 RE docs
└── README.md         ← entry point
```

Treat this `firmware_v2/` folder as the canonical source going forward.
The legacy `~/esp/obd/SEFIv1/` working tree remains in place for git
history reference; once you've confirmed FUTV1.0 builds and flashes
clean, that folder can be archived.
