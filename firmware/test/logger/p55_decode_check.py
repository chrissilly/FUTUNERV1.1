#!/usr/bin/env python3
"""
P-55 host-side decoder validator.

Re-implements the FIXED firmware decoder in pure Python and applies it
to the two raw response samples captured in koeo_baseline_2026-05-22.json
(commit 21bb993). Acceptance: every required-true variable lands in its
KOEO plausibility window.

Usage:
    python3 p55_decode_check.py
    -> exits 0 on PASS, 1 on FAIL

The firmware fix being validated:
  1. logger_config_parse_poll_response walks variables in the
     emission order recorded by logger_config_build_configuration
     (group-then-config), NOT in compare_variables_for_grouping order.
  2. Unsigned vars decode as unsigned (no int8_t/int16_t sign-extension).
  3. Formula is `phys = raw * scale + offset`, not `(raw + offset) * scale`.
  4. Byte order plumbed from boxcode_config.is_big_endian (LE for 4K0907557G).
"""
from __future__ import annotations
import json
import struct
import sys
from pathlib import Path

# Mirror of VARIABLES_4K0907557G__0003[] from firmware/src/logger/logger_variables.c
# (name, address, size, scale, offset, is_required, is_signed)
VARIABLES = [
    ("nmot_w",                  0x60020618, 2, 0.25,                  0,   True,  False),
    ("InjSys_ratEthPrtnBascFu", 0x6001522A, 2, 0.00152587167162585,   0,   True,  False),
    ("Com_stCrCtlPan",          0x600206F8, 2, 1.0,                   0,   True,  False),
    ("rl_w",                    0x60015660, 2, 0.0234375066758221,    0,   False, False),
    ("tmot",                    0x6001BF38, 1, 0.749803921568627,   -48.0, False, False),
    ("wdkba",                   0x6001B842, 1, 0.392156862745098,     0,   False, False),
]

IS_BIG_ENDIAN = False  # 4K0907557G__0003 is little-endian (Aurix Tricore)


def compare_for_grouping(a, b):
    """Mirror of logger_config.c compare_variables_for_grouping:
       size desc, then upper-16 of address asc, then lower-16 asc."""
    _, addr_a, size_a, *_ = a
    _, addr_b, size_b, *_ = b
    if size_a != size_b:
        return size_b - size_a
    upper_a = (addr_a >> 16) & 0xFFFF
    upper_b = (addr_b >> 16) & 0xFFFF
    if upper_a != upper_b:
        return upper_a - upper_b
    return (addr_a & 0xFFFF) - (addr_b & 0xFFFF)


def build_parse_order(vars_added):
    """Mirror of logger_config_build_configuration's group-then-config walk."""
    import functools
    sorted_vars = sorted(vars_added, key=functools.cmp_to_key(compare_for_grouping))
    groups = {}
    group_order = []
    for v in sorted_vars:
        upper = (v[1] >> 16) & 0xFFFF
        if upper not in groups:
            groups[upper] = []
            group_order.append(upper)
        groups[upper].append(v)
    order = []
    for upper in group_order:
        order.extend(groups[upper])
    return order


def decode(parse_order, response_hex, is_big_endian):
    """Mirror of the FIXED logger_config_parse_poll_response."""
    response = bytes.fromhex(response_hex)
    assert response[0] == 0x7E, f"Expected 0x7E header, got 0x{response[0]:02X}"
    out = {}
    offset = 1
    for (name, _addr, size, scale, var_off, _req, is_signed) in parse_order:
        chunk = response[offset:offset + size]
        if len(chunk) < size:
            raise ValueError(f"Response too short for {name}")
        if size == 1:
            u = chunk[0]
            raw = struct.unpack("b", chunk)[0] if is_signed else u
        elif size == 2:
            fmt = ">" if is_big_endian else "<"
            fmt += "h" if is_signed else "H"
            raw = struct.unpack(fmt, chunk)[0]
        else:
            raise ValueError(f"Unsupported size {size}")
        out[name] = raw * scale + var_off
        offset += size
    return out


PLAUSIBILITY = {
    # name: (lo, hi, comment)
    "nmot_w":                  (-1.0, 5.0,    "KOEO engine speed must be ~0 rpm"),
    "InjSys_ratEthPrtnBascFu": (0.0,  100.0,  "ethanol % is [0..100]"),
    "Com_stCrCtlPan":          (0.0,  65535.0,"cruise state code is any uint16"),
    "rl_w":                    (0.0,  100.0,  "rl_w % at KOEO should be [0..100]"),
    "tmot":                    (-20.0,120.0,  "engine coolant: ambient or warm-recent"),
    "wdkba":                   (0.0,  100.0,  "throttle % at KOEO should be [0..100]"),
}


def main():
    repo_root = Path(__file__).resolve().parents[3]
    baseline_path = repo_root / "firmware/test/logger/koeo_baseline_2026-05-22.json"
    baseline = json.loads(baseline_path.read_text())
    samples = baseline["raw_response_hex_samples"]

    # The firmware adds required vars first, then optional ones (via
    # add_by_name) in declaration order. Mirror that order for the
    # parse_order construction.
    required = [v for v in VARIABLES if v[5]]
    optional = [v for v in VARIABLES if not v[5]]
    added = required + optional
    parse_order = build_parse_order(added)
    print(f"emission order: {[v[0] for v in parse_order]}")
    print()

    fails = 0
    for sample in samples:
        print(f"--- sample @ {sample['captured_at']}: {sample['hex']} ---")
        try:
            decoded = decode(parse_order, sample["hex"], IS_BIG_ENDIAN)
        except Exception as e:
            print(f"  decode error: {e}")
            fails += 1
            continue
        for name, value in decoded.items():
            lo, hi, why = PLAUSIBILITY[name]
            ok = lo <= value <= hi
            mark = "PASS" if ok else "FAIL"
            print(f"  {name:28s} = {value:10.3f}    [{mark}]  {why}")
            if not ok:
                fails += 1
        print()

    if fails:
        print(f"FAIL: {fails} plausibility violations")
        return 1
    print("PASS: all decoded values in KOEO plausibility window")
    return 0


if __name__ == "__main__":
    sys.exit(main())
