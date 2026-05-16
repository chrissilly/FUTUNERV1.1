#!/usr/bin/env python3
"""
Extract per-variant SA2 bytecode, ALFID, per-block CRCs, and flash block
addresses from the Bosch MDG1 ODX files (FLASH-DATA + SECURITY) and the
matching exploitsettings.config files. Produce a single JSON manifest that
the firmware's flash module can consume to drive 0x27 SecurityAccess and
0x34/0x36/0x37/0x31 RoutineControl per-variant.

Inputs:
  - hw_reference/vag_mdg1_drive_pull/mg1_full_tree/MG1/**/*.odx
  - hw_reference/vag_mdg1_drive_pull/test_odx/*.odx
  - hw_reference/vag_mdg1_drive_pull/exploitsettings/*.config

Output:
  - secrets/mdg1_variant_manifest.json   (proprietary IP — gitignored)

The output lives under secrets/ because per-variant flash block addresses +
the SA2 bytecode are part of the proprietary flash chain (Hard Rule 5).
"""

from __future__ import annotations

import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from datetime import date
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DRIVE_PULL = REPO / "hw_reference" / "vag_mdg1_drive_pull"
# test_odx/ duplicates the contents of mg1_full_tree/MG1/Tests/. Walking only
# mg1_full_tree avoids double-counting and gives us the proper variant-folder
# context for production ODX files.
ODX_DIRS = [DRIVE_PULL / "mg1_full_tree" / "MG1"]
EXPLOIT_DIR = DRIVE_PULL / "exploitsettings"
OUT = REPO / "secrets" / "mdg1_variant_manifest.json"

# A boxcode looks like  <up-to-11-alnum> <space> <4-digit-version>
BOXCODE_RE = re.compile(r"^[0-9A-Z][0-9A-Z]{4,12}\s+\d{4}$")

# Map filename stems to the human-friendly variant name. The conversion
# back from underscores in stems is ambiguous (parens, double spaces),
# so we have a small explicit table for the cases that come up.
VARIANT_NAME_FIXUPS = {
    "MG1 CS002  Autotuner ": "MG1 CS002 (Autotuner)",
    "MG1 CS002  Autotuner":  "MG1 CS002 (Autotuner)",
}


def normalize_variant(name: str) -> str:
    return VARIANT_NAME_FIXUPS.get(name, name)


def strip_ns(tag: str) -> str:
    return tag.rsplit("}", 1)[-1] if "}" in tag else tag


def find_all(root, name):
    return [e for e in root.iter() if strip_ns(e.tag) == name]


def find_first(root, name):
    for e in root.iter():
        if strip_ns(e.tag) == name:
            return e
    return None


def text(e):
    return (e.text or "").strip() if e is not None else ""


def parse_odx(path: Path) -> dict | None:
    """Pull SA2, ALFID, per-block CRCs, and DATABLOCK metadata from one ODX."""
    try:
        tree = ET.parse(path)
    except ET.ParseError as e:
        print(f"  WARN  parse error in {path.name}: {e}", file=sys.stderr)
        return None
    root = tree.getroot()

    flash = find_first(root, "FLASH")
    flash_id = flash.attrib.get("ID", "") if flash is not None else ""

    sa2_hex = None
    alfid_hex = None
    block_crcs: dict[str, str] = {}
    seen_sa2_session: set[str] = set()

    for sec in find_all(root, "SECURITY"):
        method = find_first(sec, "SECURITY-METHOD")
        sig = find_first(sec, "FW-SIGNATURE")
        validity = find_first(sec, "VALIDITY-FOR")
        if method is None or sig is None:
            continue
        m = text(method).upper()
        s = text(sig).upper()
        if m == "SA2":
            if sa2_hex is None:
                sa2_hex = s
            elif s != sa2_hex:
                # Different SA2 across sessions in the same ODX would be unusual.
                seen_sa2_session.add(s)
        elif m == "ALFID":
            if alfid_hex is None:
                alfid_hex = s
        elif m == "CRC":
            v = text(validity)
            if v:
                checksum_e = find_first(sec, "FW-CHECKSUM")
                if checksum_e is not None:
                    block_crcs[v] = text(checksum_e).upper()

    datablocks = []
    for db in find_all(root, "DATABLOCK"):
        block_id = db.attrib.get("ID", "")
        # DB ID is shaped EMEM_<boxcode>.DB_<FD_NN>... — pull the FD_NN tag.
        m = re.search(r"\.DB_(FD_\d+[A-Za-z_]*)(?:FLASH_DATA)?$", block_id)
        fd_tag = m.group(1) if m else None
        seg = find_first(db, "SEGMENT")
        compressed = find_first(seg, "COMPRESSED-SIZE") if seg is not None else None
        uncompressed = find_first(seg, "UNCOMPRESSED-SIZE") if seg is not None else None
        source_addr = find_first(seg, "SOURCE-START-ADDRESS") if seg is not None else None
        datablocks.append({
            "fd_tag": fd_tag,
            "id": block_id,
            "type": db.attrib.get("TYPE", ""),
            "source_start_address": text(source_addr),
            "compressed_size": int(text(compressed)) if compressed is not None and text(compressed).isdigit() else None,
            "uncompressed_size": int(text(uncompressed)) if uncompressed is not None and text(uncompressed).isdigit() else None,
        })

    return {
        "path": str(path.relative_to(REPO)),
        "flash_id": flash_id,
        "sa2_hex": sa2_hex,
        "alfid_hex": alfid_hex,
        "block_crcs": block_crcs,
        "datablocks": datablocks,
        "sa2_inconsistent_sessions": sorted(seen_sa2_session),
    }


# exploitsettings.config — first stanza is whitespace-separated boxcodes; then
# a "#FlashTags" header, then comma-separated rows:
#   <ver>,<fd_tag>,<address_hex>,<length_hex>,<name>
EXPLOIT_HEADER = re.compile(r"^\s*#\s*FlashTags", re.IGNORECASE)


def parse_exploit(path: Path) -> dict:
    boxcodes: list[str] = []
    flash_tags: list[dict] = []
    other_lines: list[str] = []
    in_tags = False
    for raw in path.read_text(errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue
        if EXPLOIT_HEADER.match(line):
            in_tags = True
            continue
        if not in_tags:
            if BOXCODE_RE.match(line):
                boxcodes.append(line)
            else:
                other_lines.append(line)
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) >= 5 and parts[1].startswith("FD_"):
            flash_tags.append({
                "version": parts[0],
                "fd_tag": parts[1],
                "address": parts[2],
                "length": parts[3],
                "name": parts[4],
            })
    return {
        "variant": normalize_variant(path.stem.replace("_", " ")),
        "boxcodes": boxcodes,
        "flash_tags": flash_tags,
        "other_lines": other_lines,
    }


def variant_name_from_odx_path(path: Path) -> str:
    """mg1_full_tree / MG1 / <variant> / ... is the canonical layout. The
    Tests/ subfolder under MG1/ is collected separately under one bucket."""
    parts = path.parts
    if "mg1_full_tree" in parts:
        i = parts.index("mg1_full_tree")
        if i + 2 < len(parts):
            v = parts[i + 2]
            if v == "Tests":
                return "MG1 Tests (assorted)"
            return normalize_variant(v)
    return "Unknown"


# Tiny SA2 VM reimplementation in Python — used to sanity-check that the
# bytecode lifted out of the ODX decodes as a valid SA2 script. This is the
# same opcode/encoding as firmware/src/flash/sa2_vm.c.
SA2_OPCODES_NO_OPERAND = {0x81, 0x82, 0x49, 0x4C}
SA2_OPCODES_4B_OPERAND = {0x93, 0x84, 0x87}
SA2_OPCODES_1B_OPERAND = {0x68, 0x4A, 0x6B}


def sa2_static_check(hex_str: str) -> tuple[bool, str]:
    try:
        b = bytes.fromhex(hex_str)
    except ValueError as e:
        return False, f"hex decode: {e}"
    pc = 0
    while pc < len(b):
        op = b[pc]; pc += 1
        if op in SA2_OPCODES_NO_OPERAND:
            if op == 0x4C:
                return True, "ok (FINISH reached)"
        elif op in SA2_OPCODES_4B_OPERAND:
            if pc + 4 > len(b):
                return False, f"truncated 4B operand at pc={pc}"
            pc += 4
        elif op in SA2_OPCODES_1B_OPERAND:
            if pc >= len(b):
                return False, f"truncated 1B operand at pc={pc}"
            pc += 1
        else:
            return False, f"invalid opcode 0x{op:02X} at pc={pc-1}"
    return False, "no FINISH reached"


def main() -> int:
    odx_paths: list[Path] = []
    for d in ODX_DIRS:
        if d.exists():
            odx_paths.extend(sorted(d.rglob("*.odx")))

    odx_records: list[dict] = []
    for p in odx_paths:
        rec = parse_odx(p)
        if rec is not None:
            rec["variant"] = variant_name_from_odx_path(p)
            odx_records.append(rec)

    exploit_records: list[dict] = []
    for p in sorted(EXPLOIT_DIR.glob("*.config")):
        exploit_records.append(parse_exploit(p))

    # Group ODX records by (variant family, SA2 script). A variant family with
    # >1 SA2 script means the manifest will list each as a sub-entry.
    by_variant_sa2: dict[tuple[str, str | None], list[dict]] = defaultdict(list)
    for r in odx_records:
        by_variant_sa2[(r["variant"], r["sa2_hex"])].append(r)

    # Build the manifest.
    sa2_scripts: dict[str, dict] = {}
    for (_, sa2_hex), recs in by_variant_sa2.items():
        if not sa2_hex:
            continue
        if sa2_hex not in sa2_scripts:
            sa2_scripts[sa2_hex] = {"hex": sa2_hex, "byte_length": len(sa2_hex) // 2, "used_by_variants": [], "alfids": set()}
        for r in recs:
            v = r["variant"]
            if v not in sa2_scripts[sa2_hex]["used_by_variants"]:
                sa2_scripts[sa2_hex]["used_by_variants"].append(v)
            if r["alfid_hex"]:
                sa2_scripts[sa2_hex]["alfids"].add(r["alfid_hex"])

    sa2_list = []
    for s in sa2_scripts.values():
        ok, detail = sa2_static_check(s["hex"])
        sa2_list.append({
            "hex": s["hex"],
            "byte_length": s["byte_length"],
            "used_by_variants": sorted(s["used_by_variants"]),
            "alfids": sorted(s["alfids"]),
            "static_check_ok": ok,
            "static_check_detail": detail,
        })

    # Per-variant block map (from exploitsettings).
    variants: dict[str, dict] = {}
    for er in exploit_records:
        variants[er["variant"]] = {
            "covered_boxcodes": er["boxcodes"],
            "block_map": er["flash_tags"],
            "sa2_scripts_hex": [],
            "alfids": [],
            "odx_files": [],
        }

    # Attach SA2 + ALFID + ODX provenance to whatever variant string we observed.
    for r in odx_records:
        v = r["variant"]
        if v not in variants:
            variants[v] = {"covered_boxcodes": [], "block_map": [], "sa2_scripts_hex": [], "alfids": [], "odx_files": []}
        if r["sa2_hex"] and r["sa2_hex"] not in variants[v]["sa2_scripts_hex"]:
            variants[v]["sa2_scripts_hex"].append(r["sa2_hex"])
        if r["alfid_hex"] and r["alfid_hex"] not in variants[v]["alfids"]:
            variants[v]["alfids"].append(r["alfid_hex"])
        if r["path"] not in variants[v]["odx_files"]:
            variants[v]["odx_files"].append(r["path"])

    # Boxcode index: map each boxcode string to its variant family.
    boxcode_index: dict[str, str] = {}
    for vname, vbody in variants.items():
        for bc in vbody.get("covered_boxcodes", []):
            boxcode_index[bc] = vname

    manifest = {
        "schema_version": "1.0",
        "generated_at": str(date.today()),
        "source": "FUTV1.1/hw_reference/vag_mdg1_drive_pull (ODX + exploitsettings)",
        "summary": {
            "odx_files_parsed": len(odx_records),
            "exploitsettings_parsed": len(exploit_records),
            "unique_sa2_scripts": len(sa2_list),
            "variants_known": len(variants),
            "boxcodes_indexed": len(boxcode_index),
        },
        "sa2_scripts": sa2_list,
        "variants": variants,
        "boxcode_index": boxcode_index,
        "notes": [
            "RSA public keys per variant were not found in any ODX or "
            "exploitsettings.config in this drive pull. The MEx17/EDC17 "
            "RSAPublicKeys JSONs in vag_mdg1_drive_pull/rsa_pubkeys/ are "
            "platform-adjacent (TP2 / older UDS) and not MG1-applicable.",
            "Block addresses come from exploitsettings.config #FlashTags, "
            "not the ODX (ODX SOURCE-START-ADDRESS is just a small block index 1..6).",
            "ALFID 0131 is the access-level / family identifier sent before "
            "or with the seed request; consistent across every ODX seen.",
        ],
    }

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {OUT}")
    print(f"  ODX files parsed:        {len(odx_records)}")
    print(f"  exploitsettings parsed:  {len(exploit_records)}")
    print(f"  unique SA2 scripts:      {len(sa2_list)}")
    print(f"  variants known:          {len(variants)}")
    print(f"  boxcodes indexed:        {len(boxcode_index)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
