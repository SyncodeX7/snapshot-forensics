#!/usr/bin/env python3
"""Summarise extract_snapshot.py metadata files into a benchmark table.

Usage: bench_report.py OUTDIR [--period 8] [--json]
"""

import argparse
import glob
import json
import os
import sys


def load_runs(outdir):
    runs = []
    for path in sorted(glob.glob(os.path.join(outdir, "**", "snapshot-*.json"), recursive=True)):
        with open(path) as f:
            meta = json.load(f)
        meta["_path"] = path
        runs.append(meta)
    return runs


def tsc_hz(meta, period):
    if meta.get("header", {}).get("tsc_hz"):
        return float(meta["header"]["tsc_hz"])
    slots = meta.get("slots_applied", [])
    if len(slots) >= 2:
        first = slots[0]["dir"]
        last = slots[-1]["dir"]
        dt = last["tick"] - first["tick"]
        dtsc = last["tsc"] - first["tsc"]
        if dt > 0 and dtsc > 0:
            return dtsc * period / float(dt)
    hdr_slots = [s for s in meta.get("header", {}).get("slots", []) if s.get("magic") == 6075158343058542163]
    if len(hdr_slots) >= 2:
        dt = hdr_slots[-1]["tick"] - hdr_slots[0]["tick"]
        dtsc = hdr_slots[-1]["tsc"] - hdr_slots[0]["tsc"]
        if dt > 0 and dtsc > 0:
            return dtsc * period / float(dt)
    try:
        with open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") as f:
            return float(f.read().strip()) * 1000.0
    except Exception:
        return 3.0e9


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--period", type=int, default=8)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    runs = load_runs(args.outdir)
    if not runs:
        print("no snapshot metadata under %s" % args.outdir, file=sys.stderr)
        return 1

    rows = []
    for meta in runs:
        h = meta["header"]
        v = meta["verification"]
        hz = tsc_hz(meta, args.period)
        smi_total_ms = h["smi_tsc_total"] / hz * 1000.0 if hz else 0.0
        smi_max_ms = h["smi_tsc_max"] / hz * 1000.0 if hz else 0.0
        smi_last_ms = h["smi_tsc_last"] / hz * 1000.0 if hz else 0.0
        window_s = h["smi_count"] * args.period
        overhead = (smi_total_ms / 1000.0 / window_s * 100.0) if window_s else 0.0
        rows.append({
            "run": os.path.basename(os.path.dirname(meta["_path"])),
            "mode": h["mode_name"],
            "seq": h["sequence"],
            "ticks": h["tick"],
            "smis": h["smi_count"],
            "slots": len(meta["slots_applied"]),
            "pages": v["unique_pages"],
            "mib": round(v["unique_bytes"] / 1048576.0, 1),
            "copied_mib": round(h["bytes_copied"] / 1048576.0, 1),
            "smi_total_ms": round(smi_total_ms, 1),
            "smi_max_ms": round(smi_max_ms, 1),
            "smi_last_ms": round(smi_last_ms, 1),
            "overhead_pct": round(overhead, 3),
            "tsc_hz": int(hz),
            "hash_bad": v["hash_mismatches"],
            "truncated": sum(1 for s in meta["slots_applied"] if s["dir"].get("flags", 0) & 4),
            "coverage_pct": round(v["coverage_of_ranges"] * 100.0, 2),
        })

    if args.json:
        json.dump(rows, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0

    cols = ["run", "mode", "seq", "smis", "slots", "pages", "mib", "copied_mib",
            "smi_total_ms", "smi_max_ms", "overhead_pct", "hash_bad", "coverage_pct"]
    widths = {c: max(len(c), *(len(str(r[c])) for r in rows)) for c in cols}
    header = "  ".join(c.ljust(widths[c]) for c in cols)
    print(header)
    print("  ".join("-" * widths[c] for c in cols))
    for r in rows:
        print("  ".join(str(r[c]).ljust(widths[c]) for c in cols))
    return 0


if __name__ == "__main__":
    sys.exit(main())
