# ECU AES-128 key fingerprint inventory — 2026-05-12

_Source: pre-scan of `/Users/rabbit/034_local` (the SanDisk-mirrored 034 archive)._

_Generated: 2026-05-12T07:30:00_


## Headline numbers

- `.bin` files scanned: **3,521**
- Bins where the entropy + repeated-byte filter accepts exactly one offset (key location resolvable): **3,059**
- Bins where neither offset (0x18200 / 0x600200) is acceptable: **462** (likely non-bootloader bins or different families)
- Unique key fingerprints across the classifiable set: **53**
- Of those, already in `secrets/AES_KEYS_MASTER.md`: **5**
- **NEW fingerprints (not in our key table today)**: **48** ← the actionable expansion

## Known-key cross-reference

| sha256[:8] | Known key name |
|---|---|
| `251194eb` | MG1 generic |
| `29fd0bf7` | MG1CS011 |
| `51866eb2` | MD1CP004 T2 |
| `51ffa96a` | MD1CP014 |
| `a65a5bb0` | MD1CP004 T1 |
| `f57f3534` | MG1CS002 |

## Full fingerprint table

Ordered by bin count (most-frequent first). KNOWN entries are flagged; the rest are NEW and need key bytes sourced (RE, sibling project, or bench extract).

| sha256[:8] | Offset | Bins | Status | Inferred family | Sample path |
|---|---|---|---|---|---|
| `f57f3534` | `0x600200` | 2827 | ✅ KNOWN (MG1CS002) | mg1 cs002 | 2.9TT B9 MG1/8W0907551 S0008/Original/8W0907551 0008.bin |
| `0e2cad79` | `0x18200` | 27 | 🆕 NEW | mex9 | Bosch/Motronic/MEx9/MED 9.1.2 Dual ECU/420910552L-0261S02817-1037509722-0010.BIN |
| `251194eb` | `0x18200` | 26 | ✅ KNOWN (MG1 generic) | mg1 cs001 | Bosch/Motronic/MG1/MG1 CS001 Flexray/.testignore/8W0907115C 0006 - MOD.bin |
| `d6f4b42a` | `0x18200` | 16 | 🆕 NEW | mex17 | Bosch/Motronic/MEx17/UDS/MED 17.5.2/04E906016F S5760, Original.bin |
| `f5d0d2c3` | `0x18200` | 12 | 🆕 NEW | mex17 | Bosch/Motronic/MEx17/UDS/MED 17.1.1 Dual ECU/4S0907552F 0003.bin |
| `72e22b78` | `0x18200` | 10 | 🆕 NEW | mex17 | Bosch/Motronic/MEx17/TP2/MED 17.5.2 TP2/2.0TFSI 06J906027FJ S4352 Stock Compl... |
| `b0cb819b` | `0x18200` | 10 | 🆕 NEW | mex17 | Bosch/Motronic/MEx17/UDS/EDC 17 C74/04L906026JF S4137/04L906026JF S4137, Orig... |
| `66678bb9` | `0x18200` | 9 | 🆕 NEW | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/1 - Flashed to Sector-0x1_... |
| `664419e3` | `0x18200` | 8 | 🆕 NEW | mg1 cs002 | Bosch/Motronic/MG1/MG1 CS002 (Autotuner)/8W0907559G S0008/8W0907559G S0008, [... |
| `b7d39dab` | `0x18200` | 6 | 🆕 NEW | tcu | Bosch TCU/DQ/DSG DQ38x G2/0GC906557AM 4162  - Original BENCH A46E .bin |
| `5f0731e8` | `0x18200` | 6 | 🆕 NEW | tcu | Bosch TCU/DQ/DSG DQ500/0DL300012H S2110, [Flash Container Extract].bin |
| `b6267173` | `0x18200` | 6 | 🆕 NEW | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x1_2_DECODED.bin |
| `5c62f84d` | `0x18200` | 5 | 🆕 NEW | unknown | Damos and Defs/Bosch ME7.5/2C2SFGB3/2C2SFGB3.BIN |
| `2fc78b3a` | `0x18200` | 4 | 🆕 NEW | unknown | Damos and Defs/Bosch ME7.5/8N0906018AM_0261207417_1037360844.bin |
| `08829054` | `0x18200` | 4 | 🆕 NEW | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMod/10-0x80800100-_RAW... |
| `12f6fefd` | `0x18200` | 4 | 🆕 NEW | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMod/12-0x80980100-_RAW... |
| `3402b3f1` | `0x18200` | 4 | 🆕 NEW | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMod/8-0x80500100-_RAW.bin |
| `e05643d3` | `0x18200` | 4 | 🆕 NEW | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x5_2_RAW.bin |
| `bcd4a935` | `0x18200` | 3 | 🆕 NEW | unknown | 2.9TT B9 MG1/8W0907551D S0002/Original/Customer Backup for ORI/20220426-12042... |
| `d8191fbd` | `0x18200` | 3 | 🆕 NEW | tcu | Bosch TCU/DQ/DSG DQ38x G2 (Cal Only VR)/0GC906557M 3562.bin |
| `7971a7e5` | `0x18200` | 3 | 🆕 NEW | tcu | Bosch TCU/DQ/DSG DQ38x G2 (Cal Only VR)/0GC906557P 3560.bin |
| `44985e85` | `0x18200` | 3 | 🆕 NEW | tcu | Bosch TCU/DQ/DSG DQ500 (CMD File)/0DL300012H S2112, Original.bin |
| `a614dd18` | `0x18200` | 3 | 🆕 NEW | unknown | Damos and Defs/Bosch ME7.1/ME7.5.bin |
| `7105af75` | `0x18200` | 3 | 🆕 NEW | unknown | Dokumentation/Fresh dumps/20240822 AL551 AL552/MG1/FL_8W0907551R_0001__V001.bin |
| `4921fc68` | `0x18200` | 3 | 🆕 NEW | unknown | PowerPC (1)/8W0907559H S0009/Original/8W0907559H S0009, [Flash Container Extr... |
| `dd4df693` | `0x18200` | 2 | 🆕 NEW | unknown | 2.9TT B9 MG1/GPF/8W0907551F S0002/Original/DMG1002AH2C1793_MA20A31.bin |
| `a21af673` | `0x18200` | 2 | 🆕 NEW | tcu | Bosch TCU/AL551/0014C44_ZX8L4400/8R0927158AB 1003.bin |
| `723daec0` | `0x18200` | 2 | 🆕 NEW | tcu | Bosch TCU/AL552/AL552 Mod Cal Flash - 19-05-21_12-54-35_CANlog_parsed/1 - Fla... |
| `1582b98a` | `0x18200` | 2 | 🆕 NEW | mex17 | Bosch/Motronic/MEx17/UDS/EDC 17 C64/04L906021CK S8350/04L906021CK S8350, Orig... |
| `51866eb2` | `0x600200` | 2 | ✅ KNOWN (MD1CP004 T2) | md1 cp004 | Bosch/Motronic/MG1/MD1 CP004IFX/4K0907401 0004 MD1CP004 bench.bin |
| `51ffa96a` | `0x600200` | 2 | ✅ KNOWN (MD1CP014) | md1 cp014 | Bosch/Motronic/MG1/MD1 CP014IFX/FL_4M0997409_1005__V001.odx.bin |
| `7fa117fa` | `0x600200` | 2 | 🆕 NEW | mg1 cs008 | Bosch/Motronic/MG1/MG1 CS008IFX/4M0906014B S0003, stage 1-SQ7_93oct_Rev1.4.bin |
| `29fd0bf7` | `0x18200` | 2 | ✅ KNOWN (MG1CS011) | mg1 cs011 | Bosch/Motronic/MG1/MG1 CS011/FL_05E906018AK_9602__V001[05E906018AK S9602].bin |
| `0c1d3f66` | `0x18200` | 2 | 🆕 NEW | unknown | Damos and Defs/Bosch ME7.5/8N0018B/FKTTEMP.BIN |
| `f9a4c600` | `0x18200` | 2 | 🆕 NEW | unknown | Damos and Defs/Bosch ME7.5/TT225_0261206228.bin |
| `342e028b` | `0x18200` | 2 | 🆕 NEW | unknown | Damos and Defs/MG1 B9RS5/4K0907551 SX717/E9B00s3m4_X717.80060100.bin |
| `f0ce50e3` | `0x18200` | 2 | 🆕 NEW | unknown | Damos and Defs/MG1 B9RS5/4K0907551 SX717/E9B00s3m4_X717.805FFAE0.bin |
| `bda28ff3` | `0x18200` | 2 | 🆕 NEW | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMod/6-0x80180100-_RAW.bin |
| `2d5c470e` | `0x18200` | 2 | 🆕 NEW | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toStock/6-0x80180100-_RA... |
| `070d8826` | `0x18200` | 2 | 🆕 NEW | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/5 - Flashed to Sector-0x2_... |
| `43b5209b` | `0x18200` | 2 | 🆕 NEW | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/6 - Flashed to Sector-0x3_... |
| `555b9a08` | `0x18200` | 2 | 🆕 NEW | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/6 - Flashed to Sector-0x3_... |
| `184f18ce` | `0x18200` | 2 | 🆕 NEW | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/7 - Flashed to Sector-0x4_... |
| `d0a6801b` | `0x18200` | 2 | 🆕 NEW | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/7 - Flashed to Sector-0x4_... |
| `2ce681e5` | `0x18200` | 2 | 🆕 NEW | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/9 - Flashed to Sector-0x6_... |
| `ea7554ee` | `0x18200` | 2 | 🆕 NEW | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x2_RAW.bin |
| `fc674e00` | `0x18200` | 2 | 🆕 NEW | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x3_RAW.bin |
| `4be2d426` | `0x18200` | 1 | 🆕 NEW | unknown | Damos and Defs/Bosch ME7.5/M1E0624e.BIN |
| `56bc464c` | `0x18200` | 1 | 🆕 NEW | unknown | Damos and Defs/OPEL ME7.6.2 ASTRA 1.4 Z14XEP06/32120301.BIN |
| `cae58820` | `0x18200` | 1 | 🆕 NEW | edc17 | Damos and Defs/VAG EDC17CP14 VW TIGUAN 2.0TDI/Original_VW_Tiguan03L906022B___... |
| `fad1233d` | `0x18200` | 1 | 🆕 NEW | unknown | Damos and Defs/VAG ME7.5 AUDI A6 1.8T 180HP/0261206917.bin |
| `4eebe1a3` | `0x18200` | 1 | 🆕 NEW | unknown | Damos and Defs/VOLVO M4.4 V70 2.5 SUG/0261204570_1037358289.bin |
| `b6eb6589` | `0x18200` | 1 | 🆕 NEW | unknown | MG1 (2)/MG1CS002 2,9l V6 TFSI, EA839, AU49x RS4_5, 331kW, EU6, AL552-8Q_8W090... |

## NEW fingerprints by ECU family

### unknown — 32 new fingerprints across 87 bins

| sha256[:8] | Bins | Sample |
|---|---|---|
| `66678bb9` | 9 | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/1 - Flashed to S... |
| `b6267173` | 6 | MG1 (1)/B9_3.0T_OBD_parsed_________/0x1_2_DECODED.bin |
| `5c62f84d` | 5 | Damos and Defs/Bosch ME7.5/2C2SFGB3/2C2SFGB3.BIN |
| `2fc78b3a` | 4 | Damos and Defs/Bosch ME7.5/8N0906018AM_0261207417_1037360844.bin |
| `08829054` | 4 | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMod/10-0x808... |
| `12f6fefd` | 4 | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMod/12-0x809... |
| `3402b3f1` | 4 | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMod/8-0x8050... |
| `e05643d3` | 4 | MG1 (1)/B9_3.0T_OBD_parsed_________/0x5_2_RAW.bin |
| `bcd4a935` | 3 | 2.9TT B9 MG1/8W0907551D S0002/Original/Customer Backup for ORI/2022... |
| `a614dd18` | 3 | Damos and Defs/Bosch ME7.1/ME7.5.bin |
| `7105af75` | 3 | Dokumentation/Fresh dumps/20240822 AL551 AL552/MG1/FL_8W0907551R_00... |
| `4921fc68` | 3 | PowerPC (1)/8W0907559H S0009/Original/8W0907559H S0009, [Flash Cont... |
| `dd4df693` | 2 | 2.9TT B9 MG1/GPF/8W0907551F S0002/Original/DMG1002AH2C1793_MA20A31.bin |
| `0c1d3f66` | 2 | Damos and Defs/Bosch ME7.5/8N0018B/FKTTEMP.BIN |
| `f9a4c600` | 2 | Damos and Defs/Bosch ME7.5/TT225_0261206228.bin |
| `342e028b` | 2 | Damos and Defs/MG1 B9RS5/4K0907551 SX717/E9B00s3m4_X717.80060100.bin |
| `f0ce50e3` | 2 | Damos and Defs/MG1 B9RS5/4K0907551 SX717/E9B00s3m4_X717.805FFAE0.bin |
| `bda28ff3` | 2 | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMod/6-0x8018... |
| `2d5c470e` | 2 | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toStock/6-0x80... |
| `070d8826` | 2 | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/5 - Flashed to S... |
| `43b5209b` | 2 | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/6 - Flashed to S... |
| `555b9a08` | 2 | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/6 - Flashed to S... |
| `184f18ce` | 2 | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/7 - Flashed to S... |
| `d0a6801b` | 2 | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/7 - Flashed to S... |
| `2ce681e5` | 2 | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/9 - Flashed to S... |
| `ea7554ee` | 2 | MG1 (1)/B9_3.0T_OBD_parsed_________/0x2_RAW.bin |
| `fc674e00` | 2 | MG1 (1)/B9_3.0T_OBD_parsed_________/0x3_RAW.bin |
| `4be2d426` | 1 | Damos and Defs/Bosch ME7.5/M1E0624e.BIN |
| `56bc464c` | 1 | Damos and Defs/OPEL ME7.6.2 ASTRA 1.4 Z14XEP06/32120301.BIN |
| `fad1233d` | 1 | Damos and Defs/VAG ME7.5 AUDI A6 1.8T 180HP/0261206917.bin |
| `4eebe1a3` | 1 | Damos and Defs/VOLVO M4.4 V70 2.5 SUG/0261204570_1037358289.bin |
| `b6eb6589` | 1 | MG1 (2)/MG1CS002 2,9l V6 TFSI, EA839, AU49x RS4_5, 331kW, EU6, AL55... |

### mex17 — 5 new fingerprints across 50 bins

| sha256[:8] | Bins | Sample |
|---|---|---|
| `d6f4b42a` | 16 | Bosch/Motronic/MEx17/UDS/MED 17.5.2/04E906016F S5760, Original.bin |
| `f5d0d2c3` | 12 | Bosch/Motronic/MEx17/UDS/MED 17.1.1 Dual ECU/4S0907552F 0003.bin |
| `72e22b78` | 10 | Bosch/Motronic/MEx17/TP2/MED 17.5.2 TP2/2.0TFSI 06J906027FJ S4352 S... |
| `b0cb819b` | 10 | Bosch/Motronic/MEx17/UDS/EDC 17 C74/04L906026JF S4137/04L906026JF S... |
| `1582b98a` | 2 | Bosch/Motronic/MEx17/UDS/EDC 17 C64/04L906021CK S8350/04L906021CK S... |

### mex9 — 1 new fingerprints across 27 bins

| sha256[:8] | Bins | Sample |
|---|---|---|
| `0e2cad79` | 27 | Bosch/Motronic/MEx9/MED 9.1.2 Dual ECU/420910552L-0261S02817-103750... |

### tcu — 7 new fingerprints across 25 bins

| sha256[:8] | Bins | Sample |
|---|---|---|
| `b7d39dab` | 6 | Bosch TCU/DQ/DSG DQ38x G2/0GC906557AM 4162  - Original BENCH A46E .bin |
| `5f0731e8` | 6 | Bosch TCU/DQ/DSG DQ500/0DL300012H S2110, [Flash Container Extract].bin |
| `d8191fbd` | 3 | Bosch TCU/DQ/DSG DQ38x G2 (Cal Only VR)/0GC906557M 3562.bin |
| `7971a7e5` | 3 | Bosch TCU/DQ/DSG DQ38x G2 (Cal Only VR)/0GC906557P 3560.bin |
| `44985e85` | 3 | Bosch TCU/DQ/DSG DQ500 (CMD File)/0DL300012H S2112, Original.bin |
| `a21af673` | 2 | Bosch TCU/AL551/0014C44_ZX8L4400/8R0927158AB 1003.bin |
| `723daec0` | 2 | Bosch TCU/AL552/AL552 Mod Cal Flash - 19-05-21_12-54-35_CANlog_pars... |

### mg1 cs002 — 1 new fingerprints across 8 bins

| sha256[:8] | Bins | Sample |
|---|---|---|
| `664419e3` | 8 | Bosch/Motronic/MG1/MG1 CS002 (Autotuner)/8W0907559G S0008/8W0907559... |

### mg1 cs008 — 1 new fingerprints across 2 bins

| sha256[:8] | Bins | Sample |
|---|---|---|
| `7fa117fa` | 2 | Bosch/Motronic/MG1/MG1 CS008IFX/4M0906014B S0003, stage 1-SQ7_93oct... |

### edc17 — 1 new fingerprints across 1 bins

| sha256[:8] | Bins | Sample |
|---|---|---|
| `cae58820` | 1 | Damos and Defs/VAG EDC17CP14 VW TIGUAN 2.0TDI/Original_VW_Tiguan03L... |

## How to recover key bytes for a NEW fingerprint

For any row above marked 🆕 NEW, the recipe is mechanical:
1. Open the **sample path** bin at the listed **offset**.
2. Read 16 bytes. SHA-256 those bytes → confirm first 4 bytes match the table's `sha256[:8]`.
3. The bytes ARE the AES-128 key for that ECU class.
4. Add an entry to `secrets/AES_KEYS_MASTER.md` (per Hard Rule 5, key bytes live in `secrets/` only). Add per-boxcode mappings to `secrets/aes_keys_per_boxcode.json`.
5. Validate by capturing an MM flash session for that ECU class + running through `tools/flash_shadow_diff.py` — should match plaintext-equivalent like the RS7 did.

## Caveats

- The entropy filter rejects all-zero, all-0xFF, and repeated-byte blocks. False negatives possible if a real key happens to be ASCII-like (low entropy by Shannon math). MD1CP004 Type1 is literally the bytes `"AES-Default-Key2"` — it would pass the entropy filter, but a similarly-crafted Bosch key WITH a structure could fail. Not observed in this corpus.
- Inferred family comes from the bin's path inside `034_local/`. Paths like `MG1 (1)/2.9T MG1 - 02-07-21_..._CANlog_parsed/...` are CAN-log parser outputs of MG1 flash sessions — the bytes at offset 0x18200 there may be sniff-stream artifacts, NOT real ECU bootloader content. Sample those carefully before adding to the manifest. The 2 fingerprints with `inferred_family=unknown` fall in this category.
- `inferred_family=tcu` covers Bosch transmission ECUs (DSG DQ250/DQ381/DQ500). They use the same wire-format AES storage convention but they're a separate ECU class from engine ECUs — flash protocol details (SA2 script, section map, post-commit dependencies) differ. The keys alone don't unlock TCU flashing without the full per-variant protocol work.
- The 462 "no acceptable offset" bins are not necessarily junk — they may be older Bosch firmware that stores AES keys at offsets other than 0x18200 / 0x600200, or they may not be Bosch ECU bootloader bins at all (could be transmission, ABS, body control modules). Worth a future pass with a broader offset sweep.