# A2L DID catalog — boxcode `4K0907557G__0003`

> P-55 metadata extract. **A2L source file stays at `~/a2l/`
> (`DMG1008PH2C1795_MA22G01_Out.a2l`) — not copied into the repo
> per SRM IP rule.** Only the per-variable lookup info needed by
> the firmware logger ships here.

| field | value |
|---|---|
| A2L file | `DMG1008PH2C1795_MA22G01_Out.a2l` (release tag `MA22G01`) |
| A2L project | `P1795` (DMG1008PH2 / Audi MDG1 / Bosch MG1) |
| Boxcode | `4K0907557G__0003` (Sean's dev RS7) |
| ECU byte order | little-endian (Aurix Tricore) |

The A2L was disambiguated by matching `nmot_w` ECU_ADDRESS:

| A2L release | nmot_w ECU_ADDRESS | matches 4K0907557G__0003? |
|---|---|---|
| `MA19FB3` | 0x60020690 | no |
| `MA22FM1` | 0x600206F0 | no |
| **`MA22G01`** | **0x60020618** | **yes** (firmware table) |

---

## MEASUREMENT records (6 logger vars)

All addresses + sizes + scales below cross-checked against the
firmware's `VARIABLES_4K0907557G__0003[]` table in
`logger_variables.c`. **Every address and every size matches today.**
The bugs are in the parser, not the table — see "Parser-side bugs"
below.

### `nmot_w` — Engine speed

| A2L field | value |
|---|---|
| Description | "Motordrehzahl" |
| Data type | `UWORD` (unsigned 16-bit) |
| COMPU_METHOD | `nmot_uw_q0p25` |
| Range | 0.0 to 16383.75 rpm |
| ECU_ADDRESS | `0x60020618` |
| Formula | `phys = raw * 0.25` rpm |

### `InjSys_ratEthPrtnBascFu` — Ethanol content in base fuel

| A2L field | value |
|---|---|
| Description | "Ethanolgehalt im Basiskraftstoff" |
| Data type | `UWORD` (unsigned 16-bit) |
| COMPU_METHOD | `rel_uw_b100` |
| Range | 0.0 to 99.99847 % |
| ECU_ADDRESS | `0x6001522A` |
| Formula | `phys = raw / 655.36` % (≈ raw * 0.0015259) |

### `Com_stCrCtlPan` — Cruise control / ACC panel state

| A2L field | value |
|---|---|
| Description | "Cruise Control/ACC Bedienteilstatus" |
| Data type | `UWORD` (unsigned 16-bit) |
| COMPU_METHOD | `OneToOne` |
| Range | 0 to 65535 (state code) |
| ECU_ADDRESS | `0x600206F8` |
| Formula | `phys = raw` (identity) |

### `rl_w` — Relative air charge (actual load, word)

| A2L field | value |
|---|---|
| Description | "Relative Luftfüllung (Word)" |
| Data type | `UWORD` (unsigned 16-bit) |
| COMPU_METHOD | `rel_uw_q0p0234` |
| Range | 0.0 to 1535.977 % |
| ECU_ADDRESS | `0x60015660` |
| Formula | `phys = raw / 42.667` % (≈ raw * 0.0234375) |

### `tmot_msg` — Engine coolant temperature (CAN-messaged)

| A2L field | value |
|---|---|
| Description | "Motor-Temperatur" |
| Data type | `UBYTE` (unsigned 8-bit) |
| COMPU_METHOD | `temp_ub_q0p75_o48` |
| Range | -48.0 to 143.3 °C |
| ECU_ADDRESS | `0x6001BF38` |
| Formula | `phys = (raw - 64) / 1.333` = `raw * 0.75 - 48` °C |

Note: the firmware names this variable `tmot` (no `_msg` suffix).
There is no plain `tmot` MEASUREMENT in this A2L — `tmot_msg` is
the messaged/broadcast copy and is the correct target.

### `wdkba_msg` — Throttle position (relative to lower stop, CAN-messaged)

| A2L field | value |
|---|---|
| Description | "Drosselklappenwinkel bezogen auf unteren Anschlag" |
| Data type | `UBYTE` (unsigned 8-bit) |
| COMPU_METHOD | `relDK_ub_b100` |
| Range | 0.0 to 100.0 % |
| ECU_ADDRESS | `0x6001B842` |
| Formula | `phys = raw / 2.55` % (≈ raw * 0.3922) |

Same `_msg` note as `tmot_msg`.

---

## COMPU_METHOD reference (ASAP2 RAT_FUNC semantics)

ASAP2 RAT_FUNC encodes the **physical → internal** mapping as
`internal = (a·p² + b·p + c) / (d·p² + e·p + f)`. For linear
LINEAR-style entries we have `a = d = e = 0, f = 1`, simplifying to
`internal = b·p + c`. Solving for physical: `phys = (internal - c) / b`.

| COMPU_METHOD | COEFFS [a b c d e f] | Solved formula |
|---|---|---|
| `nmot_uw_q0p25` | 0 4.0 0.0 0 0.0 1.0 | `phys = raw / 4` = `raw * 0.25` rpm |
| `rel_uw_b100` | 0 655.36 0.0 0 0.0 1.0 | `phys = raw / 655.36` % |
| `OneToOne` | 0 1 0 0 0 1 | `phys = raw` |
| `rel_uw_q0p0234` | 0 42.667 0.0 0 0.0 1.0 | `phys = raw / 42.667` % |
| `temp_ub_q0p75_o48` | 0 1.333 64.0 0 0.0 1.0 | `phys = (raw - 64) / 1.333` = `raw * 0.75 - 48` °C |
| `relDK_ub_b100` | 0 2.55 0.0 0 0.0 1.0 | `phys = raw / 2.55` % |

The Bosch suffix-naming convention decodes as:
- `q<X>` = quantization scale (e.g., `q0p25` = 0.25 per LSB)
- `o<N>` = offset (e.g., `o48` = -48 added after scaling)
- `b<N>` = "0..N%" or "0..N range" (e.g., `b100` = byte/word mapped to 0..100 %)
- `ub` / `uw` = unsigned byte / unsigned word
- `sb` / `sw` = signed byte / signed word

All 6 of our logger vars are **unsigned** (`ub` or `uw`) and follow
the linear form `phys = raw * scale + offset`. Translating to
firmware `(scale, offset)` pairs:

| var | scale | offset | signed? | size | endian |
|---|---|---|---|---|---|
| `nmot_w` | 0.25 | 0 | no | 2 | LE |
| `InjSys_ratEthPrtnBascFu` | 0.00152587890625 (=1/655.36) | 0 | no | 2 | LE |
| `Com_stCrCtlPan` | 1.0 | 0 | no | 2 | LE |
| `rl_w` | 0.0234375 (=1/42.667) | 0 | no | 2 | LE |
| `tmot` | 0.75 | -48.0 | no | 1 | LE |
| `wdkba` | 0.392156862745098 (=1/2.55) | 0 | no | 1 | LE |

---

## Parser-side bugs (P-55 root cause)

The MEASUREMENT addresses and the scale/offset values in
`logger_variables.c::VARIABLES_4K0907557G__0003[]` are **correct**.
The KOEO baseline failures trace to three bugs in
`logger_config.c::logger_config_parse_poll_response`:

### Bug 1 — signedness hardcoded

```c
if (var->size == 1) {
    raw_value = (int8_t)response[offset];          // sign-extends
} else if (var->size == 2) {
    raw_value = (int16_t)(... LE 16-bit ...);      // sign-extends
}
```

The cast is unconditionally signed. Every byte ≥ 0x80 in a single-
byte field, and every high-bit-set word, gets decoded as negative.
For `nmot_w` raw = 0x961B (unsigned 38427) the parser produces
-27109, which scales to -6777.25 rpm in the KOEO observation.

The A2L declares all 6 of the dev RS7's logger vars as `UWORD` or
`UBYTE` (unsigned). The `logger_variable_def_t` struct in
`logger_variables.h` already has an `is_signed` field, but it is
never plumbed into `logger_variable_t` (the runtime parser struct).

### Bug 2 — formula structure inverted

```c
values_out[var_idx] = (raw_value + var->offset) * var->scale;
```

This computes `phys = (raw + offset) * scale = scale·raw + scale·offset`.
The A2L's `temp_ub_q0p75_o48` resolves to `phys = raw * 0.75 - 48`,
which is `phys = scale·raw + offset`, not `phys = scale·(raw + offset)`.

The two forms agree iff `offset == 0`, which is why only `tmot`
suffers a constant 12 °C error today (the other five vars all carry
offset = 0). At raw = 91 (a typical ambient): current parser
`(91 - 48) * 0.75 = 32.25` °C vs A2L-correct `91 * 0.75 - 48 = 20.25` °C.

### Bug 3 — byte order not plumbed per-boxcode

`boxcode_config_t` carries an `is_big_endian` field that is never
read. The parser hardcodes little-endian. The current
`4K0907557G__0003` is little-endian (Aurix), so for this boxcode the
hardcoded path happens to match — but adding `8W0907559H__0008`
(declared `is_big_endian = 1` in the table) means future boxcodes
silently misparse. Fix in the same pass while we are touching the
parser.

---

## Source provenance

A2L file: `~/a2l/DMG1008PH2C1795_MA22G01_Out.a2l` (47.9 MB,
unchanged on disk). Stays out-of-repo per CLAUDE.md proprietary-IP
rule. The MEASUREMENT line numbers above were captured 2026-05-22.
