# A2L → firmware logger catalog generator

This pipeline turns a Bosch `.a2l` calibration description into the C
header that ships in the firmware's logger module. It exists so the
firmware's per-boxcode logger variable list (addresses, sizes, scaling,
units) is **derived from authoritative ECU calibration data**, not
hand-curated. Hand-curated catalogs drift; A2L doesn't (it's emitted by
Bosch's calibration toolchain alongside the binary it describes).

Scope: read-only consumer of `.a2l`. This pipeline does NOT modify the
A2L. Per project policy A2L files stay on Sean's Desktop (likely
contain proprietary tuning data) — only the derived JSON and the
emitted `.h` are committed.

---

## What each script does

| Script | Reads | Writes | Purpose |
|---|---|---|---|
| `a2l_parser.py` | `.a2l` (raw) | (importable: `parse_a2l() → dict`) | Streaming A2L tokenizer. Extracts MEASUREMENT + COMPU_METHOD blocks and resolves linear / RAT_FUNC scale + offset. Library, not normally run directly — running it as `__main__` is a debug probe that dumps a few named vars to stdout. |
| `dump_a2l_catalog.py` | `.a2l` | `.json` (sorted by address) | Filters `parse_a2l()` output to "loggable" types (1/2/4-byte integer or `FLOAT32_IEEE`), requires `ECU_ADDRESS`, skips arrays (`MATRIX_DIM > 1`), and emits one JSON record per surviving MEASUREMENT. This is the **per-(boxcode, calibration) source-of-truth catalog** — checked in at `firmware/src/logger/catalogs/<boxcode>__<cal>.json`. |
| `match_ui_catalog.py` | `ui/control_panel.js` (extracts `ECU_VAR_DB`) + A2L JSON | `.tsv` (transient) | Reconciles the UI's hand-curated `ECU_VAR_DB` against the A2L catalog so the firmware exposes the subset of A2L names the UI actually knows how to display. Match strategy: exact → `_msg`-suffix (Bosch CAN-published convention) → case-insensitive → shortest-substring. Output is a flat TSV with a `confidence` column. |
| `emit_firmware.py` | A2L JSON + match TSV | C header (`logger_catalog_<boxcode>__<cal>.h`) | Emits a `static const logger_variable_def_t[]` array that matches `firmware/src/logger/logger_variables.h`'s schema. Only `exact` and `msg_suffix` matches are emitted by default; `substring` confidence is too weak to ship without human review. `--include-missing` adds UI-catalog rows that A2L didn't match (legacy SEFI addresses) for diagnostic builds. |

---

## Inputs

- **`.a2l` source.** Lives on Sean's Desktop; per CLAUDE.md Rule 5 the
  file does NOT enter the repo. Pass the absolute path on the command
  line.
- **Boxcode + calibration tag.** Two strings that together form the
  catalog's identity (e.g. `4K0907557G__0003` + `MA22G01`). The
  boxcode comes from `docs/boxcode_database.md`; the calibration tag
  is the Bosch internal name, usually visible in the `.a2l` header
  block.
- **UI catalog.** Lives at `ui/control_panel.js` as a `const
  ECU_VAR_DB = [ … ]` literal. The matcher pulls it out of the
  JavaScript with a regex + JSON-ish coercion (so don't refactor that
  literal into a fetched JSON without updating `extract_ui_catalog()`).
- **Required-var policy.** `emit_firmware.py` hard-codes the set of
  always-included variables per boxcode in `REQUIRED_NAMES_BY_BOXCODE`
  (currently: `nmot_w`, `InjSys_ratEthPrtnBascFu`, `Com_stCrCtlPan`
  for the dev RS7). This is firmware policy, not an A2L property —
  it stays inside `emit_firmware.py` for now. When the supported
  boxcode matrix grows, the right move is a sibling JSON/YAML
  manifest; flagging here so it doesn't get forgotten.

## Outputs

| Path | Tracked | Lifecycle |
|---|---|---|
| `firmware/src/logger/catalogs/<boxcode>__<cal>.json` | yes | Regenerated on A2L change |
| `firmware/src/logger/catalogs/generated/logger_catalog_<boxcode>__<cal>.h` | yes | Regenerated on JSON change |
| `firmware/src/logger/A2L_DID_CATALOG_<boxcode>.md` | yes | Hand-curated audit overview |
| `firmware/src/logger/A2L_DID_AUDIT.md` | yes | Hand-curated drift findings |
| match TSV | no (transient) | Built per-regen; safe to drop in `/tmp` |

---

## End-to-end regen for an existing boxcode

If the input JSON is already checked in (the usual case — A2L hasn't
changed, you're just iterating on the matcher or required-vars), skip
steps 1–2 and re-run from step 3.

```bash
cd ~/esp/obd/FUTV1.1
BOXCODE=4K0907557G__0003
CAL=MA22G01
A2L=~/Desktop/${BOXCODE}__${CAL}.a2l        # path on YOUR machine; not in-tree
CATALOG_JSON=firmware/src/logger/catalogs/${BOXCODE}__${CAL}.json
MATCH_TSV=/tmp/${BOXCODE}_match.tsv
OUT_H=firmware/src/logger/catalogs/generated/logger_catalog_${BOXCODE}__${CAL}.h

# 1. Parse the A2L → loggable-MEASUREMENT JSON.
python3 tools/a2l_to_catalog/dump_a2l_catalog.py "$A2L" "$CATALOG_JSON"

# 2. Reconcile against the UI catalog → match TSV (transient).
python3 tools/a2l_to_catalog/match_ui_catalog.py \
    ui/control_panel.js "$CATALOG_JSON" "$MATCH_TSV"

# 3. Emit the firmware header.
python3 tools/a2l_to_catalog/emit_firmware.py \
    --a2l-json "$CATALOG_JSON" \
    --match-tsv "$MATCH_TSV" \
    --boxcode "$BOXCODE" \
    --calibration "$CAL" \
    --out "$OUT_H"

# 4. Build firmware to confirm the header compiles.
cd firmware && . ~/esp/esp-idf/export.sh && idf.py build
```

`logger_variables.c` `#include`s the generated header — no further
manual wiring once `$OUT_H` lands at the expected path.

## Adding a new boxcode

1. Drop the new `.a2l` on your Desktop (do NOT commit it).
2. Add the boxcode row to `docs/boxcode_database.md` and `.json`.
3. Pick a calibration tag and set the two env vars above.
4. Add an entry to `REQUIRED_NAMES_BY_BOXCODE` in `emit_firmware.py`
   covering the boxcode's must-have vars (RPM / ethanol / cruise
   today — revise as Phase 1 features expand).
5. Run the regen sequence above.
6. Update the per-boxcode audit doc
   `firmware/src/logger/A2L_DID_CATALOG_<boxcode>.md` summarizing
   exact/`_msg`-suffix/substring/missing counts from the matcher.
7. Update `CMakeLists.txt` if the build needs to switch which
   `<boxcode>__<cal>.h` it includes per target. (As of 2026-06-06
   the build is single-target.)

## Verifying confidence

`match_ui_catalog.py` prints a confidence breakdown on stdout:

```
UI catalog size: 56
Match confidence breakdown:
  exact        32
  msg_suffix   15
  case_only    0
  substring    1
  missing      8
wrote /tmp/4K0907557G__0003_match.tsv
```

`emit_firmware.py` only emits `exact` + `msg_suffix` matches by
default. The other buckets:

- **case_only**: rare — the matcher accepts these but flags them; if
  you see any, look at whether the UI name's case is wrong.
- **substring**: weak; intentionally skipped at emit time. Investigate
  each substring hit before deciding whether the UI name should be
  renamed to match A2L or whether the substring is a false positive.
- **missing**: the UI references a name not in A2L. Two causes: (a)
  legacy SEFI-era hand-curated address that A2L doesn't have under
  any name, in which case keep using the legacy address via a manual
  override (today: a few rows in `logger_variables.c` upstream of
  the generator) or accept that the UI will display "no data" for
  that var on the new boxcode; (b) name typo on the UI side — fix the
  UI.

## Cleanup status (P-74 PHASE E, 2026-06-06)

- `a2l_parser.py`: dead `/end` conditional removed (lines 96–100 in
  the pre-cleanup file). No behavior change — the conditional only
  contained `pass # handled below`.
- `dump_a2l_catalog.py`: the always-true `--require-address` flag was
  a no-op (`action="store_true", default=True` with no escape hatch)
  — removed. The require-address check itself is still there and
  always-on; the CLI surface just stops claiming to be configurable.
- `emit_firmware.py`: header preamble referenced a `make catalogs`
  target that doesn't exist (Rule 7 vapor). Replaced with a pointer
  to this README. The generated `.h` was regenerated against the
  current JSON; the only byte diff is the preamble comment.
