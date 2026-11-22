#!/usr/bin/env bash
#
# Boot OVMF (SMM enabled) + the minimal Linux guest and dump guest RAM.
#
# Usage: scripts/run_qemu.sh [options]
#   --out DIR          output directory (default: out/run-<utc>)
#   --seconds N        seconds to run after the guest reports it booted (default 60)
#   --boot-timeout N   seconds to wait for the guest banner (default 180)
#   --code FILE        OVMF_CODE.fd (default: built OVMF)
#   --vars FILE        OVMF_VARS.fd template (default: built OVMF)
#   --esp FILE         ESP image (default: guest/esp.img)
#   --mem SIZE         guest memory (default 512M)
#   --smp N            vCPUs (default 2)
#   --accel kvm|tcg    accelerator (default: kvm if /dev/kvm exists)
#   --no-dump          do not dump RAM, just run
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/out/run-$(date -u +%Y%m%d-%H%M%S)"
SECONDS_AFTER_BOOT=60
BOOT_TIMEOUT=180
CODE="$ROOT/edk2/Build/OvmfX64/DEBUG_GCC/FV/OVMF_CODE.fd"
VARS="$ROOT/edk2/Build/OvmfX64/DEBUG_GCC/FV/OVMF_VARS.fd"
ESP="$ROOT/guest/esp.img"
MEM="1G"
SMP=2
ACCEL=""
DUMP=1

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --seconds) SECONDS_AFTER_BOOT="$2"; shift 2 ;;
        --boot-timeout) BOOT_TIMEOUT="$2"; shift 2 ;;
        --code) CODE="$2"; shift 2 ;;
        --vars) VARS="$2"; shift 2 ;;
        --esp) ESP="$2"; shift 2 ;;
        --mem) MEM="$2"; shift 2 ;;
        --smp) SMP="$2"; shift 2 ;;
        --accel) ACCEL="$2"; shift 2 ;;
        --no-dump) DUMP=0; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -f "$CODE" ] || { echo "missing OVMF code: $CODE" >&2; exit 1; }
[ -f "$VARS" ] || { echo "missing OVMF vars: $VARS" >&2; exit 1; }
[ -f "$ESP" ]  || { echo "missing ESP image: $ESP (run scripts/build_guest.sh)" >&2; exit 1; }

if [ -z "$ACCEL" ]; then
    if [ -w /dev/kvm ]; then ACCEL=kvm; else ACCEL=tcg; fi
fi

mkdir -p "$OUT"
SERIAL="$OUT/serial.log"
MON="$OUT/monitor.sock"
RAMDUMP="$OUT/ram.raw"
VARS_COPY="$OUT/OVMF_VARS.fd"
cp "$VARS" "$VARS_COPY"
rm -f "$SERIAL" "$MON"

# Memory size in bytes for pmemsave.
MEM_BYTES=$(python3 - "$MEM" <<'PY'
import sys
s = sys.argv[1].strip().upper()
mult = {"K": 1024, "M": 1024**2, "G": 1024**3}
if s[-1] in mult:
    print(int(s[:-1]) * mult[s[-1]])
else:
    print(int(s))
PY
)

CPU="host"
[ "$ACCEL" = "tcg" ] && CPU="max"

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

echo "output   : $OUT"
echo "accel    : $ACCEL"
echo "memory   : $MEM ($MEM_BYTES bytes)"
echo "OVMF code: $CODE"

qemu-system-x86_64 "${QEMU_ARGS[@]}" &
QPID=$!

cleanup() {
    if kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Wait for the guest banner.
deadline=$((SECONDS + BOOT_TIMEOUT))
booted=0
while [ $SECONDS -lt $deadline ]; do
    if ! kill -0 "$QPID" 2>/dev/null; then
        echo "QEMU exited before the guest booted" >&2
        tail -40 "$SERIAL" 2>/dev/null || true
        exit 1
    fi
    if grep -q "SNAPSHOT-GUEST: booted" "$SERIAL" 2>/dev/null; then
        booted=1
        break
    fi
    sleep 1
done

if [ "$booted" -eq 0 ]; then
    echo "guest did not boot within ${BOOT_TIMEOUT}s" >&2
    tail -60 "$SERIAL" 2>/dev/null || true
    exit 1
fi

echo "guest booted; running ${SECONDS_AFTER_BOOT}s"
sleep "$SECONDS_AFTER_BOOT"

if [ "$DUMP" -eq 1 ]; then
    echo "dumping ${MEM_BYTES} bytes to $RAMDUMP"
    python3 "$ROOT/scripts/qemu_monitor.py" "$MON" \
        "pmemsave 0 $MEM_BYTES \"$RAMDUMP\"" quit
else
    python3 "$ROOT/scripts/qemu_monitor.py" "$MON" quit || true
fi

wait "$QPID" 2>/dev/null || true
trap - EXIT

echo "serial   : $SERIAL"
if [ "$DUMP" -eq 1 ]; then
    ls -lh "$RAMDUMP"
fi
