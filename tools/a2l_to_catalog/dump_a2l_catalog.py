#!/usr/bin/env python3
"""Dump every loggable MEASUREMENT from an A2L into JSON.

Filters:
  - keep UBYTE / SBYTE / UWORD / SWORD / ULONG / SLONG / FLOAT32_IEEE
  - require ECU_ADDRESS present
  - skip MATRIX_DIM > 1 entries (arrays — logger doesn't handle yet)

Output schema (per entry):
  {
    "name":       str,
    "address":    int  (decimal — JSON-safe; emit "0x..." in firmware via codegen),
    "size":       int  (1 / 2 / 4),
    "signed":     bool,
    "datatype":   "UBYTE" | "UWORD" | ... ,
    "scale":      float | null,   # null if compu_method couldn't resolve to linear
    "offset":     float | null,
    "unit":       str | null,
    "compu_method": str | null,
  }

Usage:
    dump_a2l_catalog.py <a2l-path> <out.json>
"""
import argparse
import json
import sys
from a2l_parser import parse_a2l, resolve_scale

LOGGABLE_DATATYPES = {"UBYTE", "SBYTE", "UWORD", "SWORD",
                      "ULONG", "SLONG", "FLOAT32_IEEE"}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a2l")
    ap.add_argument("out_json")
    ap.add_argument("--require-address", action="store_true", default=True,
                    help="(default) skip MEASUREMENTs with no ECU_ADDRESS")
    args = ap.parse_args()

    print(f"parsing {args.a2l} ...", file=sys.stderr)
    data = parse_a2l(args.a2l)
    measurements = data["measurements"]
    compu_methods = data["compu_methods"]
    print(f"  {len(measurements):,} MEASUREMENTs, {len(compu_methods):,} COMPU_METHODs",
          file=sys.stderr)

    out = []
    skipped = {"no_addr": 0, "bad_type": 0, "array": 0}
    for name, m in measurements.items():
        if args.require_address and m["ecu_address"] is None:
            skipped["no_addr"] += 1
            continue
        if m["datatype_raw"] not in LOGGABLE_DATATYPES:
            skipped["bad_type"] += 1
            continue
        if m["matrix_dim"] and any(d > 1 for d in m["matrix_dim"]):
            skipped["array"] += 1
            continue
        scale, offset, unit = resolve_scale(m, compu_methods)
        out.append({
            "name": name,
            "address": m["ecu_address"],
            "size": m["bytes"],
            "signed": m["signed"],
            "datatype": m["datatype_raw"],
            "scale": scale,
            "offset": offset,
            "unit": unit,
            "compu_method": m["compu_method"],
        })

    out.sort(key=lambda r: r["address"])
    with open(args.out_json, "w") as f:
        json.dump(out, f, indent=2)
    print(f"wrote {len(out):,} loggable MEASUREMENTs -> {args.out_json}",
          file=sys.stderr)
    print(f"  skipped: no_addr={skipped['no_addr']}  "
          f"bad_type={skipped['bad_type']}  array={skipped['array']}",
          file=sys.stderr)

if __name__ == "__main__":
    main()
