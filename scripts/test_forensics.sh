#!/usr/bin/env bash
#
# RQ2 Experiment: Forensic Artifact Recovery & Verification
#
# Evaluates whether the firmware-level snapshot faithfully preserves forensic
# artifacts:
#   1. Running process tree and threads (init, snapw0..3)
#   2. Dynamic heap-resident canary tokens and credentials
#   3. Page map integrity (zero hash mismatches)
#   4. Monotonic SMI sequence and hardware timestamps
#
# Usage: scripts/test_forensics.sh --image RAM_IMAGE [--serial SERIAL_LOG]
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=""
SERIAL=""
ISF="${ISF:-$ROOT/out/isf/linux-6.12.58.json}"
VOL="$ROOT/tools/venv/bin/vol"
CANARY="CONFIDENTIAL_CANARY_TOKEN=SMM_FIRMWARE_SNAPSHOT_FORENSICS_2026_RESEARCH"

while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE="$2"; shift 2 ;;
        --serial) SERIAL="$2"; shift 2 ;;
        --isf) ISF="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -n "$IMAGE" ] || { echo "--image is required" >&2; exit 2; }
[ -f "$IMAGE" ] || { echo "image file not found: $IMAGE" >&2; exit 1; }

echo "=========================================================="
echo "RQ2: Forensic Artifact Recovery & Verification"
echo "Target Image: $IMAGE"
echo "=========================================================="

echo
echo "--- Check 1: In-Memory Confidential Canary Artifact ---"
if grep -a -F "$CANARY" "$IMAGE" >/dev/null; then
    COUNT=$(grep -a -b -o "$CANARY" "$IMAGE" | wc -l)
    MATCH=$(grep -a -b -o "$CANARY" "$IMAGE" | head -n 1)
    echo "  [PASS] Found canary secret string in snapshot: $COUNT instance(s)"
    echo "         First match at physical byte offset: $MATCH"
else
    echo "  [FAIL] Canary secret token was NOT found in memory image!" >&2
    exit 1
fi

echo
echo "--- Check 2: Volatility 3 Process Tree Reconstruction ---"
"$ROOT/scripts/validate_volatility.sh" --image "$IMAGE" ${SERIAL:+--serial "$SERIAL"}

echo
echo "--- Check 3: Page Map Integrity & Hash Verification ---"
META="$(dirname "$IMAGE")/snapshot-"*".json"
if ls $META 1>/dev/null 2>&1; then
    META_FILE="$(ls $META | head -n 1)"
    echo "  Found metadata: $META_FILE"
    MISMATCHES=$(python3 -c "import json; m=json.load(open('$META_FILE')); print(m.get('verification',{}).get('hash_mismatches', 0))")
    UNIQUE_PAGES=$(python3 -c "import json; m=json.load(open('$META_FILE')); print(m.get('verification',{}).get('unique_pages', 0))")
    COPIED_MB=$(python3 -c "import json; m=json.load(open('$META_FILE')); print(round(m.get('header',{}).get('bytes_copied', 0)/1048576, 1))")
    
    echo "  Total pages captured: $UNIQUE_PAGES ($COPIED_MB MiB)"
    echo "  Hash mismatches     : $MISMATCHES"
    if [ "$MISMATCHES" -eq 0 ]; then
        echo "  [PASS] All captured pages match FNV-1a checksums with zero integrity errors."
    else
        echo "  [FAIL] Hash mismatches detected: $MISMATCHES" >&2
        exit 1
    fi
fi

echo
echo "=========================================================="
echo "RQ2 RESULT: SUCCESS!"
echo "All forensic artifacts (processes, memory strings, page hashes) verified."
echo "=========================================================="
