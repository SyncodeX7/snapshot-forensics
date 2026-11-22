#!/usr/bin/env bash
#
# Generate an ISF from the guest vmlinux and run Volatility 3 linux.pslist on
# an extracted snapshot image.
#
# Usage: scripts/validate_volatility.sh --image out/bench/extract-A/ram-123.raw
#        [--isf FILE] [--serial FILE] [--plugin linux.pslist]
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VMLINUX="${VMLINUX:-$ROOT/guest/kbuild/vmlinux}"
ISF="${ISF:-$ROOT/out/isf/linux-6.12.58.json}"
IMAGE=""
SERIAL=""
PLUGIN="linux.pslist"
VOL="$ROOT/tools/venv/bin/vol"
DWARF2JSON="$ROOT/tools/bin/dwarf2json"

while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE="$2"; shift 2 ;;
        --isf) ISF="$2"; shift 2 ;;
        --serial) SERIAL="$2"; shift 2 ;;
        --plugin) PLUGIN="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -n "$IMAGE" ] || { echo "--image is required" >&2; exit 2; }
[ -f "$IMAGE" ] || { echo "missing image: $IMAGE" >&2; exit 1; }
[ -f "$VMLINUX" ] || { echo "missing vmlinux: $VMLINUX (run scripts/build_guest.sh)" >&2; exit 1; }
[ -x "$VOL" ] || { echo "missing volatility3 venv: $VOL" >&2; exit 1; }

if [ ! -f "$ISF" ]; then
    echo "== generating ISF from $VMLINUX =="
    mkdir -p "$(dirname "$ISF")"
    "$DWARF2JSON" linux --elf "$VMLINUX" > "$ISF"
    ls -lh "$ISF"
fi

echo "== volatility3 $PLUGIN =="
OUT="$ROOT/out/vol"
mkdir -p "$OUT"
"$VOL" -s "$(dirname "$ISF")" -f "$IMAGE" "$PLUGIN" | tee "$OUT/pslist.txt"

if [ -n "$SERIAL" ] && [ -f "$SERIAL" ]; then
    echo
    echo "== cross-check against guest serial log =="
    grep "SNAPSHOT-GUEST" "$SERIAL" || true
    for name in init snapw0 snapw1 snapw2 snapw3; do
        if grep -q "$name" "$OUT/pslist.txt"; then
            echo "  found $name in pslist"
        else
            echo "  MISSING $name in pslist" >&2
        fi
    done
fi
