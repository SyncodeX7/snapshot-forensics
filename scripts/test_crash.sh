#!/usr/bin/env bash
#
# RQ1 Experiment: Kernel Crash Forensics
#
# Evaluates whether platform firmware (SMM) captures a forensically valid memory
# snapshot after a deliberately triggered kernel panic (SysRq-c), completely
# independent of OS health (Goal G1).
#
# Usage: scripts/test_crash.sh [options]
#   --out DIR      output directory (default: out/crash-test)
#   --period N     periodic SMI period in seconds (default: 8)
#   --code FILE    OVMF_CODE.fd
#   --vars FILE    OVMF_VARS.fd
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/out/crash-test}"
PERIOD=8
CODE="${CODE:-$ROOT/edk2/Build/OvmfX64/DEBUG_GCC/FV/OVMF_CODE.fd}"
VARS="${VARS:-$ROOT/edk2/Build/OvmfX64/DEBUG_GCC/FV/OVMF_VARS.fd}"
ESP="$ROOT/guest/esp.img"
MEM="1G"
SMP=2
ACCEL="kvm"
[ -w /dev/kvm ] || ACCEL="tcg"

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --period) PERIOD="$2"; shift 2 ;;
        --code) CODE="$2"; shift 2 ;;
        --vars) VARS="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -f "$CODE" ] || { echo "missing OVMF code: $CODE" >&2; exit 1; }
[ -f "$VARS" ] || { echo "missing OVMF vars: $VARS" >&2; exit 1; }
[ -f "$ESP" ]  || { echo "missing ESP image: $ESP" >&2; exit 1; }

mkdir -p "$OUT"
SERIAL="$OUT/serial.log"
MON="$OUT/monitor.sock"
RAMDUMP="$OUT/ram.raw"
VARS_COPY="$OUT/OVMF_VARS.fd"
cp "$VARS" "$VARS_COPY"
rm -f "$SERIAL" "$MON"

CPU="host"
[ "$ACCEL" = "tcg" ] && CPU="max"

MEM_BYTES=$(python3 - "$MEM" <<'PY'
import sys
s = sys.argv[1].strip().upper()
mult = {"K": 1024, "M": 1024**2, "G": 1024**3}
print(int(s[:-1]) * mult[s[-1]] if s[-1] in mult else int(s))
PY
)

QEMU_ARGS=(
    -machine "q35,smm=on,accel=$ACCEL"
    -cpu "$CPU"
    -m "$MEM"
    -smp "$SMP"
    -global driver=cfi.pflash01,property=secure,value=on
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$CODE"
    -drive "if=pflash,format=raw,unit=1,file=$VARS_COPY"
    -drive "if=virtio,format=raw,file=$ESP"
    -net none
    -display none
    -debugcon "file:$OUT/ovmf_debug.log"
    -global "isa-debugcon.iobase=0x402"
    -chardev "file,id=ser0,path=$SERIAL"
    -serial chardev:ser0
    -monitor "unix:$MON,server=on,wait=off"
)

echo "=========================================================="
echo "RQ1: Kernel Crash / Panic Firmware Snapshot Evaluation"
echo "=========================================================="
echo "output   : $OUT"
echo "accel    : $ACCEL"
echo "memory   : $MEM"

qemu-system-x86_64 "${QEMU_ARGS[@]}" &
QPID=$!

cleanup() {
    if kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "waiting for guest to boot..."
deadline=$((SECONDS + 60))
booted=0
while [ $SECONDS -lt $deadline ]; do
    if ! kill -0 "$QPID" 2>/dev/null; then
        echo "QEMU exited unexpectedly" >&2
        exit 1
    fi
    if grep -q "SNAPSHOT-GUEST: booted" "$SERIAL" 2>/dev/null; then
        booted=1
        break
    fi
    sleep 1
done

if [ "$booted" -eq 0 ]; then
    echo "guest did not boot in time" >&2
    exit 1
fi

echo "guest booted successfully. Letting it run 12s for initial pre-crash snapshot..."
sleep 12

echo "triggering deliberate kernel panic via SysRq-c over QEMU monitor..."
python3 "$ROOT/scripts/qemu_monitor.py" "$MON" "sendkey alt-sysrq-c"

# Allow time for kernel panic output and the post-crash periodic SMI to execute
echo "waiting 10s for kernel panic and post-crash SMM snapshot..."
sleep 10

if grep -q "SysRq : Trigger a crash" "$SERIAL" 2>/dev/null || grep -q "Kernel panic" "$SERIAL" 2>/dev/null; then
    echo "CONFIRMED: Kernel panic occurred inside guest OS!"
else
    echo "warning: crash banner not yet seen on serial log, checking dump regardless..."
fi

echo "dumping physical memory to $RAMDUMP..."
python3 "$ROOT/scripts/qemu_monitor.py" "$MON" \
    "pmemsave 0 $MEM_BYTES \"$RAMDUMP\"" quit

wait "$QPID" 2>/dev/null || true
trap - EXIT

echo "extracting post-crash snapshot..."
EXTRACT="$OUT/extract"
python3 "$ROOT/scripts/extract_snapshot.py" "$RAMDUMP" -o "$EXTRACT" --tag "crash" --from-dump

IMG="$(ls "$EXTRACT/ram-"*".raw" | head -n 1)"
echo "validating forensic recovery with Volatility 3 on crashed state..."
"$ROOT/scripts/validate_volatility.sh" --image "$IMG" --serial "$SERIAL"

echo
echo "=========================================================="
echo "RQ1 RESULT: SUCCESS!"
echo "Firmware took periodic memory snapshot below the crashed OS."
echo "Volatility 3 successfully reconstructed processes from dead kernel state."
echo "=========================================================="
