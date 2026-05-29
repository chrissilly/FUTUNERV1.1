#!/usr/bin/env python3
"""For each UI catalog name, find best A2L MEASUREMENT match.

Match strategy (per name):
  1. exact match in A2L
  2. exact match with "_msg" suffix appended (Bosch CAN-published convention)
  3. case-insensitive exact
  4. heuristic: name appears as a substring of an A2L entry

Output: TSV table with one row per UI catalog entry showing the
matched A2L name + address + datatype + scale + a confidence tag.
"""
import json
import re
import sys
from pathlib import Path

def extract_ui_catalog(js_path):
    """Pull the ECU_VAR_DB literal out of control_panel.js as a JSON
    list. Catalog uses JS object literal syntax — convert keys to
    proper JSON strings then parse."""
    src = Path(js_path).read_text()
    # Find: const ECU_VAR_DB = [ ... ];
    m = re.search(r'const\s+ECU_VAR_DB\s*=\s*(\[.*?\]\s*;)', src, re.S)
    if not m:
        raise RuntimeError("ECU_VAR_DB literal not found")
    raw = m.group(1).rstrip(";").rstrip()
    # Convert JS object literal -> JSON. Specifically: unquoted keys.
    # Quote bare-word keys before `:` (one of: name, display, unit, etc.)
    raw = re.sub(r'(\b[A-Za-z_][A-Za-z0-9_]*\b)\s*:', r'"\1":', raw)
    # Strip trailing commas in objects/arrays.
    raw = re.sub(r',(\s*[\]}])', r'\1', raw)
    # JS single quotes -> double quotes (be careful: not inside strings).
    # Use a simple heuristic: replace single quotes that surround
    # identifier-shaped strings.
    raw = re.sub(r"'([^'\\]*)'", r'"\1"', raw)
    return json.loads(raw)

def main():
    js_path  = Path(sys.argv[1])
    a2l_json = Path(sys.argv[2])
    out_tsv  = Path(sys.argv[3])

    ui   = extract_ui_catalog(js_path)
    a2l  = json.load(open(a2l_json))
    by_name = {e["name"]: e for e in a2l}
    by_name_lc = {e["name"].lower(): e for e in a2l}

    rows = []
    counts = {"exact": 0, "msg_suffix": 0, "case_only": 0, "substring": 0, "missing": 0}

    for u in ui:
        n = u.get("name", "")
        if not n:
            continue
        hit, conf, why = None, "missing", ""
        if n in by_name:
            hit, conf = by_name[n], "exact"
        elif (n + "_msg") in by_name:
            hit, conf = by_name[n + "_msg"], "msg_suffix"
        elif n.lower() in by_name_lc:
            hit, conf = by_name_lc[n.lower()], "case_only"
            why = f"matched as {hit['name']}"
        else:
            # substring fallback — pick the shortest A2L name that contains UI name
            candidates = [e for e in a2l if n in e["name"] and len(e["name"]) <= len(n) + 8]
            if candidates:
                candidates.sort(key=lambda e: len(e["name"]))
                hit, conf = candidates[0], "substring"
                why = f"closest substring: {hit['name']}"
        counts[conf] += 1
        if hit:
            rows.append({
                "ui_name":  n,
                "ui_display": u.get("display", ""),
                "a2l_name": hit["name"],
                "address":  f"0x{hit['address']:08X}",
                "datatype": hit["datatype"],
                "scale":    hit.get("scale"),
                "offset":   hit.get("offset"),
                "unit":     hit.get("unit"),
                "confidence": conf,
                "note": why,
            })
        else:
            rows.append({
                "ui_name":  n,
                "ui_display": u.get("display", ""),
                "a2l_name": "",
                "address":  "",
                "datatype": "",
                "scale": None,
                "offset": None,
                "unit": None,
                "confidence": "missing",
                "note": "",
            })

    with open(out_tsv, "w") as f:
        cols = ["ui_name","ui_display","a2l_name","address","datatype",
                "scale","offset","unit","confidence","note"]
        f.write("\t".join(cols) + "\n")
        for r in rows:
            f.write("\t".join(str(r[c]) if r[c] is not None else "" for c in cols) + "\n")

    print(f"UI catalog size: {len(ui)}")
    print(f"Match confidence breakdown:")
    for k, v in counts.items():
        print(f"  {k:12s} {v}")
    print(f"wrote {out_tsv}")

if __name__ == "__main__":
    main()
