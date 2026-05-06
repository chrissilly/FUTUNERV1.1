# Frozen modules — byte-perfect from FUTV1.0, do not modify

> **Rule.** The files listed below are byte-perfect copies from
> `~/esp/obd/FUTV1.0/firmware_v2/src/...`, which is itself a verified
> carry-forward from `~/esp/obd/SEFIv1/ALBIN/ALBIN_WIP/ALBIN_WIP/sefi_firmware/`.
> They implement the live-tune RAM-update path (scal/bdef/ecu_write)
> that has been validated on Sean's RS7 dev car. They are **frozen**.
>
> Any modification to these files — whitespace, comments, reformatting,
> "modernization," or actual logic change — MUST go through an explicit
> approval ritual (see below). Claude Code agents working in this
> repository are not authorized to modify these files autonomously.
> The eval harness will FAIL if any of these files diverge from the
> hashes recorded here.

---

## The frozen list

| Path | SHA-256 | Bytes |
|------|---------|-------|
| `firmware/src/scal/scal_file.c`       | (see `frozen_modules.sha256`) | 13985 |
| `firmware/src/scal/scal_file.h`       | (see `frozen_modules.sha256`) | 5767  |
| `firmware/src/bdef/bdef_file.c`       | (see `frozen_modules.sha256`) | 14722 |
| `firmware/src/bdef/bdef_file.h`       | (see `frozen_modules.sha256`) | 3819  |
| `firmware/src/ecu_write/ecu_write.c`  | (see `frozen_modules.sha256`) | 12158 |
| `firmware/src/ecu_write/ecu_write.h`  | (see `frozen_modules.sha256`) | 1928  |

The canonical SHA-256 hashes live in `firmware/src/frozen_modules.sha256`
in standard `sha256sum` format. Run `firmware/test/verify_frozen.sh`
from the project root to verify.

---

## Why these specifically

These three modules together implement the full SBF live-tune RAM
update path:

- **`scal/scal_file.c`** — parses SCAL (calibration) files and writes the
  resulting parameter updates to ECU RAM via the UDS write path.
- **`bdef/bdef_file.c`** — parses BDEF (block definition) files that map
  named calibration parameters to ECU RAM addresses for a given variant.
- **`ecu_write/ecu_write.c`** — the low-level UDS write primitive
  (challenge-response, mid-byte handling, big/little-endian dispatch,
  retry logic) that scal and bdef call into.

The combination has been validated on a real RS7 against a real ECU,
end-to-end, with stage 1 / stage 2 / stage 3 SBF switches and ethanol
blend transitions all completing inside the 1.5–2 s spec budget. There
is **no equivalent validation** for any reimplementation, and the cost
of regression is "the dev car's tune misbehaves on the road," which is
unacceptable.

---

## What new code is allowed to do

The frozen modules expose a public API in their `.h` files. New code
(such as the upcoming SBF live-tune orchestrator) **must call this API
unmodified.** Specifically, allowed:

- Importing and calling functions declared in the frozen `.h` files.
- Wrapping their behavior in higher-level orchestration (state-machine
  glue, stage switching, ethanol-aware map selection, telemetry,
  logging around them).
- Adding new files alongside them — e.g., a new `firmware/src/scal/`
  file that calls the existing `scal_file.c` API. New files in the
  same directory are fine; the frozen *files* are the boundary, not
  the directory.

**Not allowed without approval ritual:**

- Any edit to the `.c` or `.h` files in the table above, including
  whitespace, comments, or reformatting.
- "Refactoring" out magic numbers from the frozen files. The
  no-magic-numbers rule (project-wide) is intentionally relaxed for
  these modules because the constants in them encode proven on-car
  behavior. Move them to a header or replace with named macros and you
  may have changed the binary semantics — even if the compiler happens
  to produce the same code today, someone six months from now reads
  the macro and "improves" it.
- Replacing the frozen modules with reimplementations, even
  byte-for-byte ones in a different file.
- "Modernizing" their style, types, error handling, etc.

---

## The approval ritual (if a frozen file genuinely needs to change)

If — and only if — there is a real, documented reason a frozen file
must change (a bug found on car, a new ECU variant requires it, etc.),
the procedure is:

1. **Sean approves in writing** (commit message, email, or status log
   entry). Not a Claude Code session deciding it's a good idea.
2. **The change is reproduced on the dev car** before merge. Not in a
   simulator. The whole reason these are frozen is that they're
   on-car-validated; any change has to clear the same bar.
3. **The new file's hash is added to `frozen_modules.sha256`** and
   `FROZEN_MODULES.md` is updated to reference the new bytes count.
   Old hash is recorded in this file's history section below.
4. **The change is logged** in `~/esp/obd/file-update-YYYY-MM-DD.md`
   with the on-car validation evidence.

Anything short of all four steps is a regression risk and the eval
harness will catch it.

---

## History (record changes here)

| Date       | File | Old hash (first 12) | New hash (first 12) | Reason | Approved by |
|------------|------|---------------------|---------------------|--------|-------------|
| 2026-05-04 | (initial carry-forward from FUTV1.0) | — | — | Cherry-picked from working v1.0 baseline (commit 7b4e525) | Sean |

---

## How to verify locally

```bash
cd ~/esp/obd/FUTV1.1
firmware/test/verify_frozen.sh
```

Exits 0 if all frozen files match their recorded hashes. Exits non-zero
with a list of mismatches if any have drifted.

The same verification runs as part of `firmware/test/feature_manager/eval.sh`
(and any future feature eval harness) so a Claude Code session that
modifies a frozen file cannot pass its eval.
