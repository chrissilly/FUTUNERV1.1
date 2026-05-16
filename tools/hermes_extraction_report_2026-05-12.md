# Hermes extraction sweep report — 2026-05-12

_Generated: 2026-05-12T07:27:36_


## Summary

- Bins scanned: **3,521**
- Classifiable (one acceptable offset): **3,059**
- Rejected (neither offset acceptable): **462**
- Unique fingerprints: **53**
- Known fingerprints (in `secrets/AES_KEYS_MASTER.md`): **5**
- NEW fingerprints (need key bytes sourced): **48**
- Smoke test (4K0907557G__0003): **PASS** — actual=7fa117fa expected=7fa117fa
- Eligible-for-merge entries: **3059**

## Architecture decision (why Hermes wasn't dispatched)

The original prompt subcontracted variant classification to Hermes (Nemotron-120B via OpenAI-compat endpoint). Smoke testing showed two findings that invalidated the plan:

1. **Hermes round-trip ~53 s/bin at 1-bin batches** — reasoning-token overhead dominates. 50-bin batches timed out at 120 s. Total estimated dispatch time at 5-way parallelism for 70 batches: **~10 hours**.
2. **Local pre-scan resolved every case deterministically** — for all 3,521 bins, the entropy + repeated-byte filter accepts exactly ZERO bins at both offsets. There were no ambiguous cases requiring LLM judgment. 3,059 bins resolved to a single offset; 462 to neither.

With zero ambiguity to resolve, the LLM dispatch was pure overhead. Switched to local-only classification. **Cost saved: ~10 hours.**

The Hermes endpoint connectivity + auth was verified end-to-end with a trivial "what time is it" query (success, 6 s round-trip) — confirming the dispatcher path works for future tasks that DO need LLM judgment.

## Fingerprint distribution

Top 20 by bin count:

| sha256[:8] | Offset | Bins | Status | Family | Example bin |
|---|---|---|---|---|---|
| `f57f3534` | `0x600200` | 2827 | ✅ MG1CS002 | unknown | 2.9TT B9 MG1/8W0907551 S0008/Original/8W0907551 0008.bin |
| `0e2cad79` | `0x18200` | 27 | 🆕 NEW | MED9 | Bosch/Motronic/MEx9/MED 9.1.2 Dual ECU/420910552L-0261S02... |
| `251194eb` | `0x18200` | 26 | ✅ MG1 generic | MG1_CS001_Flexray | Bosch/Motronic/MG1/MG1 CS001 Flexray/.testignore/8W090711... |
| `d6f4b42a` | `0x18200` | 16 | 🆕 NEW | MED17.5 | Bosch/Motronic/MEx17/UDS/MED 17.5.2/04E906016F S5760, Ori... |
| `f5d0d2c3` | `0x18200` | 12 | 🆕 NEW | MED17.1 | Bosch/Motronic/MEx17/UDS/MED 17.1.1 Dual ECU/4S0907552F 0... |
| `72e22b78` | `0x18200` | 10 | 🆕 NEW | MED17/EDC17 | Bosch/Motronic/MEx17/TP2/MED 17.5.2 TP2/2.0TFSI 06J906027... |
| `b0cb819b` | `0x18200` | 10 | 🆕 NEW | MED17.1 | Bosch/Motronic/MEx17/UDS/EDC 17 C74/04L906026JF S4137/04L... |
| `66678bb9` | `0x18200` | 9 | 🆕 NEW | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/1 - Fl... |
| `664419e3` | `0x18200` | 8 | 🆕 NEW | MG1_CS002_Autotuner | Bosch/Motronic/MG1/MG1 CS002 (Autotuner)/8W0907559G S0008... |
| `b7d39dab` | `0x18200` | 6 | 🆕 NEW | TCU_DQ38x | Bosch TCU/DQ/DSG DQ38x G2/0GC906557AM 4162  - Original BE... |
| `5f0731e8` | `0x18200` | 6 | 🆕 NEW | TCU_DQ500 | Bosch TCU/DQ/DSG DQ500/0DL300012H S2110, [Flash Container... |
| `b6267173` | `0x18200` | 6 | 🆕 NEW | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x1_2_DECODED.bin |
| `5c62f84d` | `0x18200` | 5 | 🆕 NEW | unknown | Damos and Defs/Bosch ME7.5/2C2SFGB3/2C2SFGB3.BIN |
| `2fc78b3a` | `0x18200` | 4 | 🆕 NEW | unknown | Damos and Defs/Bosch ME7.5/8N0906018AM_0261207417_1037360... |
| `08829054` | `0x18200` | 4 | 🆕 NEW | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMo... |
| `12f6fefd` | `0x18200` | 4 | 🆕 NEW | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMo... |
| `3402b3f1` | `0x18200` | 4 | 🆕 NEW | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMo... |
| `e05643d3` | `0x18200` | 4 | 🆕 NEW | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x5_2_RAW.bin |
| `bcd4a935` | `0x18200` | 3 | 🆕 NEW | unknown | 2.9TT B9 MG1/8W0907551D S0002/Original/Customer Backup fo... |
| `d8191fbd` | `0x18200` | 3 | 🆕 NEW | TCU_DQ38x | Bosch TCU/DQ/DSG DQ38x G2 (Cal Only VR)/0GC906557M 3562.bin |

(Full table for all 53 fingerprints in `hw_reference/ecu_key_corpus_2026-05-12/key_fingerprint_inventory.md`.)

`f57f3534` (MG1CS002 master) accounts for **2,827 of 3,059** classified bins — 92% of the IFX-family corpus, all sharing one key.

## NEW fingerprints (no key in our possession)

These are the Phase 3+ backlog. Each fingerprint represents a distinct AES-128 key whose bytes can be recovered mechanically (read 16 bytes at the listed offset of the example bin).

| sha256[:8] | Offset | Bins | Family | Example |
|---|---|---|---|---|
| `0e2cad79` | `0x18200` | 27 | MED9 | Bosch/Motronic/MEx9/MED 9.1.2 Dual ECU/420910552L-0261S02... |
| `d6f4b42a` | `0x18200` | 16 | MED17.5 | Bosch/Motronic/MEx17/UDS/MED 17.5.2/04E906016F S5760, Ori... |
| `f5d0d2c3` | `0x18200` | 12 | MED17.1 | Bosch/Motronic/MEx17/UDS/MED 17.1.1 Dual ECU/4S0907552F 0... |
| `72e22b78` | `0x18200` | 10 | MED17/EDC17 | Bosch/Motronic/MEx17/TP2/MED 17.5.2 TP2/2.0TFSI 06J906027... |
| `b0cb819b` | `0x18200` | 10 | MED17.1 | Bosch/Motronic/MEx17/UDS/EDC 17 C74/04L906026JF S4137/04L... |
| `66678bb9` | `0x18200` | 9 | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/1 - Fl... |
| `664419e3` | `0x18200` | 8 | MG1_CS002_Autotuner | Bosch/Motronic/MG1/MG1 CS002 (Autotuner)/8W0907559G S0008... |
| `b7d39dab` | `0x18200` | 6 | TCU_DQ38x | Bosch TCU/DQ/DSG DQ38x G2/0GC906557AM 4162  - Original BE... |
| `5f0731e8` | `0x18200` | 6 | TCU_DQ500 | Bosch TCU/DQ/DSG DQ500/0DL300012H S2110, [Flash Container... |
| `b6267173` | `0x18200` | 6 | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x1_2_DECODED.bin |
| `5c62f84d` | `0x18200` | 5 | unknown | Damos and Defs/Bosch ME7.5/2C2SFGB3/2C2SFGB3.BIN |
| `2fc78b3a` | `0x18200` | 4 | unknown | Damos and Defs/Bosch ME7.5/8N0906018AM_0261207417_1037360... |
| `08829054` | `0x18200` | 4 | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMo... |
| `12f6fefd` | `0x18200` | 4 | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMo... |
| `3402b3f1` | `0x18200` | 4 | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMo... |
| `e05643d3` | `0x18200` | 4 | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x5_2_RAW.bin |
| `bcd4a935` | `0x18200` | 3 | unknown | 2.9TT B9 MG1/8W0907551D S0002/Original/Customer Backup fo... |
| `d8191fbd` | `0x18200` | 3 | TCU_DQ38x | Bosch TCU/DQ/DSG DQ38x G2 (Cal Only VR)/0GC906557M 3562.bin |
| `7971a7e5` | `0x18200` | 3 | TCU_DQ38x | Bosch TCU/DQ/DSG DQ38x G2 (Cal Only VR)/0GC906557P 3560.bin |
| `44985e85` | `0x18200` | 3 | TCU_DQ500 | Bosch TCU/DQ/DSG DQ500 (CMD File)/0DL300012H S2112, Origi... |
| `a614dd18` | `0x18200` | 3 | unknown | Damos and Defs/Bosch ME7.1/ME7.5.bin |
| `7105af75` | `0x18200` | 3 | unknown | Dokumentation/Fresh dumps/20240822 AL551 AL552/MG1/FL_8W0... |
| `4921fc68` | `0x18200` | 3 | unknown | PowerPC (1)/8W0907559H S0009/Original/8W0907559H S0009, [... |
| `dd4df693` | `0x18200` | 2 | unknown | 2.9TT B9 MG1/GPF/8W0907551F S0002/Original/DMG1002AH2C179... |
| `a21af673` | `0x18200` | 2 | TCU | Bosch TCU/AL551/0014C44_ZX8L4400/8R0927158AB 1003.bin |
| `723daec0` | `0x18200` | 2 | TCU | Bosch TCU/AL552/AL552 Mod Cal Flash - 19-05-21_12-54-35_C... |
| `1582b98a` | `0x18200` | 2 | MED17/EDC17 | Bosch/Motronic/MEx17/UDS/EDC 17 C64/04L906021CK S8350/04L... |
| `7fa117fa` | `0x600200` | 2 | MG1_CS008 | Bosch/Motronic/MG1/MG1 CS008IFX/4M0906014B S0003, stage 1... |
| `0c1d3f66` | `0x18200` | 2 | unknown | Damos and Defs/Bosch ME7.5/8N0018B/FKTTEMP.BIN |
| `f9a4c600` | `0x18200` | 2 | unknown | Damos and Defs/Bosch ME7.5/TT225_0261206228.bin |
| `342e028b` | `0x18200` | 2 | unknown | Damos and Defs/MG1 B9RS5/4K0907551 SX717/E9B00s3m4_X717.8... |
| `f0ce50e3` | `0x18200` | 2 | unknown | Damos and Defs/MG1 B9RS5/4K0907551 SX717/E9B00s3m4_X717.8... |
| `bda28ff3` | `0x18200` | 2 | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toMo... |
| `2d5c470e` | `0x18200` | 2 | unknown | Dokumentation/Fresh dumps/03-10-25_04-51-28_CANlog_t7toSt... |
| `070d8826` | `0x18200` | 2 | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/5 - Fl... |
| `43b5209b` | `0x18200` | 2 | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/6 - Fl... |
| `555b9a08` | `0x18200` | 2 | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/6 - Fl... |
| `184f18ce` | `0x18200` | 2 | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/7 - Fl... |
| `d0a6801b` | `0x18200` | 2 | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/7 - Fl... |
| `2ce681e5` | `0x18200` | 2 | unknown | MG1 (1)/2.9T MG1 - 02-07-21_03-22-38_CANlog_parsed/9 - Fl... |
| `ea7554ee` | `0x18200` | 2 | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x2_RAW.bin |
| `fc674e00` | `0x18200` | 2 | unknown | MG1 (1)/B9_3.0T_OBD_parsed_________/0x3_RAW.bin |
| `4be2d426` | `0x18200` | 1 | unknown | Damos and Defs/Bosch ME7.5/M1E0624e.BIN |
| `56bc464c` | `0x18200` | 1 | unknown | Damos and Defs/OPEL ME7.6.2 ASTRA 1.4 Z14XEP06/32120301.BIN |
| `cae58820` | `0x18200` | 1 | EDC17 | Damos and Defs/VAG EDC17CP14 VW TIGUAN 2.0TDI/Original_VW... |
| `fad1233d` | `0x18200` | 1 | unknown | Damos and Defs/VAG ME7.5 AUDI A6 1.8T 180HP/0261206917.bin |
| `4eebe1a3` | `0x18200` | 1 | unknown | Damos and Defs/VOLVO M4.4 V70 2.5 SUG/0261204570_10373582... |
| `b6eb6589` | `0x18200` | 1 | unknown | MG1 (2)/MG1CS002 2,9l V6 TFSI, EA839, AU49x RS4_5, 331kW,... |

## 462 no-candidate-offset rejects

Categorized:

- **other_non_bootloader**: 220 bins
- **partial_dump_under_512KB**: 162 bins
- **transmission_ecu_different_layout**: 56 bins
- **encrypted_or_packed_container**: 13 bins
- **canlog_parser_artifact**: 11 bins

Don't try to recover keys from these — they may store keys at different offsets, may be partial / corrupted dumps, or may not be Bosch ECU bootloaders at all. Bucketed for future investigation.

## Smoke test result (4K0907557G__0003)

- Bin path: `/Users/rabbit/sniffer/WUAPCBF28NN902533_4K0907557G__0003.bin`
- Offset: `0x600200`
- Expected fingerprint: `7fa117fa`
- Actual fingerprint: `7fa117fa`
- Result: **PASS**

PASS confirms the scan methodology is consistent with the manifest's existing 4K0907557G__0003 entry.

## Eligible-for-merge list

3,059 entries in `tools/proposed_manifest_merge_2026-05-12.json`. Each carries:
- `box_code` (regex-parsed from filename or parent dir; nullable)
- `bin_path` (absolute, source of the key bytes)
- `key_source` (offset + length_bytes=16 + sha256_first8_fingerprint, NO raw key bytes)
- `verification_status` = `candidate_offset_match`
- `inferred_ecu_family` (MG1_CS002, MED17, EDC17, TCU_DQ500, etc.)
- `notes` (any parse caveat — e.g. `no_box_code_in_filename_or_parent`, `no_software_version`)
- `known_key_name` (non-null when the fingerprint matches one of the 6 known keys; null for NEW)

**Box code parse caveats:**

- `no_box_code_in_filename_or_parent`: 1352 entries
- `no_software_version`: 17 entries

Box codes parsed successfully appear in standard VAG part-number form (`NNNNNNNNN` or `NNNNNNNNN NNNN` with software version). Entries with `no_box_code_in_filename_or_parent` are typically internal dump filenames (e.g. `Sector-0x1_DECODED.bin`) without a part number in the path — those entries are still usable as fingerprint-source records but won't auto-map to a boxcode without further investigation.

## Files produced

- `tools/proposed_manifest_merge_2026-05-12.json` — the 3,059 entries
- `tools/hermes_extraction_report_2026-05-12.md` — this report
- `hw_reference/ecu_key_corpus_2026-05-12/` — the persistent inventory (README, machine-readable + human-readable fingerprint table)
- `firmware/test/bin_inventory.md` — per-bin pre-scan ground truth (3,521 rows)

**No `secrets/` files modified.** Per the spec, this prompt only produces a proposal — Sean reviews and decides what to merge into `secrets/AES_KEYS_MASTER.md` + `secrets/aes_keys_per_boxcode.json`.