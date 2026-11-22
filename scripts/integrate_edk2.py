#!/usr/bin/env python3
"""Idempotently wire SnapshotPkg into OvmfPkgX64.dsc / OvmfPkgX64.fdf.

Usage: scripts/integrate_edk2.py [--edk2 DIR] [--mode N] [--buffer BYTES]

The script adds:
  * the SnapshotPkg PCDs and PcdCpuSmmRestrictedMemoryAccess=FALSE to
    [PcdsFixedAtBuild]
  * the three drivers to [Components]
  * the three INFs to [FV.DXEFV]

It is safe to run repeatedly; each block is delimited by markers.
"""

import argparse
import os
import re
import sys


def fail(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def replace_block(text, begin, end, block, insert_after=None, insert_before=None):
    """Replace an existing marked block or insert a new one."""
    if begin in text:
        pattern = re.compile(re.escape(begin) + r".*?" + re.escape(end) + r"\n?", re.S)
        return pattern.sub(block, text, count=1)

    if insert_after is not None:
        idx = text.find(insert_after)
        if idx < 0:
            fail("anchor not found: %r" % insert_after)
        idx += len(insert_after)
        return text[:idx] + "\n" + block + text[idx:]

    if insert_before is not None:
        idx = text.find(insert_before)
        if idx < 0:
            fail("anchor not found: %r" % insert_before)
        return text[:idx] + block + "\n" + text[idx:]

    fail("no insertion point")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--edk2", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "edk2"))
    ap.add_argument("--mode", type=int, default=0, help="0=A full, 1=B hash, 2=C dirty, 3=D sliced")
    ap.add_argument("--buffer", default="0x20000000", help="requested snapshot buffer bytes (hex or dec)")
    ap.add_argument("--period", type=int, default=8)
    ap.add_argument("--max-pages", type=int, default=0)
    ap.add_argument("--delta-slots", type=int, default=16)
    ap.add_argument("--full-every", type=int, default=8)
    ap.add_argument("--reclaim", default="TRUE", choices=["TRUE", "FALSE"])
    args = ap.parse_args()

    dsc_path = os.path.join(args.edk2, "OvmfPkg", "OvmfPkgX64.dsc")
    fdf_path = os.path.join(args.edk2, "OvmfPkg", "OvmfPkgX64.fdf")
    for p in (dsc_path, fdf_path):
        if not os.path.isfile(p):
            fail("missing %s" % p)

    dsc = open(dsc_path, "r", encoding="utf-8").read()
    fdf = open(fdf_path, "r", encoding="utf-8").read()

    pcd_begin, pcd_end = "  # >>> snapshot-forensics pcd >>>", "  # <<< snapshot-forensics pcd <<<"
    comp_begin, comp_end = "  # >>> snapshot-forensics components >>>", "  # <<< snapshot-forensics components <<<"
    fdf_begin, fdf_end = "# >>> snapshot-forensics fdf >>>", "# <<< snapshot-forensics fdf <<<"

    pcd_block = "\n".join([
        pcd_begin,
        "  #",
        "  # SnapshotPkg: firmware-level periodic physical memory snapshots.",
        "  # PcdCpuSmmRestrictedMemoryAccess must be FALSE so SMM can read",
        "  # ordinary guest RAM and the EfiReservedMemoryType snapshot buffer.",
        "  #",
        "  gSnapshotPkgTokenSpaceGuid.PcdSnapshotMode|%d" % args.mode,
        "  gSnapshotPkgTokenSpaceGuid.PcdSnapshotBufferSize|%s" % args.buffer,
        "  gSnapshotPkgTokenSpaceGuid.PcdSnapshotPeriodSeconds|%d" % args.period,
        "  gSnapshotPkgTokenSpaceGuid.PcdSnapshotMaxPagesPerSmi|%d" % args.max_pages,
        "  gSnapshotPkgTokenSpaceGuid.PcdSnapshotDeltaSlots|%d" % args.delta_slots,
        "  gSnapshotPkgTokenSpaceGuid.PcdSnapshotIncludeReclaimable|%s" % args.reclaim,
        "  gSnapshotPkgTokenSpaceGuid.PcdSnapshotFullEvery|%d" % args.full_every,
        "  gSnapshotPkgTokenSpaceGuid.PcdSnapshotTscHz|0",
        "  gUefiCpuPkgTokenSpaceGuid.PcdCpuSmmRestrictedMemoryAccess|FALSE",
        "  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE",
        pcd_end,
    ]) + "\n"

    comp_block = "\n".join([
        comp_begin,
        "  #",
        "  # SnapshotPkg",
        "  #",
        "  SnapshotPkg/SmmPeriodicTimerDxe/SmmPeriodicTimerDxe.inf",
        "  SnapshotPkg/SmmSnapshotDriver/SmmSnapshotDriver.inf",
        "  SnapshotPkg/SnapshotDxe/SnapshotDxe.inf",
        comp_end,
    ]) + "\n"

    fdf_block = "\n".join([
        fdf_begin,
        "#",
        "# SnapshotPkg",
        "#",
        "INF  SnapshotPkg/SmmPeriodicTimerDxe/SmmPeriodicTimerDxe.inf",
        "INF  SnapshotPkg/SmmSnapshotDriver/SmmSnapshotDriver.inf",
        "INF  SnapshotPkg/SnapshotDxe/SnapshotDxe.inf",
        fdf_end,
    ]) + "\n"

    # 1. PCDs: insert at the top of [PcdsFixedAtBuild].
    anchor = "[PcdsFixedAtBuild]\n"
    if anchor not in dsc:
        fail("[PcdsFixedAtBuild] not found in DSC")
    dsc = replace_block(dsc, pcd_begin, pcd_end, pcd_block, insert_after=anchor)

    # 2. Components: append at the end of the file (it ends inside [Components]).
    if comp_begin not in dsc:
        dsc = dsc.rstrip("\n") + "\n\n" + comp_block
    else:
        dsc = replace_block(dsc, comp_begin, comp_end, comp_block)

    # 3. FDF: insert at the end of [FV.DXEFV], just before the separator that
    #    precedes [FV.FVMAIN_COMPACT].
    anchor = "\n################################################################################\n\n[FV.FVMAIN_COMPACT]"
    if anchor not in fdf:
        fail("[FV.FVMAIN_COMPACT] separator not found in FDF")
    fdf = replace_block(fdf, fdf_begin, fdf_end, fdf_block, insert_before=anchor)

    open(dsc_path, "w", encoding="utf-8").write(dsc)
    open(fdf_path, "w", encoding="utf-8").write(fdf)
    print("patched %s" % dsc_path)
    print("patched %s" % fdf_path)


if __name__ == "__main__":
    main()
