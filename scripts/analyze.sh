#!/usr/bin/env bash
#
# Extract one RAM dump, verify page hashes, and run Volatility 3.
#
# Usage: scripts/analyze.sh RUN_DIR [--full]
#
# RUN_DIR is a directory containing ram.raw (and usually serial.log).
# Results land in out/analyzed/<run-id>/ and the extracted image stays in
# RUN_DIR/extract/.  This is the same pipeline the GUI's Analyze button runs.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/out"
RUN=""
FULL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --full) FULL=1; shift ;;
        --*) echo "unknown option: $1" >&2; exit 2 ;;
        *) RUN="$1"; shift ;;
    esac
done

[ -n "$RUN" ] || { echo "usage: analyze.sh RUN_DIR [--full]" >&2; exit 2; }
[ -d "$RUN" ] || { echo "missing run directory: $RUN" >&2; exit 1; }
RUN="$(cd "$RUN" && pwd)"
RAM="$RUN/ram.raw"
[ -f "$RAM" ] || { echo "missing RAM dump: $RAM" >&2; exit 1; }

RID="$(python3 - "$RUN" "$OUT" <<'PY'
import os, sys
print(os.path.relpath(sys.argv[1], sys.argv[2]).replace(os.sep, "__"))
PY
)"
DEST="$OUT/analyzed/$RID"
mkdir -p "$DEST"

echo "== run: $RUN"
echo "== results: $DEST"

EXTRACT_DIR="$RUN/extract"
echo
echo "== extracting snapshot =="
set +e
python3 "$ROOT/scripts/extract_snapshot.py" "$RAM" -o "$EXTRACT_DIR" 2>&1 | tee "$DEST/extract.log"
EXTRACT_RC=${PIPESTATUS[0]}
set -e
if [ "$EXTRACT_RC" -ne 0 ]; then
    echo "extraction failed (rc=$EXTRACT_RC)" >&2
    exit "$EXTRACT_RC"
fi

META="$(ls -t "$EXTRACT_DIR"/snapshot-*.json 2>/dev/null | head -1 || true)"
[ -n "$META" ] || { echo "no extraction metadata in $EXTRACT_DIR" >&2; exit 1; }
IMAGE="$(python3 - "$META" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))["image"])
PY
)"
[ -f "$IMAGE" ] || { echo "missing extracted image: $IMAGE" >&2; exit 1; }
echo "== image: $IMAGE"

SERIAL="$RUN/serial.log"
[ -f "$SERIAL" ] || SERIAL=""

echo
echo "== volatility: linux.pslist =="
set +e
"$ROOT/scripts/validate_volatility.sh" --image "$IMAGE" ${SERIAL:+--serial "$SERIAL"} \
    > "$DEST/pslist.txt" 2> "$DEST/pslist.err"
VOL_RC=$?
set -e
if [ "$VOL_RC" -ne 0 ]; then
    echo "volatility pslist failed (rc=$VOL_RC); see $DEST/pslist.err" >&2
    tail -20 "$DEST/pslist.err" >&2 || true
    exit "$VOL_RC"
fi

if [ "$FULL" -eq 1 ]; then
    for plugin in linux.pstree linux.bash; do
        short="${plugin#linux.}"
        echo "== volatility: $plugin =="
        set +e
        "$ROOT/tools/venv/bin/vol" -s "$ROOT/out/isf" -f "$IMAGE" "$plugin" \
            > "$DEST/$short.txt" 2> "$DEST/$short.err"
        rc=$?
        set -e
        [ "$rc" -eq 0 ] || echo "  $plugin failed (rc=$rc)" >&2
    done
fi

MISMATCH="$(python3 - "$META" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))["verification"]["hash_mismatches"])
PY
)"
echo
echo "== done: $DEST"
echo "   hash mismatches: $MISMATCH"
if [ "$MISMATCH" -ne 0 ]; then
    echo "   WARNING: extracted image does not match the captured hashes" >&2
    exit 2
fi
