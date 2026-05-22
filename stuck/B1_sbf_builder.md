# B1 SBF builder — stuck

## Block (2026-05-22)

The canonical `scorpion-bin-tools` Node.js implementation at
`~/esp/obd/SEFIV1.0/scorpion-bin-tools/` does NOT build against the
canonical config.json at
`~/esp/obd/SEFIV1.0/EXE TOOL/data/4K0907557G__0003/input/config.json`.

Schema mismatch: code reads `target.base_file` + `target.patch_base`;
config supplies `target.input_file` + `target.patch_source`. Trial
build (in a tmpdir copy) fails with:

    ERROR: Build failed: The "path" argument must be of type string. Received undefined
    at FileBuilder.resolveInputFile (src/lib/file-builder.js:433:12)

Cross-checked both config copies (`EXE TOOL/data/.../config.json` and
the duplicate at `scorpion-bin-tools/data/.../config.json`); both use
the newer `input_file` schema. The Node code has not been updated to
match — no git history available locally (no `.git` in scorpion-bin-
tools dir) to see when the rename happened.

## Why I'm not auto-patching

Two paths from here, both need owner sign-off:

1. **Monkey-patch the Node tool in a tools/sbf_builder/ vendored
   copy** to accept `input_file` alongside `base_file`. Risk:
   diverges from Sean's canonical Scorpion reference; needs review.

2. **Native Python port** of the build pipeline. Risk: produces an
   output that's not byte-identical to canonical for non-obvious
   reasons. 3-5 hr effort minimum. Same review need at the end.

Per Sean's directive ("Sean explicitly un-deferred this in the
2026-05-21 chat — materials are ready"), the assumption was the
materials were ready to build immediately. The schema drift is the
block.

## Suggested resolution for morning review

Option (cheapest): patch the Node tool inside the SEFIV1.0 tree to
accept `input_file` (alias of `base_file`) AND verify output is
byte-identical to `3stage1_patched.sbf`. If yes, vendor the patched
tool into `tools/sbf_builder_node/` + wrap with Python.

Option (proper): regenerate `3stage1_patched.sbf` with current
canonical Scorpion build pipeline (if Sean has a newer scorpion-bin-
tools elsewhere), and use that as the byte-identity target. Then
either wrap the new tool or port.

## State at block time

- `~/esp/obd/SEFIV1.0/EXE TOOL/data/4K0907557G__0003/output/3stage1_patched.sbf`
  is the canonical, 35,571 bytes, SCPN format-v4 magic header.
- `~/esp/obd/SEFIV1.0/scorpion-bin-tools/` is present with
  `node_modules` installed; Node tool exits with the schema error
  on every target build invocation.
- No `tools/sbf_builder/` directory yet in FUTV1.1; nothing committed.
