#!/usr/bin/env python3
"""Local web GUI for the SMM snapshot-forensics prototype.

Stdlib only.  Serves a single-page app plus a JSON API that discovers every
RAM dump under out/, parses the snapshot header directly out of the dump, and
exposes ranges, slots, page maps, a memory heatmap, hexdumps, string search,
logs and Volatility output.

Run:
    python3 gui/server.py --port 8787
"""

import argparse
import json
import mmap
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out")
ANALYZED = os.path.join(OUT, "analyzed")
STATIC = os.path.join(ROOT, "gui", "static")
ISF_DIR = os.path.join(OUT, "isf")
VOL = os.path.join(ROOT, "tools", "venv", "bin", "vol")

sys.path.insert(0, os.path.join(ROOT, "scripts"))
import snapshot_format as fmt  # noqa: E402

MAGIC_BYTES = b"SNAPMMS1"  # little-endian 0x31534D4D50414E53

_cache = {}
_cache_lock = threading.Lock()
_jobs = {}
_jobs_lock = threading.Lock()


# --------------------------------------------------------------------------
# discovery and parsing
# --------------------------------------------------------------------------

def safe_rel(path):
    return os.path.relpath(path, OUT).replace(os.sep, "/")


def run_id(path):
    return safe_rel(path)


def run_dir_for_id(rid):
    rid = urllib.parse.unquote(rid)
    path = os.path.abspath(os.path.join(OUT, rid))
    if not path.startswith(os.path.abspath(OUT) + os.sep):
        raise ValueError("bad run id")
    return path


def find_ram_dumps():
    found = []
    if not os.path.isdir(OUT):
        return found
    for base, dirs, files in os.walk(OUT):
        if "ram.raw" in files:
            found.append(base)
        # Do not descend into extraction/analyzed output.
        dirs[:] = [d for d in dirs if d not in ("extract", "analyzed", "isf", "vol")]
    return sorted(found)


def find_header(ram_path):
    """Locate the snapshot header in a RAM dump.  Returns (offset, Header)."""
    size = os.path.getsize(ram_path)
    with open(ram_path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            pos = 0
            while True:
                off = mm.find(MAGIC_BYTES, pos)
                if off < 0:
                    return None
                try:
                    hdr = fmt.Header(mm, off)
                except Exception:
                    pos = off + 1
                    continue
                if (hdr.version == fmt.VERSION and
                        hdr.header_size == fmt.HEADER_SIZE and
                        hdr.buffer_base == off and
                        hdr.buffer_size >= 64 * 1024 and
                        off + hdr.buffer_size <= size):
                    return off, hdr
                pos = off + 1
        finally:
            mm.close()


def slot_details(ram_path, hdr):
    slots = []
    with open(ram_path, "rb") as f:
        for idx, entry in enumerate(hdr.slots):
            if entry.magic != fmt.SLOT_MAGIC:
                continue
            f.seek(hdr.buffer_base + entry.offset)
            raw = f.read(fmt.SLOT_HDR.size)
            if len(raw) < fmt.SLOT_HDR.size:
                continue
            sh = fmt.SlotHeader(raw)
            if sh.magic != fmt.SLOT_MAGIC:
                continue
            slots.append({
                "index": idx,
                "sequence": entry.sequence,
                "offset": entry.offset,
                "length": entry.length,
                "tick": entry.tick,
                "tsc": entry.tsc,
                "flags": entry.flags,
                "complete": bool(entry.flags & fmt.SLOT_COMPLETE),
                "full": bool(entry.flags & fmt.SLOT_FULL),
                "page_count": sh.page_count,
                "capacity_pages": sh.capacity_pages,
                "data_bytes": sh.data_bytes,
                "data_offset": sh.data_offset,
                "page_map_offset": sh.page_map_offset,
                "smi_tsc": sh.smi_tsc,
                "slot_flags": sh.flags,
            })
    slots.sort(key=lambda s: s["sequence"])
    return slots


def find_extraction(ram_path):
    """Return metadata from the newest extraction that used this dump."""
    target = os.path.abspath(ram_path)
    best = None
    for base, dirs, files in os.walk(OUT):
        for name in files:
            if not (name.startswith("snapshot-") and name.endswith(".json")):
                continue
            path = os.path.join(base, name)
            try:
                with open(path) as f:
                    meta = json.load(f)
            except Exception:
                continue
            if os.path.abspath(meta.get("source_dump", "")) != target:
                continue
            mtime = os.path.getmtime(path)
            if best is None or mtime > best[0]:
                best = (mtime, path, meta)
    if best is None:
        return None
    _, path, meta = best
    return {"meta_path": path, "meta": meta, "image": meta.get("image")}


def find_volatility(rid):
    base = os.path.join(ANALYZED, rid.replace("/", "__"))
    out = {}
    for key, name in (("pslist", "pslist.txt"), ("pstree", "pstree.txt"),
                      ("bash", "bash.txt"), ("malfind", "malfind.txt")):
        path = os.path.join(base, name)
        if os.path.isfile(path):
            try:
                with open(path, "r", errors="replace") as f:
                    out[key] = f.read()
            except Exception:
                pass
    return out or None


def read_log(path, lines=400):
    if not os.path.isfile(path):
        return ""
    try:
        with open(path, "r", errors="replace") as f:
            data = f.read()
    except Exception:
        return ""
    tail = data.splitlines()[-lines:]
    return "\n".join(tail)


def load_run(path):
    ram = os.path.join(path, "ram.raw")
    st = os.stat(ram)
    key = (ram, st.st_mtime, st.st_size)
    with _cache_lock:
        cached = _cache.get(key)
    if cached is not None:
        return cached

    found = find_header(ram)
    if found is None:
        info = {"id": run_id(path), "dir": safe_rel(path), "ram": ram,
                "error": "no snapshot header found", "mtime": st.st_mtime}
        with _cache_lock:
            _cache[key] = info
        return info

    off, hdr = found
    info = {
        "id": run_id(path),
        "dir": safe_rel(path),
        "ram": ram,
        "header_offset": off,
        "mtime": st.st_mtime,
        "header": hdr.to_dict(),
        "ranges": read_ranges_direct(ram, hdr),
        "slots": slot_details(ram, hdr),
        "serial": os.path.join(path, "serial.log"),
        "debug": os.path.join(path, "ovmf_debug.log"),
        "extraction": find_extraction(ram),
        "volatility": find_volatility(run_id(path)),
    }
    with _cache_lock:
        _cache[key] = info
    return info


def invalidate(rid):
    """Drop cached parse results for a run after its outputs change."""
    with _cache_lock:
        for key, info in list(_cache.items()):
            if info.get("id") == rid:
                _cache.pop(key, None)


def read_ranges_direct(ram_path, hdr):
    with open(ram_path, "rb") as f:
        f.seek(hdr.buffer_base + hdr.ranges_offset)
        raw = f.read(hdr.range_count * fmt.MEM_RANGE.size)
    ranges = []
    for i in range(hdr.range_count):
        base, length, mtype, flags = fmt.MEM_RANGE.unpack_from(raw, i * fmt.MEM_RANGE.size)
        ranges.append({"base": base, "length": length, "type": mtype, "flags": flags})
    return ranges


def header_from_file(ram_path, offset):
    with open(ram_path, "rb") as f:
        f.seek(offset)
        data = f.read(fmt.HDR.size)
    return fmt.Header(data, 0)


def summarise(path):
    info = load_run(path)
    if "error" in info:
        return {"id": info["id"], "dir": info["dir"], "error": info["error"]}
    h = info["header"]
    ext = info.get("extraction")
    return {
        "id": info["id"],
        "dir": info["dir"],
        "mode": h["mode_name"],
        "state": h["state_name"],
        "ticks": h["tick"],
        "smis": h["smi_count"],
        "sequence": h["sequence"],
        "pages_total": h["pages_total"],
        "pages_last": h["pages_last"],
        "bytes_copied": h["bytes_copied"],
        "buffer_size": h["buffer_size"],
        "guest_ram_top": h["guest_ram_top"],
        "smi_tsc_total": h["smi_tsc_total"],
        "smi_tsc_max": h["smi_tsc_max"],
        "has_extract": ext is not None,
        "has_vol": info.get("volatility") is not None,
        "hash_mismatches": (ext or {}).get("meta", {}).get("verification", {}).get("hash_mismatches"),
        "coverage": (ext or {}).get("meta", {}).get("verification", {}).get("coverage_of_ranges"),
        "mtime": info["mtime"],
    }


# --------------------------------------------------------------------------
# per-run operations
# --------------------------------------------------------------------------

def image_for(info):
    ext = info.get("extraction")
    if ext and ext.get("image") and os.path.isfile(ext["image"]):
        return ext["image"]
    return None


def hexdump(data, base, width=16):
    rows = []
    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        hexpart = " ".join("%02x" % b for b in chunk)
        asciipart = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        rows.append({"addr": base + off, "hex": hexpart, "ascii": asciipart})
    return rows


def read_at(info, pa, length, source="dump"):
    """Read guest physical memory from the original dump or the extracted image.

    The dump always contains the full address space, including the snapshot
    header and reserved buffer.  The extracted image only contains pages that
    were captured, so it reads as zeroes elsewhere.  Default to the dump.
    """
    img = image_for(info)
    order = ["image", "dump"] if source == "image" else ["dump", "image"]
    for which in order:
        path = img if which == "image" else info["ram"]
        if path is None or not os.path.isfile(path):
            continue
        if pa + length <= os.path.getsize(path):
            with open(path, "rb") as f:
                f.seek(pa)
                return f.read(length), which
    raise ValueError("address out of range")


def slot_pages(info, slot_index, offset, limit):
    hdr = header_from_file(info["ram"], info["header_offset"])
    if slot_index < 0 or slot_index >= fmt.MAX_DIR_ENTRIES:
        raise ValueError("bad slot")
    entry = hdr.slots[slot_index]
    if entry.magic != fmt.SLOT_MAGIC:
        return {"total": 0, "pages": []}
    base = hdr.buffer_base + entry.offset
    with open(info["ram"], "rb") as f:
        f.seek(base)
        sh = fmt.SlotHeader(f.read(fmt.SLOT_HDR.size))
        f.seek(base + sh.page_map_offset + offset * fmt.PAGE_ENTRY.size)
        raw = f.read(min(limit, max(0, sh.page_count - offset)) * fmt.PAGE_ENTRY.size)
    pages = []
    for i in range(0, len(raw), fmt.PAGE_ENTRY.size):
        pa, ph, flags = fmt.PAGE_ENTRY.unpack_from(raw, i)
        pages.append({"pa": pa, "hash": ph, "flags": flags, "index": offset + i // fmt.PAGE_ENTRY.size})
    return {"total": sh.page_count, "offset": offset, "pages": pages,
            "capacity": sh.capacity_pages, "sequence": sh.sequence}


def heatmap(info, bins):
    hdr = header_from_file(info["ram"], info["header_offset"])
    top = max(hdr.guest_ram_top, 1)
    counts = [0] * bins
    captured = set()
    with open(info["ram"], "rb") as f:
        for entry in hdr.slots:
            if entry.magic != fmt.SLOT_MAGIC or not (entry.flags & fmt.SLOT_COMPLETE):
                continue
            base = hdr.buffer_base + entry.offset
            f.seek(base)
            sh = fmt.SlotHeader(f.read(fmt.SLOT_HDR.size))
            f.seek(base + sh.page_map_offset)
            raw = f.read(sh.page_count * fmt.PAGE_ENTRY.size)
            for i in range(0, len(raw), fmt.PAGE_ENTRY.size):
                pa, _, _ = fmt.PAGE_ENTRY.unpack_from(raw, i)
                if pa in captured:
                    continue
                captured.add(pa)
                idx = min(bins - 1, (pa * bins) // top)
                counts[idx] += 1
    return {"bins": counts, "bin_size": top // bins, "max": max(counts) if counts else 0,
            "captured_pages": len(captured), "ram_top": top}


def search_image(info, needle, limit):
    img = image_for(info)
    path = img if img is not None else info["ram"]
    size = os.path.getsize(path)
    results = []
    with open(path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            pos = 0
            while len(results) < limit:
                off = mm.find(needle, pos)
                if off < 0:
                    break
                start = max(0, off - 32)
                ctx = mm[start:off + len(needle) + 32]
                results.append({
                    "pa": off,
                    "source": "image" if img else "dump",
                    "context": "".join(chr(b) if 32 <= b < 127 else "." for b in ctx),
                    "hex": " ".join("%02x" % b for b in ctx),
                })
                pos = off + max(1, len(needle))
        finally:
            mm.close()
    return {"matches": results, "truncated": len(results) >= limit}


def run_analyze(rid, job_id):
    script = os.path.join(ROOT, "scripts", "analyze.sh")
    log_path = os.path.join(ANALYZED, rid.replace("/", "__"), "analyze.log")
    rc = -1
    try:
        path = run_dir_for_id(rid)
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "w") as log:
            proc = subprocess.Popen([script, path], stdout=log, stderr=subprocess.STDOUT)
            with _jobs_lock:
                _jobs[job_id]["proc"] = proc
                _jobs[job_id]["log"] = log_path
            rc = proc.wait()
    except Exception as exc:  # keep the job terminal even on setup failures
        try:
            with open(log_path, "a") as log:
                log.write("analyze failed: %s\n" % exc)
        except OSError:
            pass
    finally:
        invalidate(rid)
        with _jobs_lock:
            _jobs[job_id]["status"] = "done" if rc == 0 else "failed"
            _jobs[job_id]["rc"] = rc
            _jobs[job_id]["finished"] = time.time()


# --------------------------------------------------------------------------
# HTTP layer
# --------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "SnapshotGUI/1.0"

    def log_message(self, fmt_, *args):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj, default=str).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _text(self, text, code=200, ctype="text/plain; charset=utf-8"):
        body = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _file(self, path):
        try:
            with open(path, "rb") as f:
                body = f.read()
        except OSError:
            return self._text("not found", 404)
        ctype = {"html": "text/html; charset=utf-8", "js": "application/javascript",
                 "css": "text/css", "svg": "image/svg+xml"}.get(
                     path.rsplit(".", 1)[-1], "application/octet-stream")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        try:
            if path in ("/", "/index.html"):
                return self._file(os.path.join(STATIC, "index.html"))
            if path.startswith("/static/"):
                return self._file(os.path.join(STATIC, os.path.basename(path)))
            if path == "/api/runs":
                return self._json({"runs": [summarise(p) for p in find_ram_dumps()]})
            if path == "/api/jobs":
                with _jobs_lock:
                    jobs = [{"id": k, "status": v["status"], "rid": v["rid"],
                             "log": v.get("log")} for k, v in _jobs.items()]
                return self._json({"jobs": jobs})
            if path.startswith("/api/run/"):
                rest = path[len("/api/run/"):]
                rid, _, action = rest.partition("/")
                info = load_run(run_dir_for_id(rid))
                if "error" in info:
                    return self._json(info, 404)
                if action == "":
                    payload = dict(info)
                    payload["serial_tail"] = read_log(info["serial"], 250)
                    payload["debug_tail"] = read_log(info["debug"], 250)
                    payload["header"] = info["header"]
                    return self._json(payload)
                if action == "hex":
                    pa = int(qs.get("pa", ["0"])[0], 0)
                    length = min(int(qs.get("len", ["256"])[0], 0), 65536)
                    source = qs.get("source", ["dump"])[0]
                    if source not in ("dump", "image"):
                        source = "dump"
                    data, source = read_at(info, pa, length, source)
                    return self._json({"pa": pa, "len": len(data), "source": source,
                                       "rows": hexdump(data, pa)})
                if action == "search":
                    needle = qs.get("q", [""])[0].encode()
                    if not needle:
                        return self._json({"matches": []})
                    limit = min(int(qs.get("limit", ["50"])[0]), 500)
                    return self._json(search_image(info, needle, limit))
                if action == "pages":
                    idx = int(qs.get("slot", ["0"])[0])
                    off = max(0, int(qs.get("offset", ["0"])[0]))
                    lim = min(int(qs.get("limit", ["200"])[0]), 2000)
                    return self._json(slot_pages(info, idx, off, lim))
                if action == "heatmap":
                    bins = min(max(int(qs.get("bins", ["256"])[0]), 16), 1024)
                    return self._json(heatmap(info, bins))
                if action == "log":
                    which = qs.get("which", ["serial"])[0]
                    lines = min(int(qs.get("lines", ["400"])[0]), 4000)
                    src = info["serial"] if which == "serial" else info["debug"]
                    return self._text(read_log(src, lines))
                return self._json({"error": "unknown action"}, 404)
            return self._text("not found", 404)
        except Exception as exc:  # keep the UI alive on malformed input
            return self._json({"error": str(exc)}, 400)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path.startswith("/api/run/") and parsed.path.endswith("/analyze"):
            rid = urllib.parse.unquote(parsed.path[len("/api/run/"):-len("/analyze")])
            try:
                run_path = run_dir_for_id(rid)
            except ValueError:
                return self._json({"error": "bad run id"}, 400)
            if not os.path.isfile(os.path.join(run_path, "ram.raw")):
                return self._json({"error": "run has no ram.raw"}, 400)
            job_id = "job-%d" % int(time.time() * 1000)
            with _jobs_lock:
                _jobs[job_id] = {"rid": rid, "status": "running", "started": time.time()}
            threading.Thread(target=run_analyze, args=(rid, job_id), daemon=True).start()
            return self._json({"job": job_id})
        return self._json({"error": "not found"}, 404)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    os.makedirs(ANALYZED, exist_ok=True)
    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print("snapshot GUI: http://%s:%d/" % (args.host, args.port), flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
