# FUTUNER ECU Boxcode Support Matrix

> Generated: 2026-04-05
> Sources: firmware source, ecu_variable_db.json, HW/boxcodes/*.md, USB drive (Extreme SSD)

---

## Summary

| Metric | Count |
|--------|-------|
| **Fully supported** (in firmware, logger + flash) | 4 |
| **Partially supported** (in firmware, minimal vars) | 0 (4M0906014__0005 has only 4 vars but is in firmware) |
| **Identified only** (known boxcode, not in firmware) | 32 |
| **Total unique boxcodes** | 36 |
| **ECU families covered** | MG1CS001, MG1CS002, MG1CS002IFX, MG1CS002IFX RS, MG1CS008IFX, MG1CS011, MD1CP004IFX, MD1CP014IFX |

---

## Fully Supported Boxcodes (in logger_variables.c)

### 1. 4K0907557G__0003 -- PRIMARY

| Field | Value |
|-------|-------|
| Vehicle | Audi RS7 C8 4.0L V8 TFSI |
| ECU | Bosch MG1CS002IFX (ECM40TFS) |
| Endianness | Little-endian |
| Write mid byte | 0x80 |
| Write offset | 0x000000 (direct) |
| Ethanol addr | 0x11E6AE |
| Speed display addr | 0x09F93E |
| Logger vars (firmware) | 23 |
| Logger vars (full DB) | 52 (ecu_variable_db.json) |
| A2L | DMG1008PH2C1795_MA22G01_Out.a2l |
| XDF files | 4 (full + stagex + SRM + modified) |
| BIN dumps | 13 (1 stock + 12 tuned iterations) |
| BDEF | 4K0907557G__0003.bdef |
| SCAL files | 5 (scal2-scal7) |
| AES key | MG1CS002 key |
| Status | **Fully supported** |

### 2. 8W0907559H__0008

| Field | Value |
|-------|-------|
| Vehicle | Audi B9/C8 (S4/S5/A6/A7 3.0 TFSI) |
| ECU | Bosch MG1CS002IFX |
| Endianness | **Big-endian** (only BE boxcode) |
| Write mid byte | 0x09 |
| Write offset | 0x040000 |
| Ethanol addr | 0x6A8550 |
| Speed display addr | 0x651F30 |
| Logger vars | 9 |
| Unique vars | gang, frm/frm2 per-bank, lambts/lambts2 |
| AES key | MG1CS002 key |
| Status | **Fully supported** |

### 3. 4M0906014__0005

| Field | Value |
|-------|-------|
| Vehicle | Porsche Cayenne / Bentley Bentayga / Lamborghini Urus 4.0T |
| ECU | Bosch MDG1 |
| Endianness | Little-endian |
| Write mid byte | 0x80 |
| Write offset | 0x000000 (direct) |
| Ethanol addr | 0x11E46A |
| Speed display addr | 0x09F90E |
| Logger vars | 4 (minimal -- RPM, ethanol, load, cruise only) |
| Status | **Partially supported** (needs more vars) |

### 4. 4M0906014B__0003

| Field | Value |
|-------|-------|
| Vehicle | Porsche Cayenne / Bentley Bentayga / Lamborghini Urus 4.0T |
| ECU | Bosch MDG1 (MG1CS008IFX) |
| Endianness | Little-endian |
| Write mid byte | 0x80 |
| Write offset | 0x000000 (direct) |
| Ethanol addr | 0x11E6C6 |
| Speed display addr | 0x091A70 |
| Logger vars | 10 |
| BIN dump | Stage 1 SQ7 93oct Rev1.4 bin available |
| Status | **Fully supported** |

---

## Identified Boxcodes (Not Yet in Firmware)

### MG1CS002 Family (Big-Endian, 3.0 TFSI)

| Boxcode | Vehicle | BIN | A2L | ODX/Exploit | Notes |
|---------|---------|-----|-----|-------------|-------|
| 8W0907559H__0009 | Audi S4 NA MY2018 | Yes (FRF) | No | Listed in config | Sibling of __0008 |
| 8W0907559H__0005 | Audi S4/S5 NA | Yes (extract) | No | ODX + FRF | .enc sample exists |
| 8W0907559H__0004 | Audi S4/S5 NA | Yes (extract) | No | In .testignore | |
| 8W0907559G__0011 | Audi S4 EURO MY2018 | Yes (FRF) | No | Listed in config | Euro market |
| 8W0907559G__0008 | Audi S4/S5 | Yes (extract) | **Yes** | In .testignore | A2L available |
| 8W0907559K__0002 | Audi B9/C8 | No | No | Test exploit ODX | |
| 8W0907559S__0002 | Audi B9/C8 | No | No | Test exploit ODX | |
| 8W0907559AB__0002 | Audi B9/C8 (RS4/RS5?) | No | No | Test exploit ODX | Extensive SEAN-SEFI cache |
| 8W0907559AD__0001 | Audi B9/C8 | Yes | No | Ghidra memmap | MG1CS002IFX |
| 80A907559C__0005 | Audi SQ5 | No | No | Listed in config | Revisions S0004-S0007 |
| 80A907559C__0007 | Audi SQ5 | Yes (stock + stage1) | No | Autotuner | Stage 1+ E85 tune |
| 80A907559P__0001 | Audi B9/C8 | No | No | Listed in config | MG1CS002IFX |
| 80A907559G__0002 | Audi SQ5 | No | No | Test exploit ODX | Flash container extract |
| 80A907559M__0002 | Audi SQ5 | No | No | Test exploit ODX | |
| 80A907559B__0004 | Audi B9/C8 | No | No | In .testignore | Also S0005 |
| 80A907559D__0002 | Audi B9/C8 | No | No | In .testignore | |

### MG1CS002 RS / IFX RS Family (Big-Endian, 8W0907551x)

| Boxcode | Vehicle | BIN | ODX/Exploit | Notes |
|---------|---------|-----|-------------|-------|
| 8W0907551__0004 | Audi RS (B9/C8) | No | Test exploits S0002-S0007 | MG1CS002IFX RS |
| 8W0907551A__0002 | Audi RS (B9/C8) | No | Test exploits S0002-S0004 | |
| 8W0907551B__0004 | Audi RS (B9/C8) | No | Test exploit | |
| 8W0907551D__0003 | Audi RS (B9/C8) | No | Test exploit | |
| 8W0907551F__0002 | Audi RS (B9/C8) | No | Test exploit | |
| 8W0907551G__0004 | Audi RS (B9/C8) | No | Test exploit | |
| 8W0907551J__0003 | Audi RS (B9/C8) | No | Test exploit | |

### MG1CS008IFX Family (Little-Endian, 4.0T V8)

| Boxcode | Vehicle | BIN | Notes |
|---------|---------|-----|-------|
| 4K0907557D__0003 | Audi RS6/RS7 C8 | No | Related to 4K0907557G |
| 4M8907557B__0004 | Bentley/Lamborghini 4.0T | No | 4M8 prefix |

### MD1 Family

| Boxcode | Vehicle | ECU | BIN | AES Key | Notes |
|---------|---------|-----|-----|---------|-------|
| 4K0907401__0004 | Audi RS6/RS7 | MD1CP004IFX | Yes (bench) | MD1CP004 key | Dual config listing |
| 4M0997409__1005 | Porsche/Bentley/Lambo | MD1CP014IFX | No | MD1CP014 key | Protocol v1.15 |

### MG1CS001 Family (Little-Endian, MQB 2.0T/1.5T)

| Boxcode | Vehicle | ECU | BIN | A2L | Notes |
|---------|---------|-----|-----|-----|-------|
| 8V0907115C__0002 | Golf R/S3 2.0 TFSI | MG1CS001 | No | **Yes** | ODX available |
| 8W0907115C__0006 | Audi B9 | MG1CS001 Flexray | Yes (test mod) | No | |
| 5NA907115A__0004 | VW Tiguan 2.0 TSI | MG1CS001 | Yes (stock + mod) | No | |
| 06L907309B | VW Tiguan 2.0 TSI | MG1CS001 | No | No | BDC def only |

### MG1CS011 Family (Little-Endian, EA211 1.5T)

| Boxcode | Vehicle | ECU | BIN | Notes |
|---------|---------|-----|-----|-------|
| 05E906018R__9832 | VW Golf/Polo 1.5 TSI | MG1CS011 | Yes | Unique AES IV |

---

## AES Encryption Keys

| ECU Variant | Key (hex) | IV (hex) |
|-------------|-----------|----------|
| MG1 generic | C7 12 B1 F1 4B 31 AD C1 FD 33 04 D0 FB D6 DE 6B | 00..0F |
| MG1CS002 | 83 41 C1 ED 72 CD C2 5F 9B AF 7A EA 94 61 77 EF | 00..0F |
| MG1CS011 | 6D A9 5B 9D C4 C2 F9 8B 5C 00 A3 04 A9 6A 1F 96 | 6D C9 5D 2E 09 3A DD 59 10 D1 36 7B 7F F5 A0 2B |
| MD1CP004 Type1 | 41 45 53 2D 44 65 66 61 75 6C 74 2D 4B 65 79 32 | 00..0F |
| MD1CP004 Type2 | 2B D2 A3 53 C3 59 35 D5 E1 7C 80 B8 9E 90 7B 7B | 00..0F |
| MD1CP014 | 0F 78 AE 6A FA 92 23 3B 71 8F 2C 13 85 D3 11 3A | 00..0F |

---

## Flash Protocol (Common to All MDG1/MG1)

- **UDS services**: SecurityAccess(0x27), RequestDownload(0x34), TransferData(0x36), TransferExit(0x37), ECUReset(0x11)
- **Encryption**: AES-128-CBC (key per ECU variant)
- **Compression**: LZRB (custom LZ-based, Bosch-specific)
- **CRC**: DEADBEEF marker scan, header CRC32, per-block checksums (ADD8, ADD16, ADD32, CRC32)
- **Flash blocks (MG1 CS002)**: CBOOT @ 0x08FFF800, ASW0 @ 0x0933F800, ASW1 @ 0x095FF800, ASW2 @ 0x08FCF800, DS0 @ 0x0977F800
- **SA2 bytecode**: 6807870401201593050220164A03826B068193060320178407042018494C4C
- **Security key algorithm**: Placeholder in firmware (seed + 0x12345678); real algorithm ECU-specific

---

## Key File Locations

| Resource | Path |
|----------|------|
| Firmware variable definitions | /Users/rabbit/esp/obd/SEFIv1/src/logger/logger_variables.c |
| Extended variable DB (4K0 only) | /Users/rabbit/esp/obd/SEFIv1/ecu_variable_db.json |
| Boxcode documentation | /Users/rabbit/esp/obd/SEFIv1/HW/boxcodes/*.md |
| Flash protocol (UDS) | /Users/rabbit/esp/obd/SEFIv1/src/flash/mdg1_flash.c |
| CRC tool | /Users/rabbit/esp/obd/SEFIv1/src/flash/mdg1_crc.c |
| A2L (4K0907557G) | /Users/rabbit/esp/obd/SEFIv1/DMG1008PH2C1795_MA22G01_Out.a2l |
| A2L (8V0907115C) | /Volumes/Extreme SSD/.../MG1 CS001/X03_8V0907115_C_0002g.A2L |
| A2L (8W0907559G__0008) | /Volumes/Extreme SSD/.../MG1 CS002/.testignore/8W0907559G S0008/Reference/I36_8W0907559G_0008g.A2L |
| BDEF/SCAL | /Users/rabbit/esp/obd/SEFIv1/ALBIN/ALBIN_WIP/ALBIN_WIP/ |
| XDF files | /Volumes/Extreme SSD/GitHub/c8-rs7-dev/*.xdf |
| Exploit configs | /Volumes/Extreme SSD/VAG MDG1/034/Unzipped/Bosch ECM/Motronic/MG1/*/exploitsettings.config |
| Test exploit ODX | /Volumes/Extreme SSD/VAG MDG1/034/Unzipped/Bosch ECM/Motronic/MG1/Tests/ |
| Reference paths doc | /Volumes/Extreme SSD/VAG MDG1/VAG-MDG1-Reference-Paths.md |
| MED17-MG1 crossref | /Users/rabbit/esp/obd/SEFIv1/HW/boxcodes/MED17_MG1_CROSSREF.md |

---

## Next Steps for Expanding Support

1. **Easiest wins**: 8W0907559G__0008 has both an A2L and bin -- extract RAM addresses for all variables
2. **Sibling boxcodes**: 8W0907559H__0009 is a revision of the fully-supported __0008; likely same addresses
3. **80A907559C family**: Multiple bins available; use DEADBEEF scan to determine endianness and block structure
4. **4K0907557D__0003**: Same part number family as the primary boxcode; address deltas likely small
5. **MG1CS001 expansion**: A2L for 8V0907115C available -- enables MQB platform support
6. **8W0907559AB**: Extensive live logging data in SEAN-SEFI cache; addresses already captured
