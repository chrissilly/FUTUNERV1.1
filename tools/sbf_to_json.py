#!/usr/bin/env python3
"""
sbf_to_json.py — Decode Sean's SCPN-format SBF calibration files into JSON.

Usage:
    python3 sbf_to_json.py <file.sbf>            # → file.sbf.json next to it
    python3 sbf_to_json.py <file.sbf> -o out.json
    python3 sbf_to_json.py *.sbf                 # batch

The SCPN format is reverse-engineered from the v1.0 dongle firmware logs:

    Header: version=5, segments=1, inverse_segments=144, maps=21,
            segment_start=48, inverse_segment_start=58, map_start=10276,
            segment_size=0, map_size=0, ethanol_bit_count=10, ethanol_random=687

The on-disk byte layout doesn't quite match the firmware-log field ordering
(there's a 0-padding word or reserved field between segment_start and
inverse_segment_start in the actual bytes). This decoder records both the
named fields and a `_raw_header` array so nothing is lost.

Internal segment / map data is emitted as base64 + a hex preview so you can
post-process. For full structural decoding you need to cross-reference with
Albin's `bdef_file.c` or capture live UDS traffic showing exact map shapes.
"""
import struct, json, base64, os, sys, argparse

MAGIC = b"SCPN"
HEADER_FIELDS = [
    "version",
    "segments",
    "inverse_segments",
    "maps",
    "segment_start",
    "_reserved_a",            # observed = 0; firmware log calls this "inverse_segment_start"
    "inverse_segment_start",  # observed at byte offset 28
    "map_start",              # observed at byte offset 32
    "_reserved_b",            # observed = 0
    "ethanol_bit_count",
    "ethanol_random",
]
HEADER_SIZE = 4 + 4 * len(HEADER_FIELDS)  # 4-byte magic + 11 u32 = 48 bytes


def hex_preview(b: bytes, n: int = 64) -> str:
    if len(b) <= n:
        return b.hex()
    return b[:n].hex() + f"... (+{len(b)-n} more bytes)"


def decode_sbf(path: str) -> dict:
    data = open(path, "rb").read()
    if len(data) < HEADER_SIZE:
        raise ValueError(f"{path}: too short ({len(data)} bytes < {HEADER_SIZE})")
    if data[:4] != MAGIC:
        raise ValueError(
            f"{path}: bad magic {data[:4]!r}, expected {MAGIC!r}"
        )

    raw_fields = list(struct.unpack(f"<{len(HEADER_FIELDS)}I", data[4:HEADER_SIZE]))
    header = dict(zip(HEADER_FIELDS, raw_fields))

    seg_start  = header["segment_start"]
    iseg_start = header["inverse_segment_start"]
    map_start  = header["map_start"]
    n_seg   = header["segments"]
    n_iseg  = header["inverse_segments"]
    n_maps  = header["maps"]

    # Slice each section. Where boundaries are unknown we use the next-known
    # start; the final section runs to EOF.
    boundaries = sorted({seg_start, iseg_start, map_start, len(data)})
    def section_bytes(start):
        if start <= 0 or start >= len(data):
            return b""
        end = min(b for b in boundaries if b > start)
        return data[start:end]

    segments_blob       = section_bytes(seg_start)
    inverse_seg_blob    = section_bytes(iseg_start)
    maps_blob           = section_bytes(map_start)

    # Try to interpret segments and inverse_segments as fixed-size entries.
    # Heuristic: divide blob length by count and see if it's a round value.
    def split_entries(blob: bytes, count: int):
        if count <= 0 or not blob:
            return None
        if len(blob) % count == 0:
            entry_size = len(blob) // count
            entries = []
            for i in range(count):
                entry = blob[i * entry_size : (i + 1) * entry_size]
                entries.append({
                    "index": i,
                    "size_bytes": entry_size,
                    "hex": entry.hex(),
                    "b64": base64.b64encode(entry).decode("ascii"),
                })
            return {"entry_size_bytes": entry_size, "entries": entries}
        return {"entry_size_bytes": None, "raw_blob_b64": base64.b64encode(blob).decode("ascii")}

    return {
        "_format": "SCPN/SBF v" + str(header["version"]),
        "_source_file": os.path.basename(path),
        "_file_size_bytes": len(data),
        "_header_size_bytes": HEADER_SIZE,
        "_raw_header_u32_le": raw_fields,
        "header": header,
        "ethanol_validation": {
            "bit_count": header["ethanol_bit_count"],
            "expected_value_hex": f"0x{header['ethanol_random']:x}",
            "expected_value_dec": header["ethanol_random"],
            "mask_hex": f"0x{(1 << header['ethanol_bit_count']) - 1:x}",
            "note": (
                "ECU's raw ethanol register is masked with `mask_hex` and "
                "compared to `expected_value_hex`. The cloud server's per-VIN "
                "patcher injects this value during SBF delivery, locking the "
                "tune to one ECU."
            ),
        },
        "sections": {
            "segments": {
                "start_offset": seg_start,
                "count": n_seg,
                "blob_size_bytes": len(segments_blob),
                "hex_preview": hex_preview(segments_blob),
                "decoded": split_entries(segments_blob, n_seg),
            },
            "inverse_segments": {
                "start_offset": iseg_start,
                "count": n_iseg,
                "blob_size_bytes": len(inverse_seg_blob),
                "hex_preview": hex_preview(inverse_seg_blob),
                "decoded": split_entries(inverse_seg_blob, n_iseg),
            },
            "maps": {
                "start_offset": map_start,
                "count": n_maps,
                "blob_size_bytes": len(maps_blob),
                "hex_preview": hex_preview(maps_blob),
                "decoded": split_entries(maps_blob, n_maps),
            },
        },
        "_open_questions": [
            "Exact internal layout of each segment / inverse_segment entry "
            "(target ECU address + length + payload?). Albin's bdef_file.c "
            "uses [address u32, size u32, data_offset u32] per entry; SCPN "
            "may be similar but unverified.",
            "Map entry layout: each map likely has axis sizes, axis arrays, "
            "and a 2-D Z array of float32 values, but the entry-size split "
            "above is heuristic. Cross-reference with TunerPro XDF for the "
            "same boxcode to label each map.",
            "The two `_reserved_*` fields in the header read as 0 in the "
            "live samples; confirm by inspecting more SBFs across versions.",
        ],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--output", help="output path (single-file mode)")
    args = ap.parse_args()

    if args.output and len(args.files) != 1:
        ap.error("-o requires exactly one input file")

    for path in args.files:
        try:
            decoded = decode_sbf(path)
        except Exception as e:
            print(f"[FAIL] {path}: {e}", file=sys.stderr)
            continue

        out_path = args.output or (path + ".json")
        with open(out_path, "w") as f:
            json.dump(decoded, f, indent=2)
        print(f"[OK] {path} -> {out_path}")
        h = decoded["header"]
        print(f"      v{h['version']}  segments={h['segments']}  "
              f"inverse={h['inverse_segments']}  maps={h['maps']}  "
              f"ethanol_random=0x{h['ethanol_random']:x} (bit_count={h['ethanol_bit_count']})")


if __name__ == "__main__":
    main()
