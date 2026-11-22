#!/usr/bin/env bash
#
# End-to-end benchmark: build OVMF for each capture mode, boot the guest, dump
# RAM, extract/verify snapshots, and print a comparison table.
#
# Usage: scripts/benchmark.sh [options]
#   --modes LIST       comma separated modes (default A,B,C,D)
#   --seconds N        guest runtime after boot per mode (default 90)
#   --period N         periodic SMI period in seconds (default 8)
#   --max-pages N      pages per SMI for mode D (default 8192)
#   --skip-build       reuse existing OVMF images under out/bench
#   --no-restore       do not rebuild the default (mode A) OVMF at the end
#   --jobs N           build parallelism
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODES="A,B,C,D"
SECS=100
PERIOD=8
MAX_PAGES=8192
SKIP_BUILD=0
RESTORE=1
JOBS="$(nproc)"
BENCH="$ROOT/out/bench"
EDK2="$ROOT/edk2"
BUILD_DIR="$EDK2/Build/OvmfX64/DEBUG_GCC/FV"

while [ $# -gt 0 ]; do
    case "$1" in
        --modes) MODES="$2"; shift 2 ;;
        --seconds) SECS="$2"; shift 2 ;;
        --period) PERIOD="$2"; shift 2 ;;
        --max-pages) MAX_PAGES="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --no-restore) RESTORE=0; shift ;;
        --jobs) JOBS="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$BENCH"

mode_index() {
    case "$1" in
        A) echo 0 ;;
        B) echo 1 ;;
        C) echo 2 ;;
        D) echo 3 ;;
        *) echo "bad mode $1" >&2; exit 2 ;;
    esac
}

build_ovmf() {
    local mode="$1"
    (
        cd "$EDK2"
        export WORKSPACE="$EDK2"
        export PACKAGES_PATH="$EDK2:$ROOT"
        export EDK_TOOLS_PATH="$EDK2/BaseTools"
        export PATH="$ROOT/tools/acpica-R2025_04_04/generate/unix/bin:$PATH"
        export PYTHON_COMMAND=python3
        set +u
        source edksetup.sh BaseTools >/dev/null
        set -u
        build -a X64 -t GCC -p OvmfPkg/OvmfPkgX64.dsc -D SMM_REQUIRE=TRUE \
              -b DEBUG -n "$JOBS" >"$ROOT/logs/bench-build-$mode.log" 2>&1
    )
}

IFS=',' read -ra MODE_LIST <<< "$MODES"

for M in "${MODE_LIST[@]}"; do
    IDX="$(mode_index "$M")"
    echo "===== mode $M ====="

    if [ "$SKIP_BUILD" -eq 0 ]; then
        EXTRA=(--mode "$IDX" --period "$PERIOD")
        if [ "$M" = "D" ]; then
            EXTRA+=(--max-pages "$MAX_PAGES")
        fi
        python3 "$ROOT/scripts/integrate_edk2.py" --edk2 "$EDK2" "${EXTRA[@]}" >/dev/null
        echo "building OVMF for mode $M"
        build_ovmf "$M"
        cp "$BUILD_DIR/OVMF_CODE.fd" "$BENCH/OVMF_CODE-$M.fd"
        cp "$BUILD_DIR/OVMF_VARS.fd" "$BENCH/OVMF_VARS-$M.fd"
    fi

    RUN="$BENCH/run-$M"
    rm -rf "$RUN"
    RUN_SECS="$SECS"
    if [ "$M" = "D" ] && [ "$RUN_SECS" -lt 170 ]; then
        RUN_SECS=170
    fi

    echo "booting guest for mode $M (${RUN_SECS}s after boot)"
    "$ROOT/scripts/run_qemu.sh" \
        --out "$RUN" \
        --code "$BENCH/OVMF_CODE-$M.fd" \
        --vars "$BENCH/OVMF_VARS-$M.fd" \
        --seconds "$RUN_SECS"

    echo "extracting snapshot for mode $M"
    EXTRACT_OK=1
    python3 "$ROOT/scripts/extract_snapshot.py" "$RUN/ram.raw" \
        -o "$BENCH/extract-$M" --tag "$M" || EXTRACT_OK=0
    if [ "$EXTRACT_OK" -eq 0 ]; then
        echo "mode $M did not produce a complete snapshot; skipping validation"
        continue
    fi

    echo "validating forensics for mode $M"
    IMG="$(ls -t "$BENCH/extract-$M"/ram-*.raw 2>/dev/null | head -n 1 || true)"
    if [ -n "$IMG" ] && [ -f "$IMG" ]; then
        "$ROOT/scripts/validate_volatility.sh" \
            --image "$IMG" \
            --serial "$RUN/serial.log" || true
        # Keep a per-run copy where the GUI looks for Volatility output.
        mkdir -p "$ROOT/out/analyzed/bench__run-$M"
        cp "$ROOT/out/vol/pslist.txt" "$ROOT/out/analyzed/bench__run-$M/pslist.txt" 2>/dev/null || true
    fi
done

if [ "$RESTORE" -eq 1 ] && [ "$SKIP_BUILD" -eq 0 ]; then
    echo
    echo "restoring the default OVMF build (mode A)"
    python3 "$ROOT/scripts/integrate_edk2.py" --edk2 "$EDK2" --mode 0 --period "$PERIOD" >/dev/null
    build_ovmf A
fi

echo
echo "===== summary ====="
python3 "$ROOT/scripts/bench_report.py" "$BENCH" --period "$PERIOD"
