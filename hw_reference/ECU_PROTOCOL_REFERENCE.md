# SEFI ECU Flasher - Project Reference

> **Project:** FUTUNER OBD Dongle Flasher  
> **Target ECU:** VAG MDG1 (MG1CS002 and variants)  
> **Last Updated:** 2026-02-05  
> **Status:** In Development — WiFi + Web UI + Upload working

---

## Table of Contents

1. [Hardware Overview](#hardware-overview)
2. [Bill of Materials](#bill-of-materials)
3. [Pinout & Connections](#pinout--connections)
4. [Software Architecture](#software-architecture)
5. [Protocol Stack](#protocol-stack)
6. [Project Status](#project-status)
7. [TODO List](#todo-list)
8. [Reference Documents](#reference-documents)
9. [Sample Firmware Binary Analysis](#sample-firmware-binary-analysis)

---

## Hardware Overview

### Core Specifications

| Component | Part Number | Specification |
|-----------|-------------|---------------|
| MCU | ESP32-S3-WROOM-2-N32R16V | Dual-core LX7 @ 240MHz |
| Internal Flash | (included in module) | 32MB Quad SPI |
| PSRAM | (included in module) | **16MB Octal** |
| External NAND | MX35LF4GE4AD-Z4I-T | 512MB (4Gbit) SPI NAND |
| CAN Transceiver | NCV7344D13R2G | CAN/CAN-FD, AEC-Q100 |
| USB Connector | USB4110-GF-A | USB-C 2.0 |
| Form Factor | OBD-II Dongle | 16-pin male connector |

### Memory Budget

| Resource | Available | MDG1 Firmware | Headroom |
|----------|-----------|---------------|----------|
| PSRAM | 16MB | ~8MB | **8MB free** |
| Internal Flash | 32MB | (code + data) | Plenty |
| External NAND | 512MB | (future: multi-firmware storage) | Massive |

> ✅ **Key Insight:** 16MB PSRAM can hold entire firmware. No streaming needed.

### Power Architecture

```
Vehicle 12V (OBD Pin 16)
        │
        ▼
┌───────────────────┐
│   AP63205WU-7     │  5V @ 2A Buck Converter
│   (IC5)           │
└───────┬───────────┘
        │ 5V
        ▼
┌───────────────────┐
│   AP63203WU-7     │  3.3V @ 2A Buck Converter
│   (IC3)           │
└───────┬───────────┘
        │ 3.3V
        ├──────────────► ESP32-S3 VCC
        ├──────────────► NAND VCC
        └──────────────► CAN Transceiver VCC
```

---

## Bill of Materials

| Ref | Qty | Part Number | Description | Package |
|-----|-----|-------------|-------------|---------|
| IC1 | 1 | ESP32-S3-WROOM-2-N32R16V | MCU Module (32MB Flash, 16MB PSRAM) | Module |
| IC2 | 1 | NCV7344D13R2G | CAN/CAN-FD Transceiver | SOIC-8 |
| IC3 | 1 | AP63203WU-7 | 3.3V 2A Buck Converter | TSOT-26 |
| IC4 | 1 | MX35LF4GE4AD-Z4I-T | 512MB SPI NAND Flash | WSON-8 |
| IC5 | 1 | AP63205WU-7 | 5V 2A Buck Converter | TSOT-26 |
| D1 | 1 | USBLC6-2SC6Y | USB ESD Protection | SOT-23-6 |
| D2 | 1 | PMEG3020EXEX | 30V 2A Schottky | SOD-123 |
| D3 | 1 | SMF30CA | TVS Diode 30V | SMF |
| D4 | 1 | PMEG2005EGWX | 20V 0.5A Schottky | SOD-123 |
| FL1 | 1 | DLW43SH510XK2L | 51µH Common Mode Choke | 1812 |
| FL2 | 1 | BLM18PG221SN1D | 220Ω Ferrite Bead | 0603 |
| L1, L2 | 2 | DFE252012PD-4R7M | 4.7µH Power Inductor | 1008 |
| C1 | 1 | EMK107B7105KAHT | 1µF 16V MLCC | 0402 |
| C3 | 1 | 593D106X9035D2TE3 | 10µF 35V Tantalum | 2917 |
| C4,C6,C7,C9 | 4 | GRT21BD71A226ME13L | 22µF 10V MLCC | 0805 |
| C10,C2,C5 | 3 | GCM155R71E104KE02J | 100nF 25V MLCC | 0402 |
| R1, R4 | 2 | CRCW06035K10FKEA | 5.1kΩ 1% | 0603 |
| R2,R3,R8 | 3 | RC0603FR-1310KL | 10kΩ 1% | 0603 |
| R6, R7 | 2 | AC0603FR-0722RL | 22Ω 1% | 0603 |
| R9 | 1 | AC0603FR-1047KL | 47kΩ 1% | 0603 |
| J3 | 1 | USB4110-GF-A | USB-C 2.0 Receptacle | SMD |
| OBDII | 1 | AOT-1209 | OBD-II Male Connector + Housing | TH |

---

## Pinout & Connections

### ESP32-S3 Pin Assignments

> **CRITICAL: CAN pins differ between board revisions! See table below.**
> Verified 2026-03-29 by reverse-engineering working firmware + hardware designer confirmation.

#### Board Rev 1 (old board, 32MB flash, 16MB PSRAM)

| GPIO | Function | Direction | Connected To | Notes |
|------|----------|-----------|--------------|-------|
| 0 | BOOT | Input | Boot button (if present) | Hold low for download mode |
| **21** | **TWAI_TX** | **Output** | **NCV7344 TXD** | **CAN transmit** |
| **14** | **TWAI_RX** | **Input** | **NCV7344 RXD** | **CAN receive** |
| 19 | USB_D- | Bidir | USB-C D- | Native USB |
| 20 | USB_D+ | Bidir | USB-C D+ | Native USB |
| 10 | SPI2_CS | Output | NAND CS# | NAND chip select |
| 11 | SPI2_CLK | Output | NAND SCLK | NAND clock |
| 12 | SPI2_MOSI | Output | NAND SI | NAND data in |
| 13 | SPI2_MISO | Input | NAND SO | NAND data out |

#### Board Rev 2 (new board, 16MB flash, 8MB PSRAM)

| GPIO | Function | Direction | Connected To | Notes |
|------|----------|-----------|--------------|-------|
| 0 | BOOT | Input | Boot button (if present) | Hold low for download mode |
| **5** | **TWAI_TX** | **Output** | **NCV7344 TXD** | **CAN transmit** |
| **16** | **TWAI_RX** | **Input** | **NCV7344 RXD** | **CAN receive** |
| 19 | USB_D- | Bidir | USB-C D- | Native USB |
| 20 | USB_D+ | Bidir | USB-C D+ | Native USB |
| 10 | SPI2_CS | Output | NAND CS# | NAND chip select |
| 11 | SPI2_CLK | Output | NAND SCLK | NAND clock |
| 12 | SPI2_MOSI | Output | NAND SI | NAND data in |
| 13 | SPI2_MISO | Input | NAND SO | NAND data out |

> **How to identify board revision:** Run `esptool.py flash_id`. 32MB = Rev 1, 16MB = Rev 2.
>
> **Previous values in this doc (GPIO 17/18) were WRONG for both boards.**

### NCV7344D13R2G CAN Transceiver

| Pin | Name | Connection (Rev 1) | Connection (Rev 2) |
|-----|------|--------------------|--------------------|
| 1 | TXD | ESP32 GPIO 21 | ESP32 GPIO 5 |
| 2 | GND | Ground | Ground |
| 3 | VCC | 3.3V | 3.3V |
| 4 | RXD | ESP32 GPIO 14 | ESP32 GPIO 16 |
| 5 | VIO | 3.3V | 3.3V |
| 6 | CANL | OBD Pin 14 | OBD Pin 14 |
| 7 | CANH | OBD Pin 6 | OBD Pin 6 |
| 8 | STB | GND (always active) | GND (always active) |

### OBD-II Connector Pinout

| Pin | Signal | Usage |
|-----|--------|-------|
| 4 | Chassis Ground | GND |
| 5 | Signal Ground | GND |
| 6 | CAN High | CAN_H |
| 14 | CAN Low | CAN_L |
| 16 | Battery +12V | Power input |

---

## Software Architecture

### File Structure (current)

```
SEFI-OBDII-ECU-Flasher/firmware/
├── platformio.ini              # ✅ Debug + Release environments
├── partitions.csv              # ✅ Custom partition table
├── build.bat / build.ps1       # ✅ Build automation
├── flash.bat                   # ✅ Compile + flash + monitor
├── clean_flash.bat             # ✅ Clean rebuild + flash
├── build_ui.bat                # ✅ React → C header embedding
├── serial.bat                  # ✅ Serial monitor only
├── flash_erase_all.bat         # ✅ Full erase
├── reboot.bat                  # ✅ Soft-reboot
├── ui/                         # ✅ React + Tailwind + Vite
│   ├── src/
│   │   ├── App.jsx             # ✅ Main app (status poll, upload, flash flow)
│   │   ├── brand.config.js     # ✅ White-label branding
│   │   └── components/
│   │       ├── FileUpload.jsx  # ✅ Drag-drop upload with validation
│   │       ├── FlashProgress.jsx # ✅ Progress bar (placeholder)
│   │       ├── Header.jsx      # ✅ Branding + connection status
│   │       ├── StatusCard.jsx  # ✅ State display
│   │       └── SystemInfo.jsx  # ✅ PSRAM/heap/uptime stats
│   └── scripts/
│       └── embed.js            # ✅ Gzip + C header generator
└── src/
    ├── main.cpp                # ✅ Entry point, serial commands (help/dev/status/creds/mem)
    ├── main.h                  # ✅ App state machine
    ├── settings/
    │   ├── pinout.h            # ✅ GPIO definitions
    │   └── settings.h          # ✅ All config constants (WiFi, CAN, UDS, PSRAM)
    ├── logging/
    │   └── logging.h           # ✅ Log macros (tagged, leveled)
    ├── credentials/
    │   ├── credential_generator.cpp  # ✅ Random SSID/password generation
    │   └── credential_generator.h
    ├── wifi/
    │   ├── wifi_manager.cpp    # ✅ AP mode + mDNS (sefi.local)
    │   └── wifi_manager.h
    ├── web/
    │   ├── web_server.cpp      # ✅ AsyncWebServer + API endpoints
    │   ├── web_server.h
    │   └── embedded/           # ✅ Auto-generated gzipped UI headers
    ├── can/
    │   ├── can_manager.h       # ⬜ Header only — no .cpp yet
    │   └── uds/
    │       └── uds_types.h     # ✅ SID, NRC, Session enums + helpers
    ├── flash/
    │   └── .gitkeep            # ⬜ Empty — .sefi parser not started
    └── usb/
        └── .gitkeep            # ⬜ Empty — USB CDC not started
```

### Web API Endpoints

| Method | Path | Status | Description |
|--------|------|--------|-------------|
| GET | `/api/status` | ✅ Working | PSRAM, heap, uptime, clients, upload state |
| POST | `/api/upload` | ✅ Working | Multipart firmware upload to PSRAM |
| DELETE | `/api/upload` | ✅ Working | Free PSRAM buffer (call before re-upload) |
| POST | `/api/flash` | ⬜ Stub | Returns success but no actual flash logic |
| POST | `/api/reboot` | ✅ Working | Soft-reboot device |
| GET | `/*` | ✅ Working | Serves embedded React UI (gzipped) |

### Serial Commands

| Command | Status | Description |
|---------|--------|-------------|
| `help` | ✅ | Command list |
| `dev` | ✅ | Full dev quick-reference card (build cmds, workflows, config) |
| `status` / `s` | ✅ | System state, uptime, memory, WiFi clients |
| `creds` | ✅ | Show WiFi AP SSID + password |
| `mem` | ✅ | Detailed PSRAM + heap diagnostics |
| `reboot` | ✅ | Restart device |
| `crash` | ✅ | Intentional null-pointer crash (debug testing) |

### Upload Flow (current)

```
React UI                        ESP32 Web Server              PSRAM
─────────                       ────────────────              ─────
User drops .bin file
  │
  ├─ DELETE /api/upload ──────► free(s_uploadBuffer)          [freed]
  │                              s_uploadBuffer = nullptr
  │                              ◄── 200 {freed, psram_free}
  │
  ├─ POST /api/upload ────────► index==0:
  │   (multipart)                 ps_malloc(10MB) ──────────► [allocated]
  │   chunk 1 ────────────────► memcpy(chunk) ──────────────► [filling...]
  │   chunk 2 ────────────────► memcpy(chunk) ──────────────► [filling...]
  │   ...                        ...
  │   final=true ─────────────► s_uploadSize = received       [complete]
  │                              ◄── 200 {size, filename}
  │
  ├─ "Ready to Flash" shown
  │
  ├─ POST /api/flash ─────────► TODO: read from PSRAM → UDS → CAN → ECU
```

### Simplified Data Flow (target architecture)

```
┌───────────────────────────────────────────────────────────────────────────────┐
│                              PC (Python Tool)                                │
│                                                                              │
│   1. Load ODX → Extract SA2 bytecode, checksums, block definitions           │
│   2. Load firmware .bin (already encrypted)                                  │
│   3. Package into single .sefi file                                          │
│   4. Upload to ESP32 via WiFi or USB serial                                  │
│   5. Send "FLASH" command                                                    │
│   6. Monitor progress                                                        │
│                                                                              │
└──────────────────────────────────┬────────────────────────────────────────────┘
                                   │
                             WiFi / USB Serial
                                   │
┌──────────────────────────────────▼────────────────────────────────────────────┐
│                              ESP32-S3                                         │
│                                                                              │
│   ┌───────────────────────────────────────────────────────────────────────┐   │
│   │                      PSRAM (16MB)                                    │   │
│   │   ┌────────────────────────────────────────────────────────────┐     │   │
│   │   │  FlashPackage (~8MB)                                       │     │   │
│   │   │  ├─ header (magic, version, total_size)                    │     │   │
│   │   │  ├─ sa2_bytecode[64]                                       │     │   │
│   │   │  ├─ block_count                                            │     │   │
│   │   │  ├─ blocks[N]                                              │     │   │
│   │   │  │   ├─ address, size, checksum                            │     │   │
│   │   │  │   └─ data[size]  ← encrypted firmware bytes             │     │   │
│   │   │  └─ ...                                                    │     │   │
│   │   └────────────────────────────────────────────────────────────┘     │   │
│   └───────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│   On "FLASH" command:                                                        │
│   ┌───────────────────────────────────────────────────────────────────────┐   │
│   │  UDS Flash State Machine                                             │   │
│   │  - Reads from PSRAM buffer                                           │   │
│   │  - No PC communication during flash                                  │   │
│   │  - Reports progress after completion                                 │   │
│   └─────────────────────────────────┬─────────────────────────────────────┘   │
│                                     │                                        │
└─────────────────────────────────────┼────────────────────────────────────────┘
                                      │
                                 CAN Bus (500kbps)
                                      │
┌─────────────────────────────────────▼────────────────────────────────────────┐
│                              MDG1 ECU                                        │
│   Receives UDS commands, writes firmware to internal flash                   │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Flash Package Format (.sefi)

Binary file format uploaded to ESP32:

```
Offset  Size    Field                   Description
──────────────────────────────────────────────────────────────────────────
0x0000  4       magic                   "SEFI" (0x53454649)
0x0004  2       version                 Format version (0x0001)
0x0006  2       flags                   Reserved
0x0008  4       total_size              Total package size
0x000C  4       block_count             Number of data blocks
0x0010  2       sa2_length              SA2 bytecode length
0x0012  62      sa2_bytecode            SA2 algorithm (padded)
0x0050  varies  blocks[]                Array of BlockHeader + data

BlockHeader (per block):
──────────────────────────────────────────────────────────────────────────
+0x00   1       block_id                Block identifier (0x01-0x06)
+0x01   1       address                 Target address byte
+0x02   4       compressed_size         Size of encrypted data
+0x06   4       uncompressed_size       Original size
+0x0A   4       checksum                CRC32 for verification
+0x0E   2       reserved                Padding
+0x10   N       data[]                  Encrypted firmware data
```

### Layer Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Application Layer                            │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  Web Server (web_server.cpp)                        ✅ DONE  │  │
│  │  - Serves embedded React UI (gzipped)                        │  │
│  │  - REST API: status, upload, clear, flash, reboot            │  │
│  │  - File upload to PSRAM with proper free/realloc             │  │
│  └──────────────────────────┬────────────────────────────────────┘  │
│                             │                                       │
│  ┌──────────────────────────▼────────────────────────────────────┐  │
│  │  Flash Manager (uds_flash.cpp)                      ⬜ TODO  │  │
│  │  - Parses FlashPackage from PSRAM                            │  │
│  │  - Runs state machine                                        │  │
│  │  - Calls SA2 VM for key calculation                          │  │
│  │  - Reports progress via WebSocket/API                        │  │
│  └──────────────────────────┬────────────────────────────────────┘  │
│                             │                                       │
└─────────────────────────────┼───────────────────────────────────────┘
                              │
┌─────────────────────────────┼───────────────────────────────────────┐
│                             │     UDS Protocol Layer                │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  SA2 Virtual Machine (sa2_vm.cpp)                   ⬜ TODO  │  │
│  │  - Executes bytecode from package                            │  │
│  │  - Calculates key from seed                                  │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  UDS Services (uds_manager.cpp)                     ⬜ TODO  │  │
│  │  - DiagnosticSessionControl (0x10)                           │  │
│  │  - SecurityAccess (0x27)                                     │  │
│  │  - RoutineControl (0x31)                                     │  │
│  │  - RequestDownload (0x34)                                    │  │
│  │  - TransferData (0x36)                                       │  │
│  │  - RequestTransferExit (0x37)                                │  │
│  │  - ECUReset (0x11)                                           │  │
│  └──────────────────────────┬────────────────────────────────────┘  │
│                             │                                       │
└─────────────────────────────┼───────────────────────────────────────┘
                              │
┌─────────────────────────────┼───────────────────────────────────────┐
│                             │     Transport Layer                   │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  ISO-TP (ISO 15765-2)                               ⬜ TODO  │  │
│  │  (uds_messaging.cpp)                                         │  │
│  │  - Single Frame (SF): ≤7 bytes                               │  │
│  │  - First Frame (FF): Start multi-frame                       │  │
│  │  - Consecutive Frame (CF): Continue data                     │  │
│  │  - Flow Control (FC): Pacing                                 │  │
│  │  - Max payload: 4095 bytes per transfer                      │  │
│  └──────────────────────────┬────────────────────────────────────┘  │
│                             │                                       │
└─────────────────────────────┼───────────────────────────────────────┘
                              │
┌─────────────────────────────┼───────────────────────────────────────┐
│                             │     Driver Layer                      │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  TWAI Driver (can_manager.cpp)                      ⬜ TODO  │  │
│  │  - 500 kbps                                                  │  │
│  │  - TX ID: 0x7E0                                              │  │
│  │  - RX filter: 0x7E8                                          │  │
│  │  - Native ESP32-S3 TWAI peripheral                           │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Protocol Stack

### CAN Bus Configuration

| Parameter | Value |
|-----------|-------|
| Bitrate | 500 kbps |
| Tester TX ID | 0x7E0 |
| ECU RX ID | 0x7E8 |
| TesterPresent Interval | ~2000ms |

### UDS Flash Sequence

```
Phase 1: Session Setup
─────────────────────────────────────────────────────────────────────
[10 03]             → Extended Diagnostic Session
                    ← [50 03 ...]

Phase 2: Security Unlock  
─────────────────────────────────────────────────────────────────────
[10 02]             → Programming Session
                    ← [50 02 ...]

[27 11]             → Request Seed
                    ← [67 11 xx xx xx xx]

[27 12 kk kk kk kk] → Send Key (SA2 calculated from PSRAM bytecode)
                    ← [67 12]

Phase 3: Flash Each Block (from PSRAM buffer)
─────────────────────────────────────────────────────────────────────
[31 01 FF 00 ...]   → Erase Block
                    ← [71 01 FF 00 00]

[34 ...]            → Request Download
                    ← [74 ...]

[36 01 ...]         → Transfer Data (read from PSRAM)
[36 02 ...]
...
                    ← [76 xx]

[37]                → Transfer Exit
                    ← [77]

[31 01 02 02 ...]   → Verify Checksum (from PSRAM)
                    ← [71 01 02 02 00]

Phase 4: Finalize
─────────────────────────────────────────────────────────────────────
[31 01 FF 01]       → Verify Dependencies
                    ← [71 01 FF 01 00]

[11 01]             → ECU Reset
                    ← [51 01]
```

### SA2 Bytecode Reference

```
Bytecode from ODX: 6807870401201593050220164A03826B068193060320178407042018494C4C

Opcode │ Mnemonic │ Operand    │ Description
───────┼──────────┼────────────┼─────────────────────────────────
0x68   │ FOR      │ count (1B) │ Start loop
0x49   │ NEXT     │ -          │ End loop
0x4A   │ BCC      │ skip (1B)  │ Branch if carry clear
0x4C   │ FINISH   │ -          │ End execution
0x6B   │ BRA      │ skip (1B)  │ Branch always
0x81   │ RSL      │ -          │ Rotate shift left
0x82   │ RSR      │ -          │ Rotate shift right
0x84   │ SUB      │ value (4B) │ Subtract 32-bit
0x87   │ EOR      │ value (4B) │ XOR 32-bit
0x93   │ ADD      │ value (4B) │ Add 32-bit
```

---

## Project Status

### Completed ✅

| Component | File(s) | Notes |
|-----------|---------|-------|
| PlatformIO scaffold | `platformio.ini`, `partitions.csv` | Debug + Release environments, OPI flash/PSRAM |
| Build automation | `build.bat`, `flash.bat`, `clean_flash.bat`, etc. | PowerShell backend with batch wrappers |
| WiFi AP | `wifi/wifi_manager.cpp` | Random SSID/password, mDNS (sefi.local) |
| Credential generator | `credentials/credential_generator.cpp` | Alphanumeric random credentials |
| Web server | `web/web_server.cpp` | AsyncWebServer, REST API, embedded UI serving |
| React UI | `ui/src/` (Preact + Tailwind + Vite) | File upload, status display, system info, branding |
| UI embedding | `ui/scripts/embed.js` | Gzip → C header generation |
| Firmware upload | `POST /api/upload` | Multipart to PSRAM, 10MB max |
| Upload clear | `DELETE /api/upload` | Free PSRAM buffer before re-upload (fragmentation fix) |
| Status API | `GET /api/status` | PSRAM, heap, uptime, clients, upload state |
| Serial commands | `main.cpp` | help, dev, status, creds, mem, reboot, crash |
| Dev reference | `DEV-INSTRUCTIONS.md` + `dev` serial cmd | Printable build/flash workflow for both projects |
| Shared theme | `shared-theme/` (6 files) | CSS vars, Tailwind preset, component library |
| UDS type definitions | `can/uds/uds_types.h` | Service IDs, NRC codes, session types, routines |
| CAN manager header | `can/can_manager.h` | Interface defined (begin, stop, send, receive) |
| Flash protocol analysis | `mdg1_flash_protocol_analysis.md` | Full capture decoded (in reference materials) |
| Hardware BOM | `OBDII-2025-12-31-4Gb.xlsx` | Parts specified |
| PCB design | `.f3z` / `.zofzproj` | Ready for fab |

### TODO ⬜ (ordered by dependency)

| # | Component | File(s) | Depends On | Notes |
|---|-----------|---------|------------|-------|
| 1 | **CAN Manager** | `can/can_manager.cpp` | — | TWAI driver: init, send, receive with timeout, filter |
| 2 | **ISO-TP Transport** | `can/uds/uds_messaging.cpp` | CAN Manager | SF/FF/CF/FC framing, 4095-byte reassembly |
| 3 | **SA2 Virtual Machine** | `can/uds/sa2_vm.cpp` | — | Bytecode interpreter (reference impl exists in G: drive) |
| 4 | **UDS Service Layer** | `can/uds/uds_manager.cpp` | ISO-TP, SA2 VM | Session, security, download, transfer, routines |
| 5 | **Flash Package Parser** | `flash/flash_package.cpp` | — | Parse .sefi header + index blocks from PSRAM |
| 6 | **Flash State Machine** | `can/uds/uds_flash.cpp` | UDS Manager, Package Parser | Orchestrate full flash sequence |
| 7 | **Flash API integration** | `web/web_server.cpp` | Flash State Machine | Connect POST /api/flash → state machine, progress via WebSocket |
| 8 | **PC Tool** | `tools/sefi_flasher.py` | — | ODX parser + package builder + upload + monitor |

### Future / Phase 2

| Feature | Description | Priority |
|---------|-------------|----------|
| NAND Storage | Store multiple firmwares on MX35LF4GE4AD | Medium |
| WebSocket progress | Real-time flash progress to UI | Medium |
| USB CDC handler | Alternative upload path via USB serial | Low |
| Multi-ECU | Support different CAN IDs / SA2 variants | Medium |
| Logging | Save flash logs to NAND | Low |

---

## TODO List (detailed)

### Phase 1: MVP (WiFi-Connected Flashing)

#### 1.1 CAN + UDS Stack (ESP32 firmware)

- [ ] **CAN Manager** (`can/can_manager.cpp`)
  - [ ] TWAI peripheral init (GPIO 17 TX, GPIO 18 RX, 500kbps)
  - [ ] Send frame with timeout
  - [ ] Receive frame with ID filter (0x7E8) and timeout
  - [ ] Error handling (bus-off recovery, error counters)
  - [ ] Start/stop lifecycle

- [ ] **ISO-TP Transport** (`can/uds/uds_messaging.cpp`)
  - [ ] Single Frame TX/RX (≤7 bytes)
  - [ ] First Frame + Consecutive Frame TX (multi-frame send)
  - [ ] Flow Control RX (parse BS/STmin from ECU)
  - [ ] Multi-frame RX (reassemble from ECU responses)
  - [ ] Timeout handling (N_Bs, N_Cr timers)

- [ ] **SA2 Virtual Machine** (`can/uds/sa2_vm.cpp`)
  - [ ] Bytecode interpreter (FOR/NEXT, BCC, BRA, FINISH)
  - [ ] Arithmetic ops (ADD, SUB, EOR, RSL, RSR)
  - [ ] Execute bytecode from PSRAM buffer
  - [ ] Return calculated key from seed
  - [ ] Port from reference implementation in G: drive

- [ ] **UDS Service Layer** (`can/uds/uds_manager.cpp`)
  - [ ] DiagnosticSessionControl (0x10) — Extended + Programming
  - [ ] SecurityAccess (0x27) — seed request + key send via SA2
  - [ ] RoutineControl (0x31) — erase, checksum verify, dependency check
  - [ ] RequestDownload (0x34) — negotiate transfer parameters
  - [ ] TransferData (0x36) — chunked data from PSRAM
  - [ ] RequestTransferExit (0x37) — finalize block
  - [ ] TesterPresent (0x3E) — keepalive on timer
  - [ ] ECUReset (0x11) — hard reset after flash
  - [ ] NRC handling + retry logic

#### 1.2 Flash Package + State Machine

- [ ] **Flash Package Parser** (`flash/flash_package.cpp`)
  - [ ] Validate .sefi header (magic, version)
  - [ ] Extract SA2 bytecode
  - [ ] Index block locations (pointers into PSRAM, no copy)
  - [ ] CRC32 verification per block

- [ ] **Flash State Machine** (`can/uds/uds_flash.cpp`)
  - [ ] Orchestrate: session → security → erase → download → verify → reset
  - [ ] Per-block loop with progress tracking
  - [ ] Error recovery / abort support
  - [ ] Progress reporting via callback

#### 1.3 Flash API Integration

- [ ] **Connect Web UI to Flash Engine**
  - [ ] `POST /api/flash` triggers state machine with PSRAM data
  - [ ] WebSocket or SSE for real-time progress updates
  - [ ] Flash stage + percent + current block in status API
  - [ ] Abort endpoint

#### 1.4 PC Tool (Python)

- [ ] **ODX Parser** — Extract SA2 bytecode, block definitions, checksums
- [ ] **Package Builder** — Combine encrypted .bin + ODX metadata → .sefi
- [ ] **Upload + Monitor** — HTTP upload to ESP32 + progress display

#### 1.5 Integration Test

- [ ] End-to-end test with bench ECU
- [ ] Verify timing compliance
- [ ] Error recovery testing
- [ ] Document protocol quirks

---

## Reference Documents

### In Repository

| Document | Location | Description |
|----------|----------|-------------|
| Dev Instructions | `DEV-INSTRUCTIONS.md` | Build/flash/debug workflows for both projects |
| Flash Protocol | `mdg1_flash_protocol_analysis.md` | Decoded capture |
| Hardware BOM | `OBDII-2025-12-31-4Gb.xlsx` | Bill of materials |
| PCB Design | `OBD-2025-12-31-4Gb.f3z` | Fusion 360 |

### External Reference (G: drive — read only)

| Path | Description |
|------|-------------|
| `G:\...\034\` | SA2 reference implementation, ODX files, CAN captures, encryption keys |

### Datasheets

| Resource | Description |
|----------|-------------|
| [ESP32-S3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf) | MCU specs |
| [ESP32-S3 TRM](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf) | Technical reference |
| [NCV7344 Datasheet](https://www.onsemi.com/pdf/datasheet/ncv7344-d.pdf) | CAN transceiver |
| [MX35LF4GE4AD](https://www.macronix.com/Lists/Datasheet/Attachments/8936/MX35LF4GE4AD,%203V,%204Gb,%20v1.0.pdf) | 512MB SPI NAND |

---

## Sample Firmware Binary Analysis

### File Details

| Field | Value |
|-------|-------|
| Filename | `RS7C8_SAMPLE_WUAPCBF28NN902533_4K0907557G__0003.bin` |
| File Size | 8,388,608 bytes (exactly 8.00 MB) |
| VW Part Number | **4K0907557G** |
| SW Version | **0003** |
| Bosch SW Number | **10SW099767** |
| ECU Platform | **MDG1** (MG1CS002) |
| Platform ID | `DMG1008PH2C1795_MA22G01` |
| ECU Variant | `EV_ECM40` |
| Calibration Version | `CB.06.043.0 FCG.00` |
| Firmware Version | `V08.08.00` |
| Vehicle | Audi RS7 C8, V8 4.0l TFSI |
| VIN (sample) | WUAPCBF28NN902533 |
| Diagnostic Address | J623 (Engine Control Module 2) |

### Binary Layout Overview

The `.bin` is a raw 8MB flash image. First `0x1C000` (112KB) bytes are all zeros (unused/reserved area before the first block). Each block starts with a `0xDEADBEEF` magic marker (little-endian: `EF BE AD DE`) at offset +0x000, followed by the Bosch software number `10SW099767` at offset +0x144.

Each block ends with a pair of RBA (Root of Trust / Block Authentication) digital signatures:
- `RBA1DGS1/VWAUDI5:CN00001` — DGS1 signature (RSA, ASN.1 encoded)
- `RBA4CC845215E4402C6E:PN00001` — PN signature
- Followed by `0x12345678` padding to the block boundary

### Block Map (Physical Layout in `.bin`)

The bin is a raw 8MB image. Blocks are NOT stored in UDS logical order — physical offset order differs from the ODX logical block numbering. The mapping was determined by matching block sizes between the bin and ODX metadata from multiple MG1 CS002IFX ODX files.

| Bin Offset | Bin End | Size | UDS Block | ODX Name | `is_calibration` | Entropy | Notes |
|------------|---------|------|-----------|----------|-------------------|---------|-------|
| `0x0001C000` | `0x0005FFFF` | 272 KB | **Block 5** (addr=0x05) | **DS0** (Data Set 0) | `true` | 0.58 / 8.0 | Calibration config/header. Low entropy = structured cal metadata |
| `0x00080000` | `0x001FFFFF` | 1.5 MB | **Block 6** (addr=0x06) | **ASW3 / DS1** | `false` ⚠️ | 3.49 / 8.0 | Contains MDG1 platform ID. 6th block only in CS002IFX (not in CS002). Needs confirmation — may be cal |
| `0x00200000` | `0x003FFFFF` | 2.0 MB | **Block 2** (addr=0x02) | **ASW0** | `false` | 6.48 / 8.0 | Application software — compiled TriCore code |
| `0x00400000` | `0x005FFFFF` | 2.0 MB | **Block 3** (addr=0x03) | **ASW1** | `false` | 6.07 / 8.0 | Application software — compiled TriCore code |
| `0x00600000` | `0x0062FFFF` | 192 KB | **Block 1** (addr=0x01) | **Cboot** | `false` | 4.36 / 8.0 | Boot block. Contains cal version strings as metadata (`PVAR : CB`) but is NOT calibration itself |
| `0x00630000` | `0x007FFFFF` | 1.8 MB | **Block 4** (addr=0x04) | **ASW2** | `false` | 6.71 / 8.0 | Application software |

> ⚠️ **Block 6 classification TBD**: This block only exists in CS002IFX (6 blocks) — the older CS002 has only 5 blocks. It may be a second calibration data set (DS1) or additional ASW. The exploitsettings.config for CS002 only lists through FD_05 (DS0). Needs validation against a CS002IFX capture or exploitsettings.

> **Key insight**: Physical bin offset order ≠ UDS logical block order. The UDS `RequestDownload` uses logical addresses (0x01–0x06), NOT bin offsets. The ECU bootloader maps logical → physical internally.

### Key String Locations

| Offset | String | UDS Block | Purpose |
|--------|--------|-----------|----------|
| `0x0001C144` | `10SW099767` | Block 5 (DS0) | Bosch software number (repeated in every block at +0x144) |
| `0x00080248` | `45/1/MDG1/5/P1795//DMG1008PH2C1795_MA22G01///` | Block 6 | ECU platform identification |
| `0x0015F8E6` | `4K0907557G` | Block 6 | VW part number (also at `0x0015F8FA`) |
| `0x0015F8D0` | `07309A  EV_ECM40` | Block 6 | ECU variant designation |
| `0x0015F920` | `V8 4.0l TFSI` | Block 6 | Engine description |
| `0x0015F930` | `J623` | Block 6 | Diagnostic address |
| `0x00600E42` | `MDG1  CB.06.043.0 FCG.00` | Block 1 (Cboot) | Cal version stored as Cboot metadata |
| `0x00600E60` | `PVAR : CB / 003_6.43.0; 0` | Block 1 (Cboot) | Cal variant (in Cboot, NOT in cal block) |
| `0x0061BA3A` | `V08.08.00` | Block 1 (Cboot) | Firmware version |
| `0x0005FC08` | `RBA1DGS1/VWAUDI5:CN00001` | (all blocks) | Block authentication signature |
| `0x0005FD40` | `RBA4CC845215E4402C6E:PN00001` | (all blocks) | Part number signature |

### SA2 Bytecode Analysis (from ODX cross-reference)

SA2 bytecodes vary by **hardware sub-variant**, NOT by individual part number or SW version.

| Hardware Variant | Block Count | SA2 Bytecode | Part Numbers Sharing This SA2 |
|-----------------|-------------|--------------|-------------------------------|
| MG1 CS002IFX | 6 | `6807870401201593050220164A03826B068193060320178407042018494C4C` | 8W0907559AB, 8W0907551, 8W0907551B, 4K0907557G |
| MG1 CS002IFX RS | 6 | (same as above) | 8W0907551 S0004/S0006 |
| MG1 CS002 (non-IFX) | 5 | `6807873107201493010820154A03826B068193020920168403102017494C4C` | 8W0907559H |

> **Confirmed**: Different SW versions of the same part number share identical SA2 bytecodes (e.g., 8W0907551 S0004 and S0006). SA2 is tied to hardware variant, not firmware revision.

> **Confirmed**: ALFID is `0131` across all checked MG1 variants.

> **Confirmed**: Encryption method is `2A` across all checked MG1 variants.

### Profile Architecture Implications

One profile per **hardware sub-variant** covers all part numbers and SW versions within that variant:

| Profile | Covers | Block Count | SA2 | Cal Blocks |
|---------|--------|-------------|-----|------------|
| `mg1_cs002ifx.json` | 8W0907559AB, 8W0907551, 8W0907551B, 4K0907557G, etc. | 6 | (shared) | Block 5 (+ Block 6 TBD) |
| `mg1_cs002.json` | 8W0907559H, 8W0907559G, etc. | 5 | (different) | Block 5 (DS0) |

### Implications for Cal-Only Flashing

When `flash_mode == CAL_ONLY` (CS002IFX, 6-block layout):
- **Flash**: Block 5 (DS0, 272KB) — confirmed calibration
- **Possibly also**: Block 6 (1.5MB) — TBD, may be second cal dataset
- **Skip**: Blocks 1, 2, 3, 4 (Cboot + ASW)
- **Time savings**: 75–97% reduction vs full flash (depending on Block 6 classification)

### Gap Regions

| Region | Size | Content |
|--------|------|---------|
| `0x000000` – `0x01BFFF` | 112 KB | All zeros (pre-block padding) |
| `0x060000` – `0x07FFFF` | 128 KB | All zeros (gap between Block 5 and Block 6 in bin) |

---

## Changelog

| Date | Version | Changes |
|------|---------|---------|
| 2026-02-05 | 0.4 | Added Sample Firmware Binary Analysis section — full block map, cal/app classification, key string locations from RS7C8 sample .bin |
| 2026-02-05 | 0.3 | Updated status: WiFi+WebUI+Upload working, PSRAM clear fix, shared theme, dev instructions, full TODO reorder by dependency |
| 2026-02-02 | 0.2 | Simplified to upload-to-PSRAM architecture |
| 2026-02-02 | 0.1 | Initial document |
