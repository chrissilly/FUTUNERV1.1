# OEM Documentation Inventory — 034_local archive

> Built 2026-05-18. Catalog of all PDF/TXT/MD/DOC/HTML reference docs in the 164 GB ECU archive at `/Users/rabbit/034_local/`. Bin files, source code, A2L/Damos calibration files, build artifacts, and cached web assets are NOT inventoried here — only human-readable reference documentation. Categorization is by source dir + family + standards body. Paths are absolute. Mirrored copies (e.g. `Unzipped/` mirroring `Dokumentation/`) are listed once with a `Mirrored at:` note. Inventory was assembled by Glob enumeration; PDFs were not opened — purpose is inferred from filenames, parent directory, ECU family conventions, and standards naming.

## Summary

- Total unique docs catalogued: **~190** (after deduplication; raw filesystem count is ~280 once duplicate paths are folded together)
- Major categories: **17** (see TOC below)
- Notable duplications: `Unzipped/` and top-level `Dokumentation/` are full mirrors of each other for AUTOSAR / ASAM / Aisin / Delphi / m232-master / ASAP2 / AMAX-explained / Fresh-dumps. `MG1 (1)/` and `MG1 (2)/` partially overlap (MG1 (2) is a superset). `Unzipped/Bosch TCM/AL55x/*` mirrors `Bosch TCU/AL55x/*` for PDFs. The top-level `Dokumentation (1)/` is the canonical Protocols + AUTOSAR + PowerPC + Tricore tree and does NOT mirror with anything else — that material is single-copy.
- Notable findings:
  - **Full Bosch corporate flash-programming spec set is present**: `Corporate Group Requirement Specification For Programming Control Units with Keyword Protocol 2000 Transport Protocol 2.0.pdf`, `FlashProgrammierung V1-10.pdf`, `FDS_Lastenheft_allgemein_V0_2.pdf` (VW Flash Data Security spec), `VW80124` UDS protocol, `VW80125` ECU identification, `VW80126` ECU programming, `SA2-060331-V10.pdf` (VW Seed/Key Algorithm — Seed-to-Key).
  - **Internal VAG Group Seed/Key algorithm note** (`VAG Group Seed Key algorithm.txt`) and `MED9 Seed-Key Notes.txt` — operational notes, not OEM PDFs.
  - **`Bench Flash Notice.pdf` and `Rambo Patch Comms.pdf`** — tuner-community documents, scope unclear from filename alone.
  - Complete **AUTOSAR SWS specs for the diagnostic stack** (DCM, DoIP, E2E lib, NvM, Fee, EA, MemIf) and **ASAM MCD-2MC (A2L) v1.6 spec** plus full **ASAM/ODX-MEM/Autorenrichtlinien VW specs** (with side-by-side German + machine-translated HTML).
  - **Infineon TriCore family is exhaustively documented**: TC1766/1767/1782/1784/1791/1793/1796/1797/1798, TC29x (B-step + BC), AURIX TC2xx/TC3xx, plus AUDO bootloader application notes — covers every TriCore generation the archive's bin dumps target.
  - **NXP/Motorola PowerPC family**: MPC561, MPC5674F/RM, MPC5777C/M, MPC82X, e200z3 core reference, VLE-PEM (Variable-Length Encoding instruction set).
  - Reverse-engineered Hyundai Theta II FDEF dump (`Hyundai Theta II FDEF_691F00.pdf`) — the only non-VAG calibration doc apart from Ford/Renault/Opel Damos text exports.
  - `MED9 File Structure OLS.txt` — internal note on the WinOLS file layout for MED9, useful for the FUTV1.1 calibration writer.
  - Bosch `D-MG1-011V01CJ000_Protected-VW-Audi.pdf` and a complete set of MG1CS001/002/003/008/011 SWCalDoc PDFs — the official Bosch software calibration documents (SWCalDoc) for the MG1 ECU family. **These are the gold-standard reference for FUTV1.1's MG1 CAN ID 0x7E0 lockout work.**

---

## Categories

1. [ISO / SAE / OBD Standards](#iso--sae--obd-standards)
2. [AUTOSAR SWS Specifications](#autosar-sws-specifications)
3. [ASAM Standards](#asam-standards)
4. [VW / Audi Corporate Specs (VW80124 / VW80125 / VW80126 / FDS)](#vw--audi-corporate-specs)
5. [KWP2000 / KWP1281 / K-Line / TP2.0](#kwp2000--kwp1281--k-line--tp20)
6. [Generic CAN / Bootloader / Flash Protocol Refs](#generic-can--bootloader--flash-protocol-refs)
7. [Infineon TriCore family](#infineon-tricore-family)
8. [NXP / Motorola PowerPC family](#nxp--motorola-powerpc-family)
9. [Bosch MG1 family (MG1CS001/002/003/008/011) — Software Calibration Documents](#bosch-mg1-family-swcaldoc)
10. [Bosch MED9 / MED17 / MEDC17 / EDC17](#bosch-med9--med17--medc17--edc17)
11. [Bosch TCU (AL551 / AL552 / DQ500 / DQ381)](#bosch-tcu-al551--al552--dq500--dq381)
12. [Aisin Transmission ECU (09G TipTronic)](#aisin-transmission-ecu)
13. [Delphi DCM](#delphi-dcm)
14. [m232-master (AAN Audi I5 turbo tuning project)](#m232-master)
15. [Damos and Defs (Non-VAG: BMW / Ford / Opel / Renault)](#damos-and-defs-non-vag)
16. [Internal notes / CAN logs / readme.txt / customer recovery docs](#internal-notes--can-logs)
17. [Flash Client (034 internal tooling)](#flash-client-internal-tooling)

---

## ISO / SAE / OBD Standards

| Filename | Path | Brief purpose |
|---|---|---|
| ISO14229-2006, UDS Road_vehicles_Unified_diagnostic_service.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | UDS (Unified Diagnostic Services) road-vehicles specification — the canonical UDS standard. Core reference for FUTV1.1's diag stack. |
| ISO-15765-2-2004.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | ISO 15765-2 ISO-TP — diagnostic communication over CAN, transport layer (multi-frame, flow control). |
| iso_15765-4.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | ISO 15765-4 — Requirements for emissions-related systems over CAN (OBD-II on CAN baseline). |
| ISO15765-3.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | ISO 15765-3 — Implementation of UDS over CAN. |
| ISO14230-2.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | ISO 14230-2 (KWP2000) — data link layer. |
| ISO-15031-5[1].pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | ISO 15031-5 — Emissions-related diagnostic services (OBD-II SAE J1979 equivalent at ISO). |
| ISO 22900-2-2009.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | ISO 22900-2 — D-PDU API (modular vehicle communication interface API). |
| ISO_13400-2_2019(en).pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | ISO 13400-2 — Diagnostic communication over Internet Protocol (DoIP), transport and network layer. |
| SAE_J2534_2002.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/Flashing Information/` | SAE J2534 (PassThru) — recommended practice for reprogramming via the OBD connector. |
| J2534 - PassThru_API-1.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/Flashing Information/` | SAE J2534 PassThru API reference. |
| SAE_OBD_II_ISO_15031-5_(eng).pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/Flashing Information/` | SAE/OBD-II overlap of ISO 15031-5 emissions diag services. |
| 14230-1s.pdf, 14230-2.pdf, 14230-2s.pdf, 14230-3s.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/Flashing Information/` and `/Protocols/` (74741047-14230-3s.pdf) | ISO 14230 parts 1/2/3 (KWP2000 physical/data-link/application). The `-s` versions are revised/safe-copy drafts. |
| SSF 14230-3-2000.Keyword protocol 2000.Application layer.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` and `/Protocols/Flashing Information/` | Swedish Standard SSF 14230-3 — KWP2000 application layer (mirror of ISO 14230-3). |
| OBD_dtc_list.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Standard OBD-II Diagnostic Trouble Code (DTC) list reference. |
| 148449900-OBD2-Protocols.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | General OBD-II protocols summary (third-party compilation). |
| stn1100-frpm.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | STN1100 (OBD-II interpreter IC) firmware reference / programming manual. |

## AUTOSAR SWS Specifications

Mirrored: `/Users/rabbit/034_local/Unzipped/AUTOSAR/` and `/Users/rabbit/034_local/Dokumentation/AUTOSAR/` (same files). The four DIAG-stack-specific specs additionally live under `Dokumentation (1)/Protocols/`.

| Filename | Path (primary) | Brief purpose |
|---|---|---|
| AUTOSAR_SWS_DiagnosticCommunicationManager.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | AUTOSAR SWS — DCM (Diagnostic Communication Manager). Core diagnostic dispatcher spec. |
| AUTOSAR_SWS_DiagnosticOverIP.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | AUTOSAR SWS — DoIP module. |
| AUTOSAR_SWS_E2ELibrary.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | AUTOSAR SWS — E2E (end-to-end) protection library. Critical for safety-rated CAN message integrity. |
| AUTOSAR_SWS_FlashEEPROMEmulation.pdf | `/Users/rabbit/034_local/Dokumentation/AUTOSAR/` | AUTOSAR SWS — Fee module. Mirrored at `/Users/rabbit/034_local/Unzipped/AUTOSAR/`. |
| AUTOSAR_SWS_EEPROMAbstraction.pdf | `/Users/rabbit/034_local/Dokumentation/AUTOSAR/` | AUTOSAR SWS — Ea module. Mirrored at `/Users/rabbit/034_local/Unzipped/AUTOSAR/`. |
| AUTOSAR_SWS_MemoryAbstractionInterface.pdf | `/Users/rabbit/034_local/Dokumentation/AUTOSAR/` | AUTOSAR SWS — MemIf module. Mirrored at `/Users/rabbit/034_local/Unzipped/AUTOSAR/`. |
| AUTOSAR_SWS_NVRAMManager.pdf | `/Users/rabbit/034_local/Dokumentation/AUTOSAR/` | AUTOSAR SWS — NvM module. Mirrored at `/Users/rabbit/034_local/Unzipped/AUTOSAR/`. |

## ASAM Standards

Mirrored across `Dokumentation/ASAM Standards/`, `Dokumentation (1)/ASAM Standards/`, and `Unzipped/ASAM Standards/`.

| Filename | Path (primary) | Brief purpose |
|---|---|---|
| ASAM_MCD-2MC_DataSpecifcation_V1.6.pdf | `/Users/rabbit/034_local/Dokumentation/ASAM Standards/` | ASAM MCD-2 MC v1.6 — the A2L file format specification (measurement/calibration data exchange). |
| asam_odx_autorenrichtlinie_v_1_2.pdf | `/Users/rabbit/034_local/Dokumentation/ASAM Standards/VW-ASAM-ODX-Autorenrichtlinien/` | ASAM ODX authoring guideline v1.2 (German). |
| Author guideline for ASAM _ MCD-2D and ODX data sets.html | `/Users/rabbit/034_local/Dokumentation/ASAM Standards/VW-ASAM-ODX-Autorenrichtlinien/translated/` | English machine translation of the ASAM ODX authoring guideline. |
| ECU-MEM-Spezifikation-V1_2_0.pdf | `/Users/rabbit/034_local/Dokumentation/ASAM Standards/VW-ASAM-ODX-ECU-MEM-Spezifikation/` | ASAM ODX ECU memory specification v1.2.0 (German). |
| ODX ECU-MEM specification.html | `/Users/rabbit/034_local/Dokumentation/ASAM Standards/VW-ASAM-ODX-ECU-MEM-Spezifikation/Translated/` | English machine translation of the ASAM ODX ECU-MEM spec. |
| ASAP2.pdf | `/Users/rabbit/034_local/Dokumentation/` (and `Unzipped/`) | Legacy ASAP2 (predecessor name for ASAM MCD-2 MC / A2L) spec. |
| ASAP2DAM Version 6.0.doc | `/Users/rabbit/034_local/Damos and Defs/Bosch ME7.5/` | Bosch ASAP2DAM v6.0 — Damos file format reference. |

## VW / Audi Corporate Specs

The single-copy VW-internal protocol specs. All under `/Users/rabbit/034_local/Dokumentation (1)/Protocols/`.

| Filename | Path | Brief purpose |
|---|---|---|
| VW80124_UDS_V1_4.pdf | `VW80124-uds-protocol/` | VW Group Standard 80124 v1.4 — UDS protocol application layer & implementation requirements for VW/Audi/Skoda/SEAT ECUs. |
| Unified Diagnostic Services Protocol Application-Layer & Implementation VW80124.html | `VW80124-uds-protocol/Translated/` | English machine translation of VW80124. |
| VW80125_V2_3.pdf | `VW80125-ecu-identification/` | VW Group Standard 80125 v2.3 — Regulation for the identification of electronic vehicle systems (ECU naming, software-version-coding scheme, F190/F19E/F1A2/etc DID layout). |
| Regulation for the identification of electronic vehicle systems VW 80125.html | `VW80125-ecu-identification/Translated/` | English machine translation of VW80125. |
| VW80126-060330-V11.pdf | `VW80126-ecu-programming/` | VW Group Standard 80126 v11 — UDS-compliant programming of control units. Defines flash sequencing, seed/key flow, session transitions for VW/Audi ECUs. |
| SA2-060331-V10.pdf | `VW80126-ecu-programming/` | VW Seed-to-Key (SA2) algorithm v10 — the SecurityAccess level-2 routine used by VW UDS ECUs. |
| Group specification VW80126 UDS-compliant programming of control units.html | `VW80126-ecu-programming/Translated/` | English machine translation of VW80126. |
| FDS_Lastenheft_allgemein_V0_2.pdf | `VW-Flashdatensicherheit/` | VW Flashdatensicherheit (Flash Data Security) Lastenheft v0.2 — the corporate requirements spec for flash data signing/encryption. |
| Flash data security (FDS) for UDS and KWP control units.html | `VW-Flashdatensicherheit/Translated/` | English machine translation of the FDS Lastenheft. |
| Freigabe-Motorsteuergerät_8V0907115_D_Y628.pdf | `/Users/rabbit/034_local/MG1 (1)/2.0T MG1/MG1CS001_..._Y628/` (mirror at MG1 (2)) | VW/Audi Freigabe (release/approval) doc for engine ECU PN 8V0907115_D Y628 calibration. |
| FL_81A907115_Y628__V001.pdf | `/Users/rabbit/034_local/MG1 (1)/2.0T MG1/MG1CS001_..._Y628/` (mirror at MG1 (2)) | Flash release doc for 81A907115 / Y628 / V001. |

## KWP2000 / KWP1281 / K-Line / TP2.0

All under `/Users/rabbit/034_local/Dokumentation (1)/Protocols/`.

| Filename | Path | Brief purpose |
|---|---|---|
| kwp2000.pdf | `/Protocols/` | Generic KWP2000 protocol reference. |
| kp2000_2.pdf | `/Protocols/` | KWP2000 alternate reference / extended notes. |
| KWP-1281.pdf | `/Protocols/` | KWP1281 — VW's legacy diagnostic protocol predecessor to KWP2000. |
| KWP1281 info.txt and KWP1281 info (1).txt | `/Protocols/` | Operational notes on KWP1281 implementation. |
| KWP1281 Measuring Block Value Formulas.txt and (1).txt | `/Protocols/` | Formulas to decode VW measuring blocks under KWP1281. |
| K-line communication description_V3 0(1).pdf | `/Protocols/` | K-line (single-wire ISO 9141) communication description rev 3.0. |
| Iso14230-1.doc, Iso14230-2.doc, Iso14230-3.doc | `/Protocols/kwp2000/` | ISO 14230 parts 1/2/3 in Word format (KWP2000 phy/dl/app). |
| Iso14229.doc | `/Protocols/kwp2000/` | ISO 14229 (UDS) in Word format. |
| kp2000-2.doc, kp2000-3.doc | `/Protocols/kwp2000/` | KWP2000 application-layer drafts (Word). |
| 31527838-7223-fiat-kwp2000.pdf | `/Protocols/` | Fiat-specific KWP2000 dialect notes. |
| 31527820-07274-Fiat-Standard-Diagnostic-Protocol-on-CAN.pdf | `/Protocols/` | Fiat KWP2000-on-CAN dialect. |
| 40136562-Fiat-Kwp2000.pdf, 44167816-kwp2000-euro2.pdf | `/Protocols/Flashing Information/` | Additional Fiat / Euro2 emissions-era KWP2000 references. |
| Fiat 9141, 1281.pdf | `/Protocols/Flashing Information/` | Fiat dialect notes for ISO 9141 + KWP1281. |
| lh_kwp2000_flashen_v1.1 EN.pdf | `/Protocols/` and `/Protocols/Flashing Information/` | LH KWP2000 flashing v1.1 (English). |
| LH_KWP2000_Flashen_V1_03.pdf | `/Protocols/TP2.0/` | LH KWP2000 flashing v1.03 (German original). |
| Corporate Group Requirement Specification For Programming Control Units with Keyword Protocol 2000 Transport Protocol 2.0.pdf | `/Protocols/Flashing Information/` | **VW corporate spec for programming ECUs over KWP2000 + TP2.0** — the canonical pre-UDS programming flow doc. |
| TP2.0_1_1.pdf, TP2.0_J2819.pdf, J2819_TP2.0.pdf | `/Protocols/TP2.0/` | VW TP 2.0 transport-protocol spec (v1.1, plus SAE J2819 mapping). |
| KWP2000 auf TP20_1_2.pdf | `/Protocols/TP2.0/` | KWP2000 on top of TP2.0 (German). |
| TP2.X Anhang_A 0_33.pdf, _B 0_23.pdf, _C 0_4.pdf, _D 0_3.pdf, _E 0_01.pdf | `/Protocols/TP2.0/` | TP2.X appendices A/B/C/D/E. |
| SPEC_T2FPCANUE4_V1.doc, CMSW_T2FPCANUE4_20.doc | `/Protocols/TP2.0/` | TP2.0 spec + CMSW (corporate measurement spec) variants. |
| 74741047-14230-3s.pdf | `/Protocols/` | ISO 14230-3 (s = safe-copy/reprint) third-party scan. |
| Various Protocol Descriptions 9141, TP,TP2.0, LIN.pdf | `/Protocols/` | Mixed protocol overview (ISO 9141, TP, TP2.0, LIN). |
| KWP2000 protocol _ Baidu Library.htm + _files/ | `/Protocols/Flashing Information/` | Saved-page from Baidu Library (Chinese-source KWP2000 reference, many cached assets). |
| 3.0lFlash.txt | `/Protocols/` | Internal note on 3.0L (Audi V6) flashing procedure. |
| DQ250_EF_ReadoutSteps.txt | `/Protocols/` | Internal note on DQ250 transmission ECU readout steps. |
| Flash Ranges.txt | `/Protocols/Flashing Information/` | Internal note on flash address ranges by ECU. |
| KWP Header Bytes.txt | `/Protocols/Flashing Information/` | KWP2000 header-byte decode notes. |
| MED9 File Structure OLS.txt | `/Protocols/Flashing Information/` | Internal note on MED9 ECU file structure as seen by WinOLS. |
| MED9 Seed-Key Notes.txt | `/Protocols/Flashing Information/` | Internal note on MED9 seed/key handshake. |
| VAG Group Seed Key algorithm.txt | `/Protocols/Flashing Information/` | Internal note on VAG SA1/SA2 seed-key algorithms. |
| mcmess_protocol.txt | `/Protocols/` | Internal note on Bosch MCMess (measurement) protocol. |

## Generic CAN / Bootloader / Flash Protocol Refs

| Filename | Path | Brief purpose |
|---|---|---|
| Bosch_CAN_Spec_V2.0.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Bosch CAN Specification v2.0 — the original CAN spec by Bosch. |
| CAN-Bus_English.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Generic CAN bus tutorial / overview. |
| CCP - V2.1(1).pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | ASAM CCP (CAN Calibration Protocol) v2.1. |
| XCP_ReferenceBook_V3.0_EN.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | XCP (Universal Calibration Protocol) reference book v3.0 (Vector). |
| 54498844-CAN-Automotive-Command-Set-User-Manual.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | National Instruments CAN Automotive Command Set user manual. |
| DoIP_faltblatt_softing.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Softing DoIP overview brochure (Faltblatt = leaflet). |
| odx_poster_faltblatt_softing_web.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Softing ODX overview poster. |
| UDS_Faltposter_softing2016.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Softing UDS overview poster 2016. |
| UDS_Protocol_Implementation_in_an_ECU.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Paper on implementing UDS inside an ECU (Vector or third-party). |
| UDS_Protocol_372139d.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | UDS protocol reference (additional). |
| ap1609211_CAN_Bootloader_.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Infineon Application Note 16092-11 — generic CAN bootloader. |
| TriCore_ap1609211_CAN_Bootloader_.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Same as above, TriCore-specific copy. |
| ap3213610_TriCore_AUDO_NG_Bootloader.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` (also under `/Tricore/`) | Infineon Application Note 32136-10 — AUDO NG TriCore bootloader. |
| DFU_1.1.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | USB DFU v1.1 spec (Device Firmware Upgrade). |
| FlashProgrammierung V1-10.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | German "Flash Programmierung" v1.10 — generic ECU flash-programming spec. |
| Presentation_Debrecen_En_2008_03_27.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Presentation from Debrecen 2008 (likely Vector/ECU diag/flash topic). |
| Volltext (PDF).pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | German "full text" — generic title; could be a thesis/paper full text. |
| Bench Flash Notice.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Tuner-community notice about bench flashing (purpose/warnings/procedure for direct-on-bench ECU flash). |
| Rambo Patch Comms.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Notes on "Rambo Patch" comms (community-named ECU patch tool comms protocol). |

## Infineon TriCore family

All under `/Users/rabbit/034_local/Dokumentation (1)/Tricore/`.

| Filename | Brief purpose |
|---|---|
| Infineon-TC1766-DS-v01_00-en.pdf | TC1766 datasheet. |
| TC1766_DS.pdf | TC1766 datasheet (alt scan). |
| TC1766_um_v2.0_2007_07.pdf | TC1766 user manual v2.0. |
| Infineon-TC1767-DS-v01_04-en.pdf | TC1767 datasheet v1.04. |
| Infineon-TC1782-DS-v01_04_01-en.pdf | TC1782 datasheet v1.04.01. |
| TC1784_UM_v1 1.pdf | TC1784 user manual v1.1. |
| Infineon-TC1791-DS-v01_01-en.pdf | TC1791 datasheet v1.01. |
| Infineon-TC1793-DS-v01_02-en.pdf | TC1793 datasheet v1.02. |
| tc1796_um_v2.0_2007_07.pdf | TC1796 user manual v2.0. |
| Infineon-tc1797-user-manual-UM-v01_10-EN.pdf | TC1797 user manual v1.10. |
| TC1798_um_v1_2.pdf | TC1798 user manual v1.2. |
| TriBoardManual-TC1798-V10.pdf | TriBoard hardware manual for TC1798 v1.0. |
| Infineon-TC29xBC-DataSheet-v01_00-EN.pdf | TC29x BC-step datasheet v1.00. |
| Infineon-TC29x_B-step-UM-v01_03-EN.pdf | TC29x B-step user manual v1.03. |
| Infineon-AURIX_TC2xx_Addendum-DS-v01_00-EN.pdf | AURIX TC2xx datasheet addendum v1.00. |
| Infineon-AURIX_TC3xx_Architecture_vol2-UserManual-v01_00-EN.pdf | AURIX TC3xx architecture user manual (vol 2) v1.00. |
| Infineon-AURIX_Hardware_Security_Module-Training-v01_01-EN.pdf | AURIX HSM (Hardware Security Module) training v1.01 — covers secure boot, key storage, crypto accelerators. |
| tc1_6__architecture_vol1.pdf | TriCore TC1.6 architecture manual vol 1 (core instruction set). |
| tc1_6__architecture_vol2.pdf | TriCore TC1.6 architecture manual vol 2 (peripherals). |
| tc_v131_instructionset_v138.pdf | TriCore v1.3.1 instruction set spec v1.38. |
| AP3213211_TriCore_AUDO_F_Bootloader.pdf | Infineon AppNote — AUDO Future TriCore bootloader. |
| ap3213610_TriCore_AUDO_NG_Bootloader.pdf | Infineon AppNote — AUDO Next-Gen TriCore bootloader. |
| ap326310_tc1765bootstraploader_v1.0_2002_03.pdf | Infineon AppNote — TC1765 bootstrap loader v1.0 (2002-03). |

## NXP / Motorola PowerPC family

All under `/Users/rabbit/034_local/Dokumentation (1)/PowerPC/` (some also at `MG1 (2)/` top level).

| Filename | Brief purpose |
|---|---|
| MPC561RM.pdf, MPC561RM-3139156.pdf, MPC561_Motorola.pdf | MPC561 reference manual (Motorola era). |
| MPC5674F.pdf | MPC5674F datasheet. |
| 16708_MPC5674RM_Rev7.pdf | MPC5674 reference manual rev 7. |
| MPC5777C.pdf, MPC5777C-1771093.pdf, NXP_MPC5777C_RM.pdf | MPC5777C datasheets / reference manuals — used by Bosch MG1CS family ECUs. Mirrored at `/Users/rabbit/034_local/MG1 (2)/`. |
| MPC5777CRMAD.pdf | MPC5777C reference manual addendum. Mirrored at `/Users/rabbit/034_local/MG1 (2)/`. |
| MPC5777MRM.pdf | MPC5777M reference manual. |
| MPC57XXXMB.pdf | MPC57xx motherboard/eval-board reference. |
| MPC82XINSET.pdf | MPC82x instruction-set reference. |
| VLEPEM.pdf | PowerPC VLE-PEM (Variable-Length Encoding programming environment manual). |
| cd00160310-e200z3-powerpc-core-reference-manual-stmicroelectronics (1).pdf | STMicro e200z3 PowerPC core reference manual (CD00160310). |
| E200Z0.pdf | `/Users/rabbit/034_local/MG1 (2)/` — e200z0 PowerPC core reference (peripheral coprocessor on MG1). |

## Bosch MG1 family — SWCalDoc

Bosch SWCalDoc PDFs (D-MG1-xxx... and DMG1xxx... naming) are the **canonical software calibration documents** for each MG1 variant, mapping calibration symbol names to physical/scaled engineering values. Critical reference for FUTV1.1's MG1 work.

| Filename | Path | Brief purpose |
|---|---|---|
| D-MG1-011V01CJ000_Protected-VW-Audi.pdf | `/Users/rabbit/034_local/MG1 (1)/` (mirror at `MG1 (2)/`) | Bosch SWCalDoc for MG1CS011 V01 CJ000 (VW/Audi variant, protected). |
| DMG1001A01C1398_MY18B01.pdf | `/Users/rabbit/034_local/MG1 (1)/` (mirror at `MG1 (2)/`) | SWCalDoc for MG1CS001 A01 C1398, MY18 B01 calibration. |
| DMG1001A01C1398_TY18B00.pdf | `/Users/rabbit/034_local/MG1 (2)/` | SWCalDoc for MG1CS001 A01 C1398, TY18 B00 calibration. |
| DMG1002A01C1303_MY17IC0.pdf | `/Users/rabbit/034_local/MG1 (2)/` | SWCalDoc for MG1CS002 A01 C1303, MY17 IC0 calibration. |
| VAG_MG1CS008_C1795_de_SWCalDoc.pdf | `/Users/rabbit/034_local/MG1 (1)/4.0TT/DMG1008PH2C1795_MA22G01/` (mirror at `MG1 (2)/4.0TT/`) | German-language SWCalDoc for MG1CS008 C1795 (4.0TT — Audi RS6/RS7/RSQ8 etc.). |
| BMW MG1CS003 B58 engine Germany MG1CS003_N74TUE_C0C2J6E5B_0_DE_SWCalDoc.pdf | `/Users/rabbit/034_local/MG1 (2)/` | BMW MG1CS003 SWCalDoc for B58 (6-cyl) / N74TUE, calibration C0C2J6E5B_0 (German). |
| VAG_30_TFSI_EA839_MG1CS002_DMG1038A01C1972_MAXXA00_1_de_SWCalDoc 31.10.2018.pdf | `/Users/rabbit/034_local/MG1 (2)/` | VAG 3.0 TFSI EA839 / MG1CS002 SWCalDoc dated 2018-10-31, MAXXA00 cal. |
| VAG_30_TFSI_EA839_HDR_System.pdf | `/Users/rabbit/034_local/MG1 (2)/` | EA839 HDR (high-pressure direct rail) system description doc. |
| EV_ECM29TFS0118W0907551_001005.pdf | `/Users/rabbit/034_local/MG1 (2)/MG1CS002 2,9l V6 TFSI .../` (mirror at `2.9TT B9 MG1/Retired/8W0907551 S0003/A2L/...`) | EV (Entwicklungsvereinbarung / dev-agreement) doc for ECM 2.9 TFSI PN 8W0907551, V001-005. |
| FL_8W0907551_0003__V001.pdf | `/Users/rabbit/034_local/MG1 (2)/MG1CS002 2,9l V6 TFSI .../` (mirror at `2.9TT B9 MG1/Retired/...`) | Flash release doc for 8W0907551 0003 V001. |
| pcmflash_89.pdf | `/Users/rabbit/034_local/MG1 (2)/` | PCMflash module 89 description (MG1 family flash module). |
| pcmflash_92.pdf | `/Users/rabbit/034_local/MG1 (2)/` | PCMflash module 92 description. |
| L22F5YZ03_1.txt | `/Users/rabbit/034_local/Damos and Defs/VAG MG1 RS3/` | Damos symbol export for MG1 RS3 calibration L22F5YZ03_1. |
| X03_8V0907115_C_0002g_ghidraSymbols.txt | `/Users/rabbit/034_local/MG1 (1)/2.0T MG1/MG1 2,0 R4 4V TFSI EA888 GEN3 BZ MQB A1_8V0907115C_0002/` (mirror at `MG1 (2)/`) | Ghidra-exported symbol list for 8V0907115_C_0002. |
| 2.9T MG1 - 02-07-21_03-22-38_CANlog.txt | `/Users/rabbit/034_local/MG1 (1)/2.9T MG1 - .../` (mirror at `MG1 (2)/`) | CAN log of an MG1 2.9T flash session, 2021-02-07. |
| B9 - OBD PID Listing.txt | `/Users/rabbit/034_local/MG1 (1)/` (mirror at `MG1 (2)/`) | OBD PID listing for B9 (Audi A4/A5/Q5 platform) MG1 ECUs. |

## Bosch MED9 / MED17 / MEDC17 / EDC17

| Filename | Path | Brief purpose |
|---|---|---|
| VW20FSIMED9.5.pdf | `/Users/rabbit/034_local/Damos and Defs/Bosch MED9.1/_Doc/` | VW 2.0 FSI MED9.5 ECU calibration doc. |
| txg0s057_olbz.doc, txg0s057_mlbz.doc | `/Users/rabbit/034_local/Damos and Defs/Bosch MED9.1/_Doc/` | Bosch MED9.1 calibration symbol-list / Damos exports (TXG0S057). |
| readme.txt | `/Users/rabbit/034_local/Bosch/Motronic/MEx17/TP2/MED 17.5.11 TP2/` | Readme for MED17.5.11 TP2 dump set. |
| D17162A02C000_MY19A1.error.txt | `/Users/rabbit/034_local/Bosch/Motronic/MEx17/UDS/MED 17.1.62/8S0907404E_0001/` | A2L parse/error log for MED17.1.62 MY19 A1 calibration. |
| E17H5YVA2_m5g.error.txt, E17H7XVE1_mg.error.txt | `/Users/rabbit/034_local/Bosch/Motronic/MEx17/UDS/MED 17.1.62/8S0907404E_0001/` | More A2L parse/error logs for MED17.1.62 calibration variants. |

## Bosch TCU (AL551 / AL552 / DQ500 / DQ381)

| Filename | Path | Brief purpose |
|---|---|---|
| ZF_AL551_asis2_AUAJ20B0.pdf | `/Users/rabbit/034_local/Bosch TCU/AL551/` (mirror at `Unzipped/Bosch TCM/AL551/`) | ZF AL551 transmission ECU "asis2" calibration documentation, ECU SW AUAJ20B0. |
| ZF_AL551_ABK_AUAJ20B0.pdf | same path | ZF AL551 ABK (Abnahmebescheinigung / acceptance certificate) for AUAJ20B0. |
| ZF_AL551_sabal_AUAJ20B0.pdf | same path | ZF AL551 SABAL (calibration label) for AUAJ20B0. |
| AL551 8 FE 8HP55 asis2_AUAJ20B0_SKRZYNIA_ZF.pdf | same path | AL551 / 8HP55 transmission "asis2" cal doc (Polish: SKRZYNIA = gearbox). |
| pcmflash_96.pdf | `/Users/rabbit/034_local/Bosch TCU/AL552/` (mirror at `Unzipped/Bosch TCM/AL552/`) | PCMflash module 96 description (AL552 transmission family). |
| AL552 Mod Cal Flash - 19-05-21_12-54-35_CANlog.txt | `/Users/rabbit/034_local/Bosch TCU/AL552/` (and `.../AL552 Mod Cal Flash - 19-05-21_12-54-35_CANlog_parsed/`) | CAN log of an AL552 mod-cal flash session from 2021-05-19. |
| 0014C32_ZX8M3200_ghidraSymbols.txt | `/Users/rabbit/034_local/Bosch TCU/AL551/0014C32_ZX8M3200/` | Ghidra symbol export for AL551 SW 0014C32 / ZX8M3200. |
| 0014C44_ZX8L4400_ghidraSymbols.txt | `/Users/rabbit/034_local/Bosch TCU/AL551/0014C44_ZX8L4400/` | Ghidra symbol export for AL551 SW 0014C44 / ZX8L4400. |
| 2738C48_VAAQ50B0_ghidraSymbols.txt | `/Users/rabbit/034_local/Bosch TCU/AL552/VAAQ50B0/` | Ghidra symbol export for AL552 SW 2738C48 / VAAQ50B0. |
| 0DL300011N_2014_  TCMDQ500021 - Original E2A5.txt | `/Users/rabbit/034_local/Bosch TCU/DQ/DSG DQ500 (Autotuner File)/` (mirror at `Bosch/TCU/DQ/...`) | DQ500 TCM file PN 0DL300011N S2014, original CRC E2A5 — Autotuner export notes. |

## Aisin Transmission ECU

Mirrored: `/Users/rabbit/034_local/Dokumentation/Aisin Flashing Info/` and `/Users/rabbit/034_local/Unzipped/Aisin Flashing Info/`.

| Filename | Brief purpose |
|---|---|
| atsg_09g_09m_eng_AISIN_TipTronic_09G_Data.pdf | ATSG (Automatic Transmission Service Group) doc on Aisin 09G/09M TipTronic transmission. |
| pcmflash_94.pdf | PCMflash module 94 description (Aisin 09G/09M). |

## Delphi DCM

Mirrored: `/Users/rabbit/034_local/Dokumentation/Delphi DCM/` and `/Users/rabbit/034_local/Unzipped/Delphi DCM/`.

| Filename | Brief purpose |
|---|---|
| pcmflash_90.pdf | PCMflash module 90 description (Delphi DCM family). |

## m232-master (AAN Audi I5 turbo tuning project)

Mirrored across `/Users/rabbit/034_local/Dokumentation/m232-master/` and `/Users/rabbit/034_local/Unzipped/m232-master AAN Tuning (PRJ)/m232-master/m232-master/`.

| Filename | Brief purpose |
|---|---|
| README.md | Top-level README for the m232-master open-source project (Audi I5 AAN engine tuning via Bosch M2.3.2 ECU). |
| ECU/README.txt | Readme for the ECU bin dumps shipped with the project. |
| Factory data/M2_3_2 Schematics.pdf | Bosch M2.3.2 ECU electrical schematics (factory data). |
| Factory data/AAN_ABH_manual.pdf | AAN engine ABH (Allgemeine Betriebs-Handbuch / general operations manual). |
| Factory data/5231830_Adaptive_closed_loop_knock_contr.pdf | Bosch internal publication 5231830 — "Adaptive closed-loop knock control" paper. |

## Damos and Defs (Non-VAG)

Damos symbol-list `.txt` exports from various non-VAG calibrations under `/Users/rabbit/034_local/Damos and Defs/`. These are not OEM PDFs but plain-text symbol listings useful as reference.

| Filename | Brief purpose |
|---|---|
| FORD/PUMA24c_A2_FRQ020_ra.txt | Ford Puma (2.4) calibration symbol export A2/FRQ020. |
| FORD/PUMA32c_A2_FRP020_ra.txt | Ford Puma (3.2) calibration symbol export A2/FRP020. |
| OPEL M2.7 CALIBRA 2.0 TURBO C20LET OLS/C20LETDO.txt | Opel Calibra 2.0 Turbo C20LET (M2.7 ECU) Damos export. |
| OPEL ME7.6.2 ASTRA 1.4 Z14XEP06/32120301.txt | Opel Astra 1.4 Z14XEP (ME7.6.2) calibration 32120301 export. |
| RENAULT FULL/renault.txt | Renault (generic) Damos export. |
| VAG ME7.1 AUDI TT 225/TT225.txt | Audi TT 225 ME7.1 Damos export. |
| VAG ME7.5 NOT SORTED/22ib_TLEV/fdeflist_22ib.txt | ME7.5 fdef list (22ib TLEV). |
| VAG ME7.5 NOT SORTED/24B8L-PhaseIn/fdeflist_24b8.txt | ME7.5 fdef list (24B8L Phase-In). |
| VAG PPD1.1 VW PASSAT 2.0TDI/offset 800000.txt | PPD1.1 Passat 2.0TDI Damos offset note. |
| Damos EU6-EU5 VAG 3.0 CR TDI 204 PS 258PS Adblue EDC17CP44/.../*.txt (~7 files) | Touareg 3.0 CR TDI EDC17CP44 Damos / cal-version notes (CUC / RGN / Emi variants). |

(Bin/.ols/.dam files in this tree are excluded from the inventory.)

## Internal notes / CAN logs

| Filename | Path | Brief purpose |
|---|---|---|
| AMAX Explained.txt | `/Users/rabbit/034_local/Unzipped/` (mirror at `Dokumentation/`) | Internal note on the AMAX field/object. |
| CAN msg snapshot, mk7 GOlfR.txt | `/Users/rabbit/034_local/DBCs/` | Snapshot of CAN messages observed on MK7 Golf R. |
| Fresh dumps/03-10-25_04-51-28_CANlog_t7readMod.txt | `/Users/rabbit/034_local/Unzipped/Fresh dumps/` (mirror at `Dokumentation/`) | CAN log of a "t7 read mod" session, 2025-03-10. |
| Fresh dumps/03-10-25_04-51-28_CANlog_t7toMod.txt | same | CAN log of a "t7 to mod" flash session. |
| Fresh dumps/03-10-25_04-51-28_CANlog_t7toStock.txt | same | CAN log of a "t7 to stock" revert session. |
| Fresh dumps/.../log.txt (multiple) | same parent dirs | Per-session log file for each CAN trace folder. |
| 20240822 AL551 AL552/Request 2024 Eric.txt | `/Users/rabbit/034_local/Unzipped/Fresh dumps/` (mirror at `Dokumentation/`) | Customer/internal request note from 2024-08-22. |
| 8W0907559H S0009/Customer Recovery/Brian Henderson - WAUB4AF44JA001898/notes.txt | `/Users/rabbit/034_local/PowerPC (1)/` | Customer recovery notes for VIN WAUB4AF44JA001898 — likely contains VIN; do not transcribe in external docs. |
| how-to-use-backup.html (multiple) | various `Customer Recovery/` and `Customer Backup/` subdirs under `2.9TT B9 MG1/` and `PowerPC/80A907559J S0001/` | Bench-flash-tool generated "how to use backup" instructions accompanying customer ECU backups. |
| index.html (CANlog parsed dirs, multiple) | `Bosch TCU/AL552/...`, `MG1 (1)/2.9T MG1.../`, `MG1 (2)/2.9T MG1.../`, `MG1 (1)/B9_3.0T_OBD_parsed.../`, `MG1 (2)/B9_3.0T_OBD_parsed.../`, `Unzipped/Fresh dumps/.../` (and `Dokumentation/` mirrors) | Auto-generated HTML index for each CAN log parser output dir. |
| Hyundai Theta II FDEF_691F00.pdf | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | Reverse-engineered FDEF (function definition) dump of Hyundai Theta II ECU at offset 0x691F00. Only non-VAG OEM-protocol PDF in the archive. |
| ECUx Comm Log.xls | `/Users/rabbit/034_local/Dokumentation (1)/Protocols/` | ECU communication log (Excel). Not a PDF/text doc but worth knowing about. |

## Flash Client (034 internal tooling)

All under `/Users/rabbit/034_local/Flash Client/Flash History Data/` (with one item under `iOS sqlite Build error/`). These are import-results / conversion-results log files, not narrative docs — but they are text-format and may be useful as reference.

| Filename | Brief purpose |
|---|---|
| Flash History nstuart 01-01-2017 - 08-23-2021.csv.importResults.txt | Result log from importing 034 user "nstuart" flash history CSV. |
| Flash History nstuart 01-01-2017 - 08-23-2021.csv.importResults-2.txt | Second-pass import result. |
| Flash History nstuart 01-01-2017 - 08-23-2021.csv.importResults.txt.MissingUsers.csv.importResults.txt | Missing-users subset import result. |
| Flash History sbloom 01-01-2017 - 10-08.21csv.csv.importResults.txt | Import result for user "sbloom" flash history. |
| Flash History sbloom 09192021_093454.csv.importResults.txt | Import result, sbloom 2021-09-19 export. |
| Flash History sbloom 09272021_091143.csv.importResults.txt | sbloom 2021-09-27 export. |
| Flash History sbloom 09072021-09152021.csv.importResults.txt | sbloom 2021-09-07 to 09-15 range export. |
| Flash History sbloom 09172021_074978.csv.importResults.txt | sbloom 2021-09-17 export. |
| Master Credential Log - General - 100721.csv.importResults.txt | Credential-log import results from 2021-10-07. **Likely contains user identifiers — handle as sensitive.** |
| Master Credential Log - General.csv.importResults.txt | Same, undated. |
| missingUsers.txt.grouped.txt | Grouped list of missing users from import. |
| imports/DEV/conversionResults.txt | DEV-env conversion results log. |
| imports/Ostrich - 09072021/conversionResults.txt | Ostrich import conversion results (2021-09-07). |
| iOS sqlite Build error/clang++ exited with code 1.txt | iOS Flash Client sqlite-related clang++ build-error log. |

---

## Sensitive content notes

- `/Users/rabbit/034_local/PowerPC (1)/8W0907559H S0009/Customer Recovery/Brian Henderson - WAUB4AF44JA001898/notes.txt` is filed under a path containing a customer name and a 17-character VIN. Do not transcribe the VIN or name into FUTV1.1 source/docs.
- `Flash Client/Flash History Data/Master Credential Log - General*.csv.importResults.txt` likely contains user identifiers / flash credentials. Treat as sensitive — referenced by path only here.
- The MG1CS001/002/003/008/011 SWCalDoc PDFs and the `Freigabe-Motorsteuergerät_8V0907115_D_Y628.pdf` are confidential Bosch / VW corporate documents. They are catalogued by filename/path for findability; do not redistribute outside the FUTV1.1 working environment.
- No AES keys, seed/key constants, or VIN strings have been transcribed into this inventory — only filenames and paths.

[MAC]
