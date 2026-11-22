#!/usr/bin/env python3
"""On-disk format of the SnapshotPkg SMM ring buffer.

Keep in sync with SnapshotPkg/Include/Snapshot.h.  All structures are packed
little-endian.

Usage as a tool:
    python3 snapshot_format.py info RAMDUMP [--offset N]
"""

import argparse
import json
import struct
import sys

HEADER_MAGIC = 0x31534D4D50414E53  # "SNAPMMS1"
SLOT_MAGIC = 0x544F4C5350414E53    # "SNAPSLOT"
CONFIG_MAGIC = 0x3147464350414E53  # "SNAPFCG1"
VERSION = 1

HEADER_SIZE = 0x1000
PAGE_SIZE = 0x1000
MAX_RANGES = 64
MAX_DIR_ENTRIES = 32
FULL_DIR_INDEX = 0

MODE_A, MODE_B, MODE_C, MODE_D = 0, 1, 2, 3
MODE_NAMES = {MODE_A: "A-full", MODE_B: "B-hash", MODE_C: "C-dirty", MODE_D: "D-sliced"}

STATE_NAMES = {0: "empty", 1: "ready", 2: "capturing", 3: "error"}

SLOT_COMPLETE = 0x00000001
SLOT_FULL = 0x00000002
SLOT_TRUNCATED = 0x00000004
SLOT_CAPTURING = 0x00000008
SLOT_PARTIAL = 0x00000010

PAGE_FULL = 0x00000001
PAGE_DELTA = 0x00000002
PAGE_DIRTY = 0x00000004
PAGE_SLICED = 0x00000008

_HDR_BASE_FMT = (
    "<QIIII"
    "QQQQQQQQ"
    "II"
    "QQQQ"
    "QQQQQQQQ"
    "II"
    "Q"
    "II"
    "6Q"
)
HDR_BASE = struct.Struct(_HDR_BASE_FMT)
assert HDR_BASE.size == 264, HDR_BASE.size

_HDR_FMT = _HDR_BASE_FMT + "QQQQQQIIIIQ" * MAX_DIR_ENTRIES
HDR = struct.Struct(_HDR_FMT)
assert HDR.size == 2568, HDR.size

SLOT_HDR = struct.Struct("<QIIQQQQIIIIQQQ4Q")
assert SLOT_HDR.size == 120, SLOT_HDR.size

PAGE_ENTRY = struct.Struct("<QII")
assert PAGE_ENTRY.size == 16, PAGE_ENTRY.size

MEM_RANGE = struct.Struct("<QQII")
assert MEM_RANGE.size == 24, MEM_RANGE.size

CONFIG = struct.Struct("<QIIQQIIQQIIQ" + "QQII" * MAX_RANGES)
assert CONFIG.size == 1608, CONFIG.size

_HDR_FIELDS = [
    "magic", "version", "header_size", "mode", "state",
    "buffer_base", "buffer_size", "data_start", "data_end",
    "full_slot_offset", "full_slot_capacity", "delta_start",
    "delta_slot_size", "delta_slot_count", "delta_write_index",
    "sequence", "full_sequence", "tick", "tsc_hz",
    "smi_count", "smi_tsc_total", "smi_tsc_max", "smi_tsc_last",
    "pages_total", "pages_last", "bytes_copied", "guest_ram_top",
    "range_count", "range_entry_size", "ranges_offset",
    "flags", "reserved0",
] + ["reserved%d" % i for i in range(6)]

_SLOT_FIELDS = [
    "magic", "offset", "length", "sequence", "tick", "tsc",
    "mode", "flags", "page_count", "reserved0", "data_bytes",
]

_SLOT_HDR_FIELDS = [
    "magic", "version", "mode", "sequence", "tick", "tsc", "smi_tsc",
    "page_count", "capacity_pages", "flags", "reserved0",
    "data_offset", "page_map_offset", "data_bytes",
] + ["reserved%d" % i for i in range(4)]


def fnv1a32(data):
    """Same 32-bit fold used by HashPage() in SmmSnapshotDriver.c."""
    h = 0xCBF29CE484222325
    for i in range(0, len(data), 8):
        h ^= struct.unpack_from("<Q", data, i)[0]
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return (h ^ (h >> 32)) & 0xFFFFFFFF


class SlotEntry:
    def __init__(self, values):
        for name, value in zip(_SLOT_FIELDS, values):
            setattr(self, name, value)

    @property
    def complete(self):
        return bool(self.flags & SLOT_COMPLETE)

    @property
    def is_full(self):
        return bool(self.flags & SLOT_FULL)

    def to_dict(self):
        d = {name: getattr(self, name) for name in _SLOT_FIELDS}
        d["complete"] = self.complete
        d["is_full"] = self.is_full
        return d


class Header:
    def __init__(self, data, offset=0):
        values = HDR_BASE.unpack_from(data, offset)
        for name, value in zip(_HDR_FIELDS, values):
            setattr(self, name, value)
        base = offset + HDR_BASE.size
        self.slots = []
        for i in range(MAX_DIR_ENTRIES):
            off = base + i * 72
            self.slots.append(SlotEntry(struct.unpack_from("<QQQQQQIIIIQ", data, off)))

    def to_dict(self):
        d = {name: getattr(self, name) for name in _HDR_FIELDS}
        d["mode_name"] = MODE_NAMES.get(self.mode, "?")
        d["state_name"] = STATE_NAMES.get(self.state, "?")
        d["slots"] = [s.to_dict() for s in self.slots if s.magic == SLOT_MAGIC]
        return d


class SlotHeader:
    def __init__(self, data, offset=0):
        values = SLOT_HDR.unpack_from(data, offset)
        for name, value in zip(_SLOT_HDR_FIELDS, values):
            setattr(self, name, value)

    def to_dict(self):
        d = {name: getattr(self, name) for name in _SLOT_HDR_FIELDS}
        d["complete"] = bool(self.flags & SLOT_COMPLETE)
        d["full"] = bool(self.flags & SLOT_FULL)
        d["truncated"] = bool(self.flags & SLOT_TRUNCATED)
        return d


def read_header(ram, offset=0):
    if offset + HDR.size > len(ram):
        raise ValueError("offset out of range")
    return Header(ram, offset)


def find_header(ram, step=HEADER_SIZE):
    """Return the offset of the first plausible snapshot header."""
    limit = len(ram) - HDR.size
    off = 0
    while off <= limit:
        if struct.unpack_from("<Q", ram, off)[0] == HEADER_MAGIC:
            hdr = Header(ram, off)
            if (hdr.version == VERSION and
                    hdr.header_size == HEADER_SIZE and
                    hdr.buffer_base == off and
                    hdr.buffer_size >= 64 * 1024 and
                    off + hdr.buffer_size <= len(ram)):
                return off
        off += step
    return None


def read_ranges(ram, header):
    ranges = []
    for i in range(header.range_count):
        off = header.buffer_base + header.ranges_offset + i * MEM_RANGE.size
        base, length, mtype, flags = MEM_RANGE.unpack_from(ram, off)
        ranges.append({"base": base, "length": length, "type": mtype, "flags": flags})
    return ranges


def dump_info(ram, offset=None):
    if offset is None:
        offset = find_header(ram)
        if offset is None:
            raise SystemExit("no snapshot header found in image")
    hdr = read_header(ram, offset)
    info = hdr.to_dict()
    info["header_offset"] = offset
    info["ranges"] = read_ranges(ram, hdr)
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["info"])
    ap.add_argument("ram")
    ap.add_argument("--offset", type=lambda x: int(x, 0))
    args = ap.parse_args()

    ram = open(args.ram, "rb").read()
    info = dump_info(ram, args.offset)
    json.dump(info, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
