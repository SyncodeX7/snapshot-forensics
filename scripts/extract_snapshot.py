#!/usr/bin/env python3
"""Extract a physical-memory snapshot from a QEMU RAM dump.

The snapshot buffer lives inside guest RAM, so the QEMU dump contains both the
ring buffer and the captured page data.  This tool:

  1. locates SNAPSHOT_HEADER in the dump,
  2. picks a snapshot (latest complete one by default),
  3. rebuilds a physical-memory image by overlaying the captured pages,
  4. verifies every page against the FNV-1a hash stored in the page map,
  5. writes <out>/ram-<seq>.raw and <out>/snapshot-<seq>.json.

For modes B and C the full base slot is applied first, then the delta slots in
sequence order.

Examples:
    python3 extract_snapshot.py dump.raw -o out/
    python3 extract_snapshot.py dump.raw -o out/ --list
    python3 extract_snapshot.py dump.raw -o out/ --slot 3
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snapshot_format as fmt  # noqa: E402


def slot_header(ram, buffer_base, entry):
    off = buffer_base + entry.offset
    if off + fmt.SLOT_HDR.size > len(ram):
        raise SystemExit("slot %d header outside dump" % entry.sequence)
    sh = fmt.SlotHeader(ram, off)
    if sh.magic != fmt.SLOT_MAGIC:
        raise SystemExit("slot at 0x%x has bad magic 0x%x" % (off, sh.magic))
    return sh


def iter_pages(ram, buffer_base, entry):
    sh = slot_header(ram, buffer_base, entry)
    map_off = buffer_base + entry.offset + sh.page_map_offset
    data_off = buffer_base + entry.offset + sh.data_offset
    for i in range(sh.page_count):
        pa, phash, flags = fmt.PAGE_ENTRY.unpack_from(ram, map_off + i * fmt.PAGE_ENTRY.size)
        page = ram[data_off + i * fmt.PAGE_SIZE: data_off + (i + 1) * fmt.PAGE_SIZE]
        if len(page) != fmt.PAGE_SIZE:
            raise SystemExit("page %d of slot seq %d is truncated" % (i, entry.sequence))
        yield pa, phash, flags, page


def apply_entry(ram, buffer_base, entry, image_path, stats):
    sh = slot_header(ram, buffer_base, entry)
    fd = os.open(image_path, os.O_WRONLY)
    try:
        for pa, phash, flags, page in iter_pages(ram, buffer_base, entry):
            if pa + fmt.PAGE_SIZE > stats["image_size"]:
                stats["pages_out_of_range"] += 1
                continue
            actual = fmt.fnv1a32(page)
            if actual != phash:
                stats["hash_mismatches"] += 1
            os.pwrite(fd, page, pa)
            stats["pages_written"] += 1
            stats["bytes_written"] += fmt.PAGE_SIZE
            stats["unique_pages"].add(pa)
            if flags & fmt.PAGE_DELTA:
                stats["delta_pages"] += 1
            if flags & fmt.PAGE_DIRTY:
                stats["dirty_pages"] += 1
    finally:
        os.close(fd)
    return sh


def pick_slots(header, slot_index):
    if slot_index is not None:
        if slot_index < 0 or slot_index >= fmt.MAX_DIR_ENTRIES:
            raise SystemExit("slot index out of range")
        entry = header.slots[slot_index]
        if entry.magic != fmt.SLOT_MAGIC:
            raise SystemExit("directory entry %d is empty" % slot_index)
        return [entry]

    if header.mode in (fmt.MODE_A, fmt.MODE_D):
        entry = header.slots[fmt.FULL_DIR_INDEX]
        if entry.magic != fmt.SLOT_MAGIC or not entry.complete:
            raise SystemExit("no complete full snapshot yet (state=%s)" % fmt.STATE_NAMES.get(header.state))
        return [entry]

    base = header.slots[fmt.FULL_DIR_INDEX]
    if base.magic != fmt.SLOT_MAGIC or not base.complete:
        raise SystemExit("no complete base snapshot yet (state=%s)" % fmt.STATE_NAMES.get(header.state))

    deltas = []
    for entry in header.slots[1:]:
        if entry.magic != fmt.SLOT_MAGIC or not entry.complete:
            continue
        if entry.sequence <= base.sequence:
            continue
        deltas.append(entry)
    deltas.sort(key=lambda e: e.sequence)
    return [base] + deltas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ram")
    ap.add_argument("-o", "--out", default="out")
    ap.add_argument("--offset", type=lambda x: int(x, 0), help="snapshot header offset")
    ap.add_argument("--slot", type=int, help="directory index to extract")
    ap.add_argument("--list", action="store_true", help="list slots and exit")
    ap.add_argument("--from-dump", action="store_true",
                    help="start from a copy of the RAM dump instead of zeroes")
    ap.add_argument("--tag", default="", help="suffix for output file names")
    args = ap.parse_args()

    ram = open(args.ram, "rb").read()
    offset = args.offset
    if offset is None:
        offset = fmt.find_header(ram)
        if offset is None:
            raise SystemExit("no snapshot header found (is the SMM driver configured?)")

    header = fmt.read_header(ram, offset)
    if args.list:
        info = header.to_dict()
        info["header_offset"] = offset
        json.dump(info, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return

    slots = pick_slots(header, args.slot)
    seq = slots[-1].sequence
    os.makedirs(args.out, exist_ok=True)
    suffix = ("-" + args.tag) if args.tag else ""
    image_path = os.path.join(args.out, "ram-%d%s.raw" % (seq, suffix))
    meta_path = os.path.join(args.out, "snapshot-%d%s.json" % (seq, suffix))

    image_size = header.guest_ram_top
    if image_size <= 0:
        raise SystemExit("header has no guest RAM size")
    if image_size > len(ram):
        image_size = len(ram)

    # Create a sparse image of the full guest RAM size.
    with open(image_path, "wb") as f:
        f.truncate(image_size)

    if args.from_dump:
        with open(args.ram, "rb") as src, open(image_path, "r+b") as dst:
            remaining = min(image_size, os.path.getsize(args.ram))
            while remaining:
                chunk = src.read(min(remaining, 1 << 20))
                if not chunk:
                    break
                dst.write(chunk)
                remaining -= len(chunk)

    stats = {
        "image_size": image_size,
        "pages_written": 0,
        "bytes_written": 0,
        "unique_pages": set(),
        "hash_mismatches": 0,
        "pages_out_of_range": 0,
        "delta_pages": 0,
        "dirty_pages": 0,
    }

    slot_infos = []
    for entry in slots:
        sh = apply_entry(ram, offset, entry, image_path, stats)
        slot_infos.append({"dir": entry.to_dict(), "header": sh.to_dict()})

    meta = {
        "source_dump": os.path.abspath(args.ram),
        "header_offset": offset,
        "header": header.to_dict(),
        "ranges": fmt.read_ranges(ram, header),
        "slots_applied": slot_infos,
        "verification": {
            "pages_written": stats["pages_written"],
            "bytes_written": stats["bytes_written"],
            "unique_pages": len(stats["unique_pages"]),
            "unique_bytes": len(stats["unique_pages"]) * fmt.PAGE_SIZE,
            "hash_mismatches": stats["hash_mismatches"],
            "pages_out_of_range": stats["pages_out_of_range"],
            "delta_pages": stats["delta_pages"],
            "dirty_pages": stats["dirty_pages"],
            "coverage_of_ranges": (
                len(stats["unique_pages"]) / float(header.pages_total)
                if header.pages_total else 0.0
            ),
        },
        "image": os.path.abspath(image_path),
    }
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)

    v = meta["verification"]
    print("snapshot seq=%d mode=%s state=%s" % (
        seq, fmt.MODE_NAMES.get(header.mode, "?"), fmt.STATE_NAMES.get(header.state, "?")))
    print("  slots applied : %d" % len(slots))
    print("  pages         : %d (%d unique, %.1f MiB)" % (
        v["pages_written"], v["unique_pages"], v["unique_bytes"] / 1048576.0))
    print("  coverage      : %.1f%% of %d pages" % (
        v["coverage_of_ranges"] * 100.0, header.pages_total))
    print("  hash mismatch : %d" % v["hash_mismatches"])
    print("  image         : %s" % image_path)
    print("  metadata      : %s" % meta_path)
    if v["hash_mismatches"]:
        sys.exit(2)


if __name__ == "__main__":
    main()
