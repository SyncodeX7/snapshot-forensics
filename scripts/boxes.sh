#!/usr/bin/env bash
#
# Manage the prototype as a libvirt "box" (GNOME Boxes / virt-manager).
#
# Boxes talks to qemu:///session, so this script defines a session domain with
# q35 + SMM + secure pflash.  Plain Boxes VMs do not enable SMM, which the
# SMM_REQUIRE OVMF build needs, so this is the supported way to run the
# prototype inside Boxes.
#
# Usage: scripts/boxes.sh COMMAND [options]
#
#   define     write the domain XML and register it with libvirt
#   start      define if needed, then start the domain
#   stop       destroy the running domain
#   restart    stop + start
#   status     show domain state and file locations
#   list       list session domains (what Boxes shows)
#   capture    dump guest RAM to <out>/ram.raw
#   analyze    extract + Volatility for the newest dump
#   run        start, wait for the guest, capture, then analyze
#   gui        start the local web GUI
#   open       launch GNOME Boxes
#
# Options:
#   --name NAME        libvirt domain name (default: snapshot-forensics)
#   --out DIR          output directory (default: out/boxes, or out/boxes-NAME)
#   --mem MiB          guest RAM (default: 1024)
#   --vcpus N          vCPUs (default: 2)
#   --seconds N        seconds to run after boot before capture (default: 24)
#   --timeout N        boot timeout in seconds (default: 240)
#   --uri URI          libvirt URI (default: qemu:///session)
#   --keep             with run: reuse an already running domain
#   --no-analyze       with run: skip extraction/Volatility
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ROOT="$ROOT/out"
NAME="snapshot-forensics"
OUTDIR=""
MEM_MIB=1024
VCPUS=2
SECONDS_AFTER_BOOT=24
BOOT_TIMEOUT=240
URI="${LIBVIRT_URI:-qemu:///session}"
KEEP=0
DO_ANALYZE=1
GUI_PORT=8787

COMMAND="${1:-help}"
if [ $# -gt 0 ]; then shift; fi

while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2 ;;
        --out) OUTDIR="$2"; shift 2 ;;
        --mem) MEM_MIB="$2"; shift 2 ;;
        --vcpus) VCPUS="$2"; shift 2 ;;
        --seconds) SECONDS_AFTER_BOOT="$2"; shift 2 ;;
        --timeout) BOOT_TIMEOUT="$2"; shift 2 ;;
        --uri) URI="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --no-analyze) DO_ANALYZE=0; shift ;;
        --port) GUI_PORT="$2"; shift 2 ;;
        -h|--help) COMMAND=help; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$OUTDIR" ]; then
    if [ "$NAME" = "snapshot-forensics" ]; then
        OUTDIR="$OUT_ROOT/boxes"
    else
        OUTDIR="$OUT_ROOT/boxes-$NAME"
    fi
fi
OUTDIR="$(mkdir -p "$OUTDIR" && cd "$OUTDIR" && pwd)"

CODE="$ROOT/edk2/Build/OvmfX64/DEBUG_GCC/FV/OVMF_CODE.fd"
VARS_TEMPLATE="$ROOT/edk2/Build/OvmfX64/DEBUG_GCC/FV/OVMF_VARS.fd"
ESP="$ROOT/guest/esp.img"
NVRAM="$OUTDIR/OVMF_VARS.fd"
SERIAL="$OUTDIR/serial.log"
RAMDUMP="$OUTDIR/ram.raw"
XML="$OUTDIR/domain.xml"
MEM_BYTES=$((MEM_MIB * 1024 * 1024))

virsh_() { virsh -c "$URI" "$@"; }

domain_exists() { virsh_ dominfo "$NAME" >/dev/null 2>&1; }
domain_running() { [ "$(virsh_ domstate "$NAME" 2>/dev/null || true)" = "running" ]; }

write_xml() {
    [ -f "$CODE" ] || { echo "missing OVMF code: $CODE (run scripts/setup_edk2.sh)" >&2; exit 1; }
    [ -f "$VARS_TEMPLATE" ] || { echo "missing OVMF vars: $VARS_TEMPLATE" >&2; exit 1; }
    [ -f "$ESP" ] || { echo "missing ESP image: $ESP (run scripts/build_guest.sh)" >&2; exit 1; }
    [ -f "$NVRAM" ] || cp "$VARS_TEMPLATE" "$NVRAM"

    local uuid=""
    if domain_exists; then
        uuid="$(virsh_ domuuid "$NAME" 2>/dev/null || true)"
    fi

    cat > "$XML" <<XML
<domain type='kvm'>
  <name>$NAME</name>
  ${uuid:+<uuid>$uuid</uuid>}
  <title>SMM Snapshot Forensics</title>
  <description>OVMF (SMM) + minimal Linux guest with the SnapshotPkg DXE/SMM drivers.</description>
  <memory unit='MiB'>$MEM_MIB</memory>
  <vcpu placement='static'>$VCPUS</vcpu>
  <os>
    <type arch='x86_64' machine='q35'>hvm</type>
    <loader readonly='yes' type='pflash' secure='yes'>$CODE</loader>
    <nvram template='$VARS_TEMPLATE'>$NVRAM</nvram>
    <boot dev='hd'/>
  </os>
  <features>
    <acpi/>
    <apic/>
    <smm state='on'/>
  </features>
  <cpu mode='host-passthrough' check='none'/>
  <clock offset='utc'/>
  <on_poweroff>destroy</on_poweroff>
  <on_reboot>restart</on_reboot>
  <on_crash>destroy</on_crash>
  <devices>
    <disk type='file' device='disk'>
      <driver name='qemu' type='raw'/>
      <source file='$ESP'/>
      <target dev='vda' bus='virtio'/>
    </disk>
    <serial type='file'>
      <source path='$SERIAL'/>
      <target port='0'/>
    </serial>
    <graphics type='spice' autoport='yes' listen='127.0.0.1'>
      <listen type='address' address='127.0.0.1'/>
    </graphics>
    <video>
      <model type='vga'/>
    </video>
    <memballoon model='none'/>
  </devices>
</domain>
XML

    virsh_ define "$XML"
}

ensure_defined() {
    if ! domain_exists; then
        write_xml
    fi
}

start_domain() {
    ensure_defined
    if domain_running; then
        echo "domain $NAME is already running"
        return 0
    fi
    rm -f "$SERIAL"
    virsh_ start "$NAME"
    echo "serial: $SERIAL"
}

wait_booted() {
    local deadline=$((SECONDS + BOOT_TIMEOUT))
    while [ $SECONDS -lt $deadline ]; do
        if grep -q "SNAPSHOT-GUEST: booted" "$SERIAL" 2>/dev/null; then
            return 0
        fi
        if ! domain_running; then
            echo "domain $NAME stopped before the guest booted" >&2
            tail -40 "$SERIAL" 2>/dev/null || true
            return 1
        fi
        sleep 1
    done
    echo "guest did not boot within ${BOOT_TIMEOUT}s" >&2
    tail -60 "$SERIAL" 2>/dev/null || true
    return 1
}

capture() {
    if ! domain_running; then
        echo "domain $NAME is not running; use: $0 start" >&2
        exit 1
    fi
    echo "dumping ${MEM_BYTES} bytes to $RAMDUMP"
    rm -f "$RAMDUMP"
    virsh_ qemu-monitor-command "$NAME" --hmp \
        "pmemsave 0 $MEM_BYTES \"$RAMDUMP\"" >/dev/null
    [ -f "$RAMDUMP" ] || { echo "pmemsave did not produce $RAMDUMP" >&2; exit 1; }
    local got
    got=$(stat -c %s "$RAMDUMP")
    if [ "$got" -ne "$MEM_BYTES" ]; then
        echo "short dump: got $got bytes, wanted $MEM_BYTES" >&2
        exit 1
    fi
    ls -lh "$RAMDUMP"
}

do_analyze() {
    "$ROOT/scripts/analyze.sh" "$OUTDIR"
}

case "$COMMAND" in
    define)
        write_xml
        ;;
    start)
        start_domain
        ;;
    stop)
        if domain_running; then
            virsh_ destroy "$NAME"
        else
            echo "domain $NAME is not running"
        fi
        ;;
    restart)
        if domain_running; then virsh_ destroy "$NAME"; fi
        start_domain
        ;;
    status)
        if domain_exists; then
            virsh_ dominfo "$NAME"
            echo "output : $OUTDIR"
            echo "serial : $SERIAL"
            [ -f "$RAMDUMP" ] && ls -lh "$RAMDUMP"
        else
            echo "domain $NAME is not defined (run: $0 define)"
        fi
        ;;
    list)
        virsh_ list --all
        echo
        echo "GNOME Boxes uses $URI, so these domains appear there."
        ;;
    capture)
        capture
        ;;
    analyze)
        do_analyze
        ;;
    run)
        if [ "$KEEP" -eq 0 ] && domain_running; then
            virsh_ destroy "$NAME"
        fi
        start_domain
        wait_booted
        echo "guest booted; running ${SECONDS_AFTER_BOOT}s"
        sleep "$SECONDS_AFTER_BOOT"
        capture
        if [ "$DO_ANALYZE" -eq 1 ]; then
            do_analyze
        fi
        ;;
    gui)
        exec python3 "$ROOT/gui/server.py" --port "$GUI_PORT"
        ;;
    open)
        if command -v gnome-boxes >/dev/null 2>&1; then
            nohup gnome-boxes >/dev/null 2>&1 &
            echo "launched GNOME Boxes; look for '$NAME'"
        else
            echo "gnome-boxes not installed; try virt-manager -c $URI" >&2
            exit 1
        fi
        ;;
    help|*)
        sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        ;;
esac
