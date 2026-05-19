# MG1 / MDG1 ECU Flashing Research — Part 1: File Catalog & Architecture Reference

**Purpose:** Reference document for developing an ESP32-based bench flashing device for Bosch MG1/MD1 family ECUs (VAG platform).

**Base Path:** `C:\Users\buttplug\Documents\GitHub\VAG MDG1\034\Unzipped\Bosch ECM\Motronic\MG1\`

---

## 1. ECU Variant Summary

The collection spans **11 subdirectories** covering two major ECU families across two microcontroller architectures.

| Directory | ECU Family | MCU Architecture | CAN Bus | Application | Exploit Config Part Number |
|---|---|---|---|---|---|
| `MG1 CS001` | MG1CS001 | NXP MPC5777C (PowerPC) | Standard CAN | VW/Audi 2.0T EA888 Gen3 | `8V0907115C 0002` |
| `MG1 CS001 Flexray` | MG1CS001 | NXP MPC5777C (PowerPC) | FlexRay + CAN | Audi A4/A5 2.0T (B9 platform) | `8W0907115C 0006` |
| `MG1 CS002` | MG1CS002 | NXP MPC5777C (PowerPC) | Standard CAN | Audi S4/S5/SQ5 3.0T EA839 (CWGD) | `8W0907559H 0009` / `8W0907559G 0011` / `80A907559C 0005` |
| `MG1 CS002 (Autotuner)` | MG1CS002 | NXP MPC5777C (PowerPC) | Standard CAN | Autotuner flash extracts, 3.0T | `80A907559C 0007` |
| `MG1 CS002 RS` | MG1CS002 | NXP MPC5777C (PowerPC) | Standard CAN | Placeholder (empty apart from config) | `4K0907401 0004` |
| `MG1 CS002IFX` | MG1CS002 | Infineon TriCore TC2xx | Standard CAN | Audi S4/S5 3.0T (Infineon refresh) | `80A907559P 0001` |
| `MG1 CS002IFX RS` | MG1CS002 | Infineon TriCore TC2xx | Standard CAN | Audi RS3/TTRS 2.5T (EA855 DAZA/DNWA) | `8W0907551 0004` |
| `MG1 CS008IFX` | MG1CS008 | Infineon TriCore TC2xx | Standard CAN | Audi SQ7/SQ8 4.0T | `4K0907557D 0003` / `4M8907557B 0004` |
| `MG1 CS011` | MG1CS011 | (Likely NXP MPC5777) | Standard CAN | VW 1.5T EA211 EVO | `05E906018R 9832` |
| `MD1 CP004IFX` | MD1CP004 | Infineon TriCore TC2xx | Standard CAN | Audi 2.0T (B9.5+) | `4K0907401 0004` |
| `MD1 CP014IFX` | MD1CP014 | Infineon TriCore TC2xx | Standard CAN | Audi RS6/RS7 4.0T (C8) | `4M0997409 1005` |

### Key Architectural Split

- **NXP MPC5777C (PowerPC e200z7):** MG1 CS001, CS001 Flexray, CS002, CS002 RS, CS011. Full read files are 8.00 MB (8,388,608 bytes). BDC backup files are ~8.25 MB.
- **Infineon TriCore TC2xx (AURIX):** All "IFX" variants — CS002IFX, CS002IFX RS, CS008IFX, MD1 CP004IFX, MD1 CP014IFX. Full read files are also 8.00 MB.

**This distinction is critical for the ESP32 tool** — the boot sequence, flash command set, and memory protection mechanisms differ entirely between MPC5777C and TriCore.

---

## 2. File Type Reference

### 2.1 Binary Flash Dumps (`.bin`)

**Purpose:** Raw flash memory dumps — the actual ECU firmware as read from the chip.

| Pattern | Meaning | Size | ESP32 Relevance |
|---|---|---|---|
| `PARTNO SXXXX, Original.bin` | Stock OEM flash dump | 8.00 MB | **Read/Write target** — this is what you read from and write to the ECU |
| `PARTNO SXXXX, MOD.bin` | Modified (tuned) flash dump | 8.00 MB | Write target for flashing tunes |
| `PARTNO SXXXX, [Flash Container Extract].bin` | Extracted from FRF/ODX container | 8.00 MB | Same format as direct read, usable as write source |
| `*bench.bin` | Bench (off-car) flash dump | 8.00 MB | Bench-specific read — confirms ESP32 bench approach |
| `*_TEST_EXPLOIT.odx` | Test exploit container | Varies | Testing artifacts, not direct flash targets |

**Naming convention decoded:**
- `8W0907559H` = VAG part number (ECU hardware)
- `S0009` = Software version (leading S, 4-digit zero-padded)
- `0009` = Same software version (without S prefix in some contexts)

### 2.2 Flash Containers (`.frf`)

**Purpose:** VAG factory flash update containers — compressed, signed packages used by ODIS/VAS diagnostic tools.

| Example | Size | Notes |
|---|---|---|
| `FL_8W0907559H_0009__V001.frf` | 2.8–3.5 MB | Compressed firmware + metadata |
| `*.frf.skip` | Same | Marked to skip during batch processing |

**Structure:** FRF files contain ODX-wrapped flash data with VW/Audi security signatures. They must be unpacked/decrypted to extract the raw .bin calibration data. The ESP32 tool would NOT flash FRF files directly — they are a source for extracting clean stock .bin images.

### 2.3 ODX Flash Data (`.odx`, `.odx.bin`)

**Purpose:** Open Diagnostic data eXchange format — intermediate between FRF containers and raw bin.

| Extension | Meaning |
|---|---|
| `.odx` | ODX XML container with flash data blocks |
| `.odx.bin` | Extracted binary payload from ODX container |

**ESP32 Relevance:** The `.odx.bin` files are essentially pre-extracted flash images and can be used as write sources identical to `.bin` files.

### 2.4 A2L Calibration Maps (`.A2L`)

**Purpose:** ASAP2 measurement and calibration description files — they define every tunable parameter address, scaling, and data type in the ECU firmware.

| File | ECU | Size | Project ID |
|---|---|---|---|
| `X03_8V0907115_C_0002g.A2L` | MG1CS001 (2.0T) | 34.11 MB | `DMG1001A01C1394` "MG1CS001" |
| `20A31_1Flut_P1793 (IFX).a2l` | MG1CS002IFX RS (2.5T RS) | (large) | `DMG1002AH2C1793_MA20A31` |
| `I36_8W0907559G_0008g.A2L` | MG1CS002 (3.0T S4/S5) | (reference) | (In .testignore) |

**ESP32 Relevance:** Not directly needed for flashing, but **essential for calibration tool development.** The A2L defines:
- XCP (Universal Measurement and Calibration Protocol) connection parameters
- Memory segment addresses for each calibration area
- Parameter addresses, data types, conversion formulas

### 2.5 Intel HEX Files (`.hex`)

| File | ECU | Size |
|---|---|---|
| `X03_8V0907115_C_0002g.hex` | MG1CS001 (2.0T) | 18.99 MB |

**Purpose:** Address-tagged firmware image. Contains the same data as the .bin but with explicit address records (`:AAAAAATT` format). Useful for verifying memory map alignment.

### 2.6 BDC Backup Files (`.bdc`)

**Purpose:** Proprietary backup container format (likely from a specific bench tool — BitSuit/BFlash style).

| File | Size | Notes |
|---|---|---|
| `Tiguan 06L907309B.bdc` | 8.25 MB | Slightly larger than raw flash — includes metadata/header |
| `3.0T_BenchECU_StockRead.bdc` | (in CS002) | Bench ECU backup |
| `B9A4GTron_Stock.bdc` | (in CS001 Flexray) | B9 A4 G-Tron backup |

**ESP32 Relevance:** These contain full flash dumps with additional metadata. Would need parsing to extract the raw 8MB image if used as source data.

### 2.7 Ghidra Memory Map (`.json`)

| File | Location |
|---|---|
| `ZTF_Ghidra_Memorymap_8W0907559AD0001_regs.json` | `MG1 CS002IFX\8W0907559AD S0001\Original\` |

**Purpose:** 21,009-entry register map for the Infineon TriCore TC2xx as used in MG1CS002IFX. Defines every hardware peripheral register address and size.

**Key modules identified for ESP32 communication:**

| Module | Base Address (hex) | Purpose |
|---|---|---|
| `MODULE_CAN` | `0xF0218000` | MCAN controller (primary diagnostic CAN) |
| `MODULE_CANR` | `0xF0228000` | Second CAN controller |
| `MODULE_ASCLIN0–N` | `0xF0000500+` | Async/sync serial (UART/LIN/SPI) |
| `MODULE_QSPI0–5` | `0xF0001C00+` | SPI controllers (external flash, sensors) |
| `MODULE_FLASH0` | `0xF8001000` | Flash controller (PFLASH/DFLASH management) |
| `MODULE_PMU0` | `0xF8000000` | Power Management Unit |
| `MODULE_LMU` | `0xF8700000` | Local Memory Unit (SRAM) |
| `MODULE_EMEM` | `0xF9060000` | Extended Memory (EMEM) |
| `FCE0` (CRC engine) | `0xF0003C00` | Hardware CRC-32 — used for flash block checksums |

### 2.8 Reference Memory Extracts

| File | Address Range | Size |
|---|---|---|
| `ExtractData_0x60100000-0x60107FFF.bin` | `0x60100000–0x60107FFF` | 32 KB |
| `ExtractData_0xC0000000-0xC0007FFF.bin` | `0xC0000000–0xC0007FFF` | 32 KB |

These are small extracts from specific memory regions of the IFX ECU — `0x60100000` is in the EMEM/cached flash region, `0xC0000000` is in the DFLASH (data flash) region. Useful for understanding security/HSM key storage areas.

---

## 3. Flash Memory Layout (MG1 CS002 — MPC5777C)

The `exploitsettings.config` for MG1 CS002 contains the most detailed flash structure:

```
#FlashTags
1.18,FD_01,0x08FFF800,0xD8,FlashRecord_Cboot
1.18,FD_01,0x08FFFFF8,0x4,Checksum
1.18,FD_02,0x0933F800,0xD8,FlashRecord_ASW0
1.18,FD_02,0x0933FFF8,0x4,Checksum
1.18,FD_03,0x095FF800,0xD8,FlashRecord_ASW1
1.18,FD_03,0x095FFFF8,0x4,Checksum
1.18,FD_04,0x08FCF800,0xD8,FlashRecord_ASW2
1.18,FD_04,0x08FCFFF8,0x4,Checksum
1.18,FD_05,0x0977F800,0xD8,FlashRecord_DS0
1.18,FD_05,0x0977FFF8,0x4,Checksum
```

### Flash Block Definitions

| Flash Descriptor | Address | Size | Content | Writable? |
|---|---|---|---|---|
| `FD_01` Cboot | `0x08FFF800` | 0xD8 (216 bytes) record + 4-byte checksum | **Customer Bootloader** — do NOT overwrite | ⚠️ DANGER |
| `FD_02` ASW0 | `0x0933F800` | 0xD8 record + 4-byte checksum | Application Software Block 0 (main code) | ✅ Yes |
| `FD_03` ASW1 | `0x095FF800` | 0xD8 record + 4-byte checksum | Application Software Block 1 | ✅ Yes |
| `FD_04` ASW2 | `0x08FCF800` | 0xD8 record + 4-byte checksum | Application Software Block 2 | ✅ Yes |
| `FD_05` DS0 | `0x0977F800` | 0xD8 record + 4-byte checksum | **Dataset 0 (Calibration data)** — primary tuning target | ✅ Yes |

### Flash Record Structure (0xD8 = 216 bytes each)

Each flash record at the end of a block contains:
- Block ID / validation signature
- Software version identifier
- CRC/checksum of the block contents
- Flash erase/write counters
- Validity flags

The 4-byte checksum at `offset + 0x7F8` from each record is a CRC-32 of the entire flash block.

**For the ESP32 tool:** After writing any block, the corresponding FlashRecord and Checksum must be recalculated, or the ECU will reject the flash and fail to boot.

---

## 4. Exploit Settings Config Format

The `exploitsettings.config` file in each variant directory specifies the **default part number** used for exploit/flash operations on that ECU variant.

| Config Value | Format | Meaning |
|---|---|---|
| `8V0907115C 0002` | `PARTNO SWVER` | Part number + software version for exploit seed/key |
| `1.18` (in FlashTags) | Version prefix | Flash protocol version |
| `1.15` (MD1 CP014IFX) | Version only | Likely indicates a different protocol revision |

**For the ESP32 tool:** The part number from the config determines which security seed/key algorithm and which UDS (Unified Diagnostic Services) session parameters to use for flash access.

---

## 5. File Path Quick Reference for ESP32 Tool Development

### Stock Flash Images (Write Sources)

```
# MG1 CS001 — 2.0T EA888
MG1 CS001\5NA907115A S0004, Original.bin
MG1 CS001\FL_8V0907115C_0002__V001.odx.bin

# MG1 CS001 Flexray — 2.0T B9 A4
MG1 CS001 Flexray\8W0907115C S0006, Testing-Mod Test.bin

# MG1 CS002 — 3.0T S4/S5/SQ5
MG1 CS002\8W0907559G S0011\Original\Audi S4 EURO MY2018 3.0TFSI CWGD 8W0907559G S0011 (frf).bin
MG1 CS002\8W0907559H S0009\Original\Audi S4 NA MY2018 3.0TFSI CWGD 8W0907559H S0009 (frf).bin

# MG1 CS002IFX — 3.0T IFX Refresh
MG1 CS002IFX\8W0907559AD S0001\Original\8W0907559AD 0001.bin

# MG1 CS002IFX RS — 2.5T RS3/TTRS
MG1 CS002IFX RS\8W0907551 S0004\Original\8W0907551 S0004, Original.bin
MG1 CS002IFX RS\.testignore\8W0907551B S0004\8W0907551B S0004, Original.bin

# MD1 CP004IFX — 2.0T B9.5+
MD1 CP004IFX\4K0907401 0004 MD1CP004 bench.bin

# MD1 CP014IFX — 4.0T RS6/RS7
MD1 CP014IFX\FL_4M0997409_1005__V001.odx.bin
```

### Calibration / Map Definition Files

```
# MG1 CS001 A2L (XCP + full parameter map)
MG1 CS001\X03_8V0907115_C_0002g.A2L

# MG1 CS001 HEX (address-tagged firmware)
MG1 CS001\X03_8V0907115_C_0002g.hex

# MG1 CS002IFX RS A2L
MG1 CS002IFX RS\.testignore\8W0907551F S0002\20A31_1Flut_P1793 (IFX).a2l

# MG1 CS002 A2L (reference only)
MG1 CS002\.testignore\8W0907559G S0008\Reference\I36_8W0907559G_0008g.A2L
```

### Hardware / Memory Reference Files

```
# Ghidra register map (TriCore TC2xx — IFX variants)
MG1 CS002IFX\8W0907559AD S0001\Original\ZTF_Ghidra_Memorymap_8W0907559AD0001_regs.json

# DFLASH / EMEM security region extracts
MG1 CS002IFX\8W0907559AD S0001\.testignore\Reference\ExtractData_0x60100000-0x60107FFF.bin
MG1 CS002IFX\8W0907559AD S0001\.testignore\Reference\ExtractData_0xC0000000-0xC0007FFF.bin
```

### Pinout / Board Images

```
MG1 CS001\mg1cs001.png
MG1 CS001 Flexray\.testignore\mg1cs001.png
```

---

## 6. Next Steps (Future Research Parts)

**Part 2 — Communication Protocols:**
- UDS (ISO 14229) session management for MG1/MD1
- Security Access (Seed/Key) algorithm details
- Flash programming sequence (RequestDownload → TransferData → TransferExit)
- CAN vs FlexRay considerations for ESP32

**Part 3 — ESP32 Hardware Design:**
- CAN transceiver selection (MCP2515/SN65HVD230)
- Bench wiring diagrams (power, K-line, CAN-H/CAN-L)
- MPC5777C vs TriCore boot mode pin requirements for bench flash

**Part 4 — Flash Block Parsing:**
- Binary structure of the 8MB flash images
- Block boundary detection
- Checksum algorithms (CRC-32 variants)
- FlashRecord validation and recalculation

**Part 5 — FRF/ODX Container Parsing:**
- Extracting raw .bin from .frf containers
- ODX XML structure and data block offsets
- VW/Audi signature handling

---

*Document generated: 2026-02-05 | Part 1 of N | Context-safe chunk*
