#!/usr/bin/env python3
"""Minimal streaming A2L parser — MEASUREMENT + COMPU_METHOD blocks.

A2L is whitespace-tolerant ASCII (Bosch sometimes emits Latin-1 in
description strings; we read raw bytes + decode lazily). This parser
covers the subset of A2L needed to derive a logger variable catalog:

  - MEASUREMENT: name, datatype, COMPU_METHOD ref, ECU_ADDRESS,
                 BIT_MASK, MATRIX_DIM, optional limits
  - COMPU_METHOD: name, type (RAT_FUNC/LINEAR/IDENTICAL/...), format,
                   unit, COEFFS

Linear / RAT_FUNC resolution: phys = (a*raw² + b*raw + c) / (d*raw² +
e*raw + f). For most logger MEASUREMENTs A2L emits with a=0, d=0, e=0;
scale = b/f, offset = c/f. parse_compu_method() returns the resolved
(scale, offset) when that pattern applies, None otherwise.

Public API:
    parse_a2l(path) -> dict
        { "measurements": {name: meas_dict, ...},
          "compu_methods": {name: cm_dict, ...} }
"""

import re

# A2L is loosely tokenized. We do line-streaming with a small
# state machine: outside any block, /begin <KIND> <NAME> starts a
# block, /end <KIND> ends it. Lines inside are key/value-ish.

_RE_BEGIN     = re.compile(rb'^\s*/begin\s+(\S+)\s+(\S+)')
_RE_END       = re.compile(rb'^\s*/end\s+(\S+)')
_RE_ECU_ADDR  = re.compile(rb'\bECU_ADDRESS\s+0x([0-9A-Fa-f]+)')
_RE_BIT_MASK  = re.compile(rb'\bBIT_MASK\s+0x([0-9A-Fa-f]+)')
_RE_MATRIX    = re.compile(rb'\bMATRIX_DIM\s+(\d+)(?:\s+(\d+))?(?:\s+(\d+))?')
_RE_COEFFS    = re.compile(rb'^\s*COEFFS\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)')
_RE_STR       = re.compile(rb'"([^"]*)"')

A2L_DATATYPES = {
    b"UBYTE":         ("u8",  1, False),
    b"SBYTE":         ("s8",  1, True),
    b"UWORD":         ("u16", 2, False),
    b"SWORD":         ("s16", 2, True),
    b"ULONG":         ("u32", 4, False),
    b"SLONG":         ("s32", 4, True),
    b"FLOAT32_IEEE":  ("f32", 4, True),
}

def _decode(b):
    """Decode bytes from A2L (Bosch sometimes emits Latin-1 in strings)."""
    return b.decode("utf-8", errors="replace") if b is not None else None

def parse_a2l(path):
    measurements = {}
    compu_methods = {}

    # State machine — at most one block open at a time at the depth
    # we care about. (A2L allows nested /begin blocks; we only care
    # about the outermost MEASUREMENT / COMPU_METHOD).
    stack = []  # list of (kind, name, dict)

    with open(path, "rb") as f:
        for line in f:
            m = _RE_BEGIN.search(line)
            if m:
                kind = m.group(1)
                name = _decode(m.group(2))
                if kind == b"MEASUREMENT":
                    stack.append(("MEASUREMENT", name, {
                        "name": name,
                        "datatype_raw": None,
                        "kind": None,           # "u8"|"s16"|... resolved later
                        "bytes": None,
                        "signed": None,
                        "ecu_address": None,
                        "bit_mask": None,
                        "matrix_dim": None,
                        "compu_method": None,
                    }))
                elif kind == b"COMPU_METHOD":
                    stack.append(("COMPU_METHOD", name, {
                        "name": name,
                        "type": None,             # "RAT_FUNC"|"LINEAR"|"IDENTICAL"|...
                        "format": None,           # "%6.3f"
                        "unit": None,             # "rpm"
                        "coeffs": None,           # (a,b,c,d,e,f)
                        "scale": None,            # derived if linear-resolvable
                        "offset": None,
                    }))
                else:
                    # Other block kind — push so /end nests correctly.
                    stack.append((kind, name, None))
                continue

            m = _RE_END.search(line)
            if m and stack:
                top_kind, top_name, top_d = stack.pop()
                if top_kind == "MEASUREMENT" and top_d is not None:
                    measurements[top_name] = top_d
                elif top_kind == "COMPU_METHOD" and top_d is not None:
                    cm = top_d
                    if cm["coeffs"]:
                        a, b, c, d, e, f = cm["coeffs"]
                        if cm["type"] == "RAT_FUNC":
                            # ASAP2 RAT_FUNC encodes phys → raw:
                            #   raw = (a·phys² + b·phys + c) /
                            #         (d·phys² + e·phys + f)
                            # Logger decode wants raw → phys. For the
                            # linear sub-case (a=d=e=0, b≠0):
                            #   phys = (raw·f − c) / b
                            #   scale  = f / b
                            #   offset = −c / b
                            if a == 0 and d == 0 and e == 0 and b != 0:
                                cm["scale"]  = f / b
                                cm["offset"] = -c / b
                        elif cm["type"] == "LINEAR":
                            # LINEAR is raw → phys directly:
                            #   phys = a·raw + b
                            cm["scale"]  = a
                            cm["offset"] = b
                    compu_methods[top_name] = cm
                continue

            if not stack:
                continue
            top_kind, top_name, top_d = stack[-1]
            if top_d is None:
                continue  # inside a block we don't care about

            if top_kind == "MEASUREMENT":
                # A2L MEASUREMENT block grammar (Bosch one-token-per-
                # line dialect):
                #   /begin MEASUREMENT <name>
                #     "<description>"
                #     <DATATYPE>
                #     <COMPU_METHOD>
                #     <resolution> <accuracy> <lower> <upper>
                #     [...optional fields including ECU_ADDRESS...]
                #   /end MEASUREMENT
                # We track sequence position via flags on the dict.
                if top_d["datatype_raw"] is None:
                    for token, (kind_str, nbytes, signed) in A2L_DATATYPES.items():
                        if re.search(rb'\b' + token + rb'\b', line):
                            top_d["datatype_raw"] = token.decode()
                            top_d["kind"]   = kind_str
                            top_d["bytes"]  = nbytes
                            top_d["signed"] = signed
                            break
                elif top_d["compu_method"] is None:
                    # First content line AFTER the datatype line is the
                    # COMPU_METHOD reference. Skip blank lines and lines
                    # that are clearly other fields (anything containing
                    # known sub-keywords). Single-token lines are the
                    # signature.
                    stripped = line.strip()
                    if stripped and not stripped.startswith(b"/"):
                        tokens = stripped.split()
                        first = _decode(tokens[0])
                        # Skip ECU_ADDRESS-line / FORMAT-line / etc.
                        # Conversion ref is always a bare identifier
                        # (or NO_COMPU_METHOD).
                        if first and first.replace("_", "").replace(".", "").isalnum():
                            top_d["compu_method"] = first

                ea = _RE_ECU_ADDR.search(line)
                if ea:
                    top_d["ecu_address"] = int(ea.group(1), 16)
                bm = _RE_BIT_MASK.search(line)
                if bm:
                    top_d["bit_mask"] = int(bm.group(1), 16)
                md = _RE_MATRIX.search(line)
                if md:
                    dims = [int(g) for g in md.groups() if g]
                    top_d["matrix_dim"] = dims

            elif top_kind == "COMPU_METHOD":
                # COMPU_METHOD <name> "<desc>" <TYPE> "<format>" "<unit>"
                # followed by COEFFS etc on subsequent lines.
                if top_d["type"] is None:
                    for kind in (b"RAT_FUNC", b"LINEAR", b"IDENTICAL",
                                 b"TAB_INTP", b"TAB_NOINTP", b"TAB_VERB", b"FORM"):
                        if re.search(rb'\b' + kind + rb'\b', line):
                            top_d["type"] = kind.decode()
                            break
                # A2L Bosch dialect emits one quoted string per line:
                # description, then (after the TYPE keyword) format,
                # then unit. We track which positional string slot
                # the next quoted line fills.
                if top_d["type"]:
                    strs = _RE_STR.findall(line)
                    for s in strs:
                        sd = _decode(s)
                        if top_d["format"] is None:
                            top_d["format"] = sd
                        elif top_d["unit"] is None:
                            top_d["unit"] = sd
                cf = _RE_COEFFS.match(line)
                if cf:
                    try:
                        coeffs = tuple(float(g.decode()) for g in cf.groups())
                        top_d["coeffs"] = coeffs
                    except ValueError:
                        pass

    return {
        "measurements": measurements,
        "compu_methods": compu_methods,
    }


def resolve_scale(meas, cm_index):
    """Given a MEASUREMENT dict + COMPU_METHOD index, return
       (scale, offset, unit) or (None, None, None) if unresolvable."""
    cm_name = meas.get("compu_method")
    if not cm_name or cm_name == "NO_COMPU_METHOD":
        return (1.0, 0.0, None)
    cm = cm_index.get(cm_name)
    if not cm:
        return (None, None, None)
    if cm["type"] == "IDENTICAL":
        return (1.0, 0.0, cm.get("unit"))
    if cm["scale"] is not None:
        return (cm["scale"], cm["offset"], cm.get("unit"))
    return (None, None, cm.get("unit"))


if __name__ == "__main__":
    import sys, json
    out = parse_a2l(sys.argv[1])
    # Demo dump — just the names we care about.
    names = sys.argv[2:] if len(sys.argv) > 2 else ["nmot_w", "rl_w", "tmot", "wdkba"]
    for n in names:
        m = out["measurements"].get(n)
        if not m:
            print(f"{n}: NOT FOUND")
            continue
        scale, offset, unit = resolve_scale(m, out["compu_methods"])
        print(f"{n}:")
        print(f"  datatype={m['datatype_raw']} bytes={m['bytes']} signed={m['signed']}")
        print(f"  address=0x{m['ecu_address']:08X}" if m['ecu_address'] is not None else "  address=NONE")
        print(f"  compu_method={m['compu_method']} -> scale={scale} offset={offset} unit={unit}")
