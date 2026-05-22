# A2L ↔ firmware DID-table audit — 2026-05-22

> P-55 offline-portion audit. Done autonomously during overnight
> Phase α run.
>
> **Status: AUDIT INCOMPLETE — no A2L on disk for boxcode
> `4K0907557G__0003`.** Findings + ask documented below.

---

## Goal

Cross-check the firmware's hardcoded logger DID/address table
(`firmware/src/logger/logger_variables.c::variable_definitions`)
against the canonical Bosch MG1 A2L for the dev RS7 boxcode
`4K0907557G__0003`. Catch any wrong RAM address, wrong scale,
wrong offset, or wrong endianness that would explain P-55's
KOEO `nmot_w = -5369 rpm` reading (impossible — engine OFF must
yield 0).

## Source materials available

| Source | Path | Useful for this audit? |
|---|---|---|
| A2L file (canonical) | NOT ON DISK | — |
| XDF (TunerPro calibration map) | `~/esp/obd/SEFIV1.0/Customers/Sean Cyr/C8 RS7/SEFI Original Files/4K0907557G__0003.xdf` | Partial — gives MAP scales + ECU-binary file addresses, not RAM addresses |
| Config.json (Scorpion build manifest) | `~/esp/obd/SEFIV1.0/EXE TOOL/data/4K0907557G__0003/input/config.json` | Has 3 `labels` entries (ethanol, engine_speed_display, max_load_1) — names what the patcher edits; not a DID table |
| Stage1 .stf (human-readable patched binary) | `~/esp/obd/SEFIV1.0/EXE TOOL/data/4K0907557G__0003/output/stage1_patched.stf` | Maps/segments output. Could carry symbol-to-address hints; not parsed yet. |
| logger_variables.c | `firmware/src/logger/logger_variables.c` lines 17-23 | Current firmware-side DID table for 4K0907557G__0003 — what we're auditing AGAINST |

Bottom line: the XDF doesn't include English titles for any of the 6
logger variables (`nmot_w`, `InjSys_ratEthPrtnBascFu`, `Com_stCrCtlPan`,
`rl_w`, `tmot`, `wdkba`) — searches for "Coolant", "Throttle",
"Cruise", "Engine speed" return either nothing or only
constants-table titles like "Maximum engine speed to activation
boost control diagnostic" that are clearly different (a single
RPM limit, not the live RPM signal).

The German-language Bosch symbol names (nmot_w = `n_motor_w`,
tmot = temperature motor, wdkba = Winkel Drosselklappe Anschlag
Berechnet/Ist, etc.) suggest the canonical mapping lives in the
ECU's A2L, not the XDF.

## Firmware DID table (audit target)

From `firmware/src/logger/logger_variables.c` lines 17-23, the
`4K0907557G__0003` entry:

| name | display name | unit | RAM address | size | scale | offset | required |
|---|---|---|---|---|---|---|---|
| `nmot_w` | Engine Speed | rpm | `0x60020618` | 2 | `0.25` | `0` | true |
| `InjSys_ratEthPrtnBascFu` | Ethanol Content | % | `0x6001522A` | 2 | `0.00152587167162585` | `0` | true |
| `Com_stCrCtlPan` | Cruise Control Status | - | `0x600206F8` | 2 | `1.0` | `0` | true |
| `rl_w` | Load (actual) | % | `0x60015660` | 2 | `0.0234375066758221` | `0` | false |
| `tmot` | Coolant Temp | °C | `0x6001BF38` | 1 | `0.749803921568627` | `-48` | false |
| `wdkba` | Throttle Position | % | `0x6001B842` | 1 | `0.392156862745098` | `0` | false |

Signedness: none of the entries declare signed in the struct
visible at line 17 (the boolean is the trailing `false` per the
struct layout; nothing in `logger_variables.h` flips signed
unless I'm missing a field — needs a fresh look at
`logger_variable_t` definition).

## P-55 observation re-stated (chip report, status-2026-05-19)

KOEO observation on dev RS7 via `get_logger_data` over WS:

| variable | observed | expected (KOEO) | plausibility |
|---|---|---|---|
| `nmot_w` | **-5369.25** rpm | 0 (engine off) | IMPOSSIBLE — RPM is unsigned in any sane representation; negative means signedness mis-decoded OR wrong DID entirely |
| `InjSys_ratEthPrtnBascFu` | **-49.99** % | 0-100% | IMPOSSIBLE — ethanol % can't be negative |
| `Com_stCrCtlPan` | 0 | 0 (idle) | plausible |
| `rl_w` | 59.13 % | 0 (engine off) | IMPOSSIBLE — load with engine off should be 0 or undefined |
| `tmot` | **-35.99** °C | ambient (~20-25°C) | IMPOSSIBLE — offset of -48 + scale 0.749… expects `(raw * 0.749) - 48 ≈ ambient`. For ambient = 20°C, raw ≈ 91. We're seeing raw that decodes to -35.99 which implies raw ≈ 16 — implausibly cold. Or the byte is being read as signed when it should be unsigned. |
| `wdkba` | 0.392 % | 0 (no input) | borderline — 0.392 = 1 raw * scale; consistent with raw=1 (off-by-one in idle) |

Pattern: 4 of 6 variables produce IMPOSSIBLE values. P-53 demux fix
landed elsewhere on a different code path (UDS NRC 0x78 drain in
`dtc_uds.c`'s `target_uds_request`); the logger has its own response
pipeline (`logger_manager_handle_poll_response` →
`logger_config_parse_poll_response`) that hasn't been audited for
the same drain pattern. **The same demux conflation could be in
play here.**

The new `get_logger_data_raw` WS command (commit `fed30f1`) will
let an on-car HIL probe surface the bytes the ECU actually returned
pre-parse, so the next HIL pass can discriminate:

- **Pre-parse bytes correct → wrong DID table** (regenerate from
  A2L, NOT from XDF — A2L has the German symbol mapping)
- **Pre-parse bytes look like a stale or shifted response → demux
  conflation** (port P-53's NRC 0x78 drain to logger path)
- **Pre-parse bytes correct but match a different variable → wrong
  service or wrong address mapping for this variant**

## Recommendations for Sean's morning review

1. **Locate the A2L for `4K0907557G__0003`** — likely held by Sean
   or in a Bosch / Volkswagen tuner-toolchain bundle separate from
   the SEFIV1.0 archive. Without it, the audit closes inconclusive.
2. **Once A2L is on disk:** cross-check each of the 6 RAM
   addresses + scale + offset + signedness. Regenerate the
   `4K0907557G__0003` table in `logger_variables.c` if any drift
   surfaces.
3. **Independently:** run the P-55 HIL discriminator (next HIL
   session) using `get_logger_data_raw` to pre-parse-byte capture.
   That tells us whether the bug is the DID table or the parse
   pipeline regardless of A2L availability.

## What was NOT changed

This audit is doc-only. No edits to `logger_variables.c`, no
edits to `logger_config.c`, no edits to any DID/scale/offset values.
Per Sean's directive ("DID table changes affect what gets logged to
customer cars — needs Sean's review"), changes wait for explicit
sign-off after the A2L cross-check.

P-55 stays 🟡 in `PHASE_2_PREREQUISITES.md`. The HIL probe portion
of A2 remains gated on the next on-car session.
