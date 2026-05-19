# MED17 → MG1 Variable Cross-Reference

**Purpose:** Maps variables from the older MED17 ECU family (282 entries from a tuning CSV) to their MG1 equivalents. Used as a "nice to have" reference when expanding boxcode variable catalogs.

**Cross-reference method:** Each MED17 variable's base name was searched against `DMG1008PH2C1795_MA22G01_Out.a2l` MEASUREMENT blocks.

**Results:** ~52 of 213 unique base names found (24.4% hit rate)

---

## High-Confidence Matches (Identical or Near-Identical Names)

These exist in the MG1 A2L and can be added directly once RAM addresses are confirmed per boxcode.

| MED17 Name | MG1 A2L Name | Category | Notes |
|-----------|-------------|----------|-------|
| `nmot_w` | `nmot_w` | Engine | Engine speed — already in all boxcodes |
| `rl_w` | `rl_w` | Engine | Actual load — already in 4K0/4M0B |
| `tmot` | `tmot` | Temperature | Coolant temp — already in 4K0/4M0B |
| `pvdg_w` | `pvdg_w` | Boost | Boost pressure — already in 4K0/4M0B |
| `frm_w` | `frm_w` | Fuel | STFT — already in 4K0/4M0B |
| `zwoutzyl_w` | `zwoutzyl_w` | Ignition | Timing — already in 4K0/4M0B |
| `wdkba` | `wdkba` | Throttle | Throttle position — already in 4K0/4M0B |
| `gang` | `gang` | Transmission | Gear — already in 8W0 |
| `lambts` | `lambts` | Lambda | Lambda B1 — already in 8W0 |
| `vfzg` | `vfzg` | Vehicle | Vehicle speed |
| `ubatt_w` | `ubatt_w` | Electrical | Battery voltage |
| `iatpd_w` | `iatpd_w` | Temperature | Intake air temp (pre-throttle) |
| `iat_w` | `iat_w` | Temperature | Intake air temp |
| `mshfm_w` | `mshfm_w` | Air | Air mass (HFM) |
| `dzwi_w` | `dzwi_w` | Ignition | Individual cylinder retard |
| `frl` | `frl` | Fuel | Fuel rail pressure limit |
| `prail_w` | `prail_w` | Fuel | Fuel rail pressure actual |
| `drlp_w` | `drlp_w` | Engine | Load delta |
| `rlmx_w` | `rlmx_w` | Engine | Max predicted load |
| `rlsol_w` | `rlsol_w` | Engine | Requested load |
| `puvdg_w` | `puvdg_w` | Boost | Ambient pressure |
| `psol_w` | `psol_w` | Boost | Requested boost |
| `mdzul_w` | `mdzul_w` | Torque | Permitted torque |
| `mdverlr_w` | `mdverlr_w` | Torque | Driver torque request |
| `mdbas_w` | `mdbas_w` | Torque | Base torque |
| `etacib_w` | `etacib_w` | Efficiency | Combustion efficiency |

## Naming Convention Differences (Partial Matches)

MED17 uses abbreviated shorthand; MG1 uses longer structured names:

| MED17 Name | Likely MG1 Equivalent | Status |
|-----------|----------------------|--------|
| `BoostPres` | `pvdg_w` | Alias — already mapped |
| `ChgAirPres` | `puvdg_w` or `pvdg_w` | Context-dependent |
| `CrkShaftSpd` | `nmot_w` | Alias |
| `AirChgCalc` | `rlp_w` / `rl_w` | Alias |
| `IgnAdv` | `zwoutzyl_w` / `zwoausk_w` | Timing family |
| `LamCorr` | `frm_w` / `frm` | STFT family |
| `ExhGasTemp` | `tabgk_w` / `tabg_w` | EGT — exists in A2L |
| `MisFireCnt` | Misfiring counters | Exist as `AUSmot_cntMsfDs*` |

## Variables NOT Found in MG1 (100% Miss Categories)

| Category | Example Variables | Reason |
|---------|------------------|--------|
| Group 17 (aftermarket) | `WGComp`, `BoostError`, `mapswitch`, `FlexCont`, `AFRTarget` | Virtual variables from tuning tools, not in ECU firmware |
| OBD-II PIDs | `$01`, `$05`, `$0B`, `$0C` | Standard OBD PIDs — accessed via different UDS services, not RAM reads |
| Turbo-specific | `WstGtDCy`, `N75DuCy` | MED17 wastegate names; MG1 uses `ldrue_w` family |

## Recommended A2L Additions for Future Boxcodes

From the A2L analysis, these MG1 MEASUREMENT variables are high-value additions that weren't in the original boxcode catalogs:

### Tier 1 — High Priority
| Variable | Description | Size | Notes |
|----------|------------|------|-------|
| `AUSngd_facKnkAdjInd_C1` | Knock retard cyl 1 | 2B | Per-cylinder knock detection |
| `AUSngd_facKnkAdjInd_C2-C8` | Knock retard cyl 2-8 | 2B | Full V8 knock visibility |
| `prail_w` | Fuel rail pressure | 2B | Critical for DI engines |
| `vfzg` | Vehicle speed | 2B | Useful for speed-based logging |
| `ubatt_w` | Battery voltage | 2B | Electrical health |
| `tabgk_w` | EGT (pre-cat) | 2B | Thermal protection |
| `AUSmot_cntMsfDs*` | Misfire counters | 2B | Per-cylinder misfire |

### Tier 2 — Medium Priority
| Variable | Description | Notes |
|----------|------------|-------|
| `mshfm_w` | Air mass flow | Airflow verification |
| `mdverlr_w` | Driver torque request | Torque analysis |
| `mdzul_w` | Permitted torque | Torque limiting |
| `iat_w` / `iatpd_w` | Intake air temps | Charge air cooling |
| `psol_w` | Target boost | Boost control tuning |
| `rlsol_w` | Requested load | Load request analysis |

### Tier 3 — Nice to Have
| Variable | Description | Notes |
|----------|------------|-------|
| `etacib_w` | Combustion efficiency | Advanced tuning |
| `drlp_w` | Load delta | Load error tracking |
| `dzwi_w` | Individual timing retard | Fine-grained timing |
| `frl` | Rail pressure limit | Fuel system limits |
| `rlmx_w` | Max predicted load | Load ceiling |

---

## Usage Notes

- All variables require **per-boxcode RAM address lookup** — the names are the same across MG1 firmwares but addresses differ
- Adding new variables requires updating `logger_variables.c` with the correct address for each boxcode
- The `LOGGER_MAX_VARIABLES` constant (currently 32) limits how many can be active simultaneously on the ECU patch
- Use the **logger profile system** (`set_logger_profile` command) to select which subset the user wants
