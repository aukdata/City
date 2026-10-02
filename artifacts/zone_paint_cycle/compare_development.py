#!/usr/bin/env python3
"""Compare authoritative DEV1 snapshots without loading or changing the game.

Schema: src/save/DevelopmentSnapshot.hpp/.cpp. Zone omission means Unzoned.
Building fields and complete parcel records are compared as raw bytes, preserving
floating-point bits, IDs, ordering, and polygon coordinates.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def parse_snapshot(path):
    data = path.read_bytes()
    offset = 0

    def take(size):
        nonlocal offset
        if size < 0 or offset + size > len(data):
            raise ValueError(f"Truncated snapshot: {path} at byte {offset}")
        result = data[offset:offset + size]
        offset += size
        return result

    def read(fmt):
        return struct.unpack("<" + fmt, take(struct.calcsize("<" + fmt)))

    header = read("IHHIHHI")
    if header[:6] != (0x31564544, 1, 64, 1024, 64, 64) or header[6] > 4096:
        raise ValueError(f"Unsupported DEV1 header: {header}")
    chunks = {}
    totals = dict(zones=0, buildings=0, patches=0, vertices=0)
    for _ in range(header[6]):
        x, y, urban, zone_count, building_count, patch_count = read("HHBIII")
        if ((x, y) in chunks or x >= 64 or y >= 64 or urban > 1
                or zone_count > 4096 or building_count > 4096 or patch_count > 65536):
            raise ValueError(f"Invalid chunk header at {(x, y)}")
        zones = {}
        for _ in range(zone_count):
            cell, zone = read("HB")
            if cell >= 4096 or cell in zones or zone > 6:
                raise ValueError(f"Invalid zone record at {(x, y, cell)}")
            zones[cell] = zone
        buildings = take(31 * building_count)
        patch_start = offset
        for _ in range(patch_count):
            _, _, _, _, _, vertices = read("iBfIqI")
            if vertices > 4096:
                raise ValueError("Invalid parcel vertex count")
            take(16 * vertices)
            totals["vertices"] += vertices
        patches = data[patch_start:offset]
        chunks[x, y] = (urban, zones, building_count, buildings, patch_count, patches)
        totals["zones"] += zone_count
        totals["buildings"] += building_count
        totals["patches"] += patch_count
    if offset != len(data):
        raise ValueError(f"Trailing bytes: {len(data) - offset}")
    building_hash = hashlib.sha256()
    patch_hash = hashlib.sha256()
    for (x, y), item in sorted(chunks.items()):
        building_hash.update(struct.pack("<HHI", x, y, item[2]) + item[3])
        patch_hash.update(struct.pack("<HHI", x, y, item[4]) + item[5])
    summary = {
        "path": str(path), "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(), "chunks": len(chunks),
        "totals": totals, "buildings_sha256": building_hash.hexdigest(),
        "patches_sha256": patch_hash.hexdigest(),
    }
    return summary, chunks


def compare(original, candidate):
    before, before_chunks = parse_snapshot(original)
    after, after_chunks = parse_snapshot(candidate)
    result = {
        "original": before, "candidate": after,
        "same_chunk_set": before_chunks.keys() == after_chunks.keys(),
        "snapshot_bit_exact": before["sha256"] == after["sha256"],
        "urbanization_changes": [], "zone_changes": [],
        "building_changed_chunks": [], "parcel_changed_chunks": [],
    }
    for coord in sorted(before_chunks.keys() & after_chunks.keys()):
        first, second = before_chunks[coord], after_chunks[coord]
        if first[0] != second[0]:
            result["urbanization_changes"].append(coord)
        for cell in sorted(first[1].keys() | second[1].keys()):
            old, new = first[1].get(cell, 0), second[1].get(cell, 0)
            if old != new:
                gx, gz = coord[0] * 64 + cell % 64, coord[1] * 64 + cell // 64
                result["zone_changes"].append({
                    "chunk": coord, "cell": [cell % 64, cell // 64],
                    "global_cell": [gx, gz], "center": [(gx + .5) * 16, (gz + .5) * 16],
                    "before": old, "after": new,
                })
        if first[2:4] != second[2:4]:
            result["building_changed_chunks"].append(coord)
        if first[4:6] != second[4:6]:
            result["parcel_changed_chunks"].append(coord)
    result["buildings_bit_exact"] = result["same_chunk_set"] and not result["building_changed_chunks"]
    result["parcels_bit_exact"] = result["same_chunk_set"] and not result["parcel_changed_chunks"]
    return result


def expected_zone(value):
    try:
        result = tuple(int(part) for part in value.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("Use GX,GZ,OLD,NEW") from error
    if len(result) != 4:
        raise argparse.ArgumentTypeError("Use GX,GZ,OLD,NEW")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("candidate", type=Path)
    expectation = parser.add_mutually_exclusive_group()
    expectation.add_argument("--expect-zone", type=expected_zone, action="append",
                             metavar="GX,GZ,OLD,NEW", help="Require exactly these zone changes")
    expectation.add_argument("--expect-identical", action="store_true")
    parser.add_argument("--output", type=Path, help="Optional JSON result; input files are never changed")
    args = parser.parse_args()
    result = compare(args.original, args.candidate)
    expected = None
    if args.expect_identical:
        expected = result["snapshot_bit_exact"]
    elif args.expect_zone is not None:
        changes = {(*item["global_cell"], item["before"], item["after"])
                   for item in result["zone_changes"]}
        expected = (changes == set(args.expect_zone) and result["same_chunk_set"]
                    and not result["urbanization_changes"] and result["buildings_bit_exact"]
                    and result["parcels_bit_exact"])
    result["expectation_passed"] = expected
    output = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(output, encoding="utf-8")
    print(output, end="")
    return 1 if expected is False else 0


if __name__ == "__main__":
    raise SystemExit(main())
