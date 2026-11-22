#!/usr/bin/env bash
#
# Build the minimal Linux guest used for snapshot validation:
#   - static /init (guest/init.c)
#   - a kernel with DWARF4 debug info, EFI stub and built-in initramfs
#   - a FAT32 ESP image containing \EFI\BOOT\BOOTX64.EFI
#
# Usage: scripts/build_guest.sh [--jobs N] [--clean]
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KSRC="${KSRC:-$ROOT/guest/linux-6.12.58}"
KBUILD="${KBUILD:-$ROOT/guest/kbuild}"
INIT="$ROOT/guest/init"
INITRAMFS_TXT="$ROOT/guest/initramfs.txt"
ESP="$ROOT/guest/esp.img"
JOBS="$(nproc)"

while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --clean) rm -rf "$KBUILD" "$ESP" "$INIT" "$INITRAMFS_TXT"; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -d "$KSRC" ] || { echo "missing kernel source: $KSRC" >&2; exit 1; }

# The kernel build needs bc(1) for kernel/time/timeconst.bc.  A locally built
# GNU bc lives in tools/bin so the build works without root/package installs.
export PATH="$ROOT/tools/bin:$PATH"
command -v bc >/dev/null || { echo "missing bc (expected $ROOT/tools/bin/bc)" >&2; exit 1; }

echo "== building static init =="
gcc -static -Os -Wall -o "$INIT" "$ROOT/guest/init.c"

echo "== generating initramfs file list =="
cat > "$INITRAMFS_TXT" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/ttyS0 0600 0 0 c 4 64
dir /proc 0555 0 0
dir /sys 0555 0 0
file /init $INIT 0755 0 0
EOF

echo "== configuring kernel =="
make -C "$KSRC" O="$KBUILD" defconfig >/dev/null

CFG="$KSRC/scripts/config --file $KBUILD/.config"
$CFG --enable  DEBUG_INFO
$CFG --enable  DEBUG_INFO_DWARF4
$CFG --disable DEBUG_INFO_DWARF5
$CFG --disable DEBUG_INFO_NONE
$CFG --disable DEBUG_INFO_REDUCED
$CFG --disable DEBUG_INFO_SPLIT
$CFG --disable DEBUG_INFO_BTF
$CFG --disable GDB_SCRIPTS
$CFG --disable RANDOMIZE_BASE
$CFG --disable RANDOMIZE_MEMORY
$CFG --enable  EFI
$CFG --enable  EFI_STUB
$CFG --enable  BLK_DEV_INITRD
$CFG --set-str INITRAMFS_SOURCE "$INITRAMFS_TXT"
$CFG --enable  INITRAMFS_COMPRESSION_NONE
$CFG --disable MODULES
$CFG --enable  SERIAL_8250
$CFG --enable  SERIAL_8250_CONSOLE
$CFG --enable  PRINTK
$CFG --enable  TTY
$CFG --enable  PROC_FS
$CFG --enable  SYSFS
$CFG --enable  DEVTMPFS
$CFG --enable  DEVTMPFS_MOUNT
$CFG --enable  TMPFS
$CFG --enable  BINFMT_ELF
$CFG --enable  BINFMT_SCRIPT
$CFG --enable  ACPI
$CFG --enable  PCI
$CFG --enable  SMP
$CFG --enable  KALLSYMS
$CFG --enable  CMDLINE_BOOL
$CFG --set-str CMDLINE "console=ttyS0 nokaslr"
$CFG --enable  CMDLINE_OVERRIDE

make -C "$KSRC" O="$KBUILD" olddefconfig >/dev/null

echo "== building kernel (-j$JOBS) =="
make -C "$KSRC" O="$KBUILD" -j"$JOBS" bzImage

echo "== building ESP =="
rm -f "$ESP"
dd if=/dev/zero of="$ESP" bs=1M count=64 status=none
mkfs.vfat -F 32 -n SNAPESP "$ESP" >/dev/null
mmd -i "$ESP" ::/EFI ::/EFI/BOOT
mcopy -i "$ESP" "$KBUILD/arch/x86/boot/bzImage" ::/EFI/BOOT/BOOTX64.EFI
mdir -i "$ESP" ::/EFI/BOOT

echo "== done =="
echo "vmlinux: $KBUILD/vmlinux"
echo "ESP    : $ESP"
