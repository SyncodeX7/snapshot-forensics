#!/usr/bin/env bash
#
# Reproducible setup of the EDK II side of the snapshot-forensics prototype.
#
#   scripts/setup_edk2.sh [--skip-clone] [--mode N] [--jobs N]
#
# Steps:
#   1. shallow-clone edk2 (if needed) and check out the submodules the OVMF
#      build touches,
#   2. build iasl from ACPICA if it is not on PATH,
#   3. build BaseTools,
#   4. wire SnapshotPkg into OvmfPkgX64.dsc/.fdf,
#   5. build OVMF with -D SMM_REQUIRE=TRUE.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDK2="$ROOT/edk2"
TOOLS="$ROOT/tools"
ACPICA="$TOOLS/acpica-R2025_04_04"
IASL="$ACPICA/generate/unix/bin/iasl"
JOBS="$(nproc)"
MODE=0
SKIP_CLONE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-clone) SKIP_CLONE=1; shift ;;
        --mode) MODE="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [ "$SKIP_CLONE" -eq 0 ] && [ ! -d "$EDK2/.git" ]; then
    echo "== cloning edk2 =="
    git clone --depth 1 https://github.com/tianocore/edk2.git "$EDK2"
fi

if [ ! -d "$EDK2/.git" ]; then
    echo "missing $EDK2 (use --skip-clone only if it is already there)" >&2
    exit 1
fi

echo "== submodules =="
git -C "$EDK2" submodule update --init --depth 1 --jobs 4 || true
# Shallow submodule clones occasionally leave an empty work tree; force it.
for p in $(git -C "$EDK2" config --file .gitmodules --get-regexp path | awk '{print $2}'); do
    if [ -d "$EDK2/$p" ] && [ -z "$(ls -A "$EDK2/$p" 2>/dev/null | grep -v '^\.git$')" ]; then
        git -C "$EDK2/$p" checkout -f HEAD >/dev/null 2>&1 || true
    fi
done

if [ ! -x "$IASL" ]; then
    echo "== building iasl =="
    mkdir -p "$TOOLS"
    if [ ! -d "$ACPICA" ]; then
        curl -sSL --retry 5 -o "$TOOLS/acpica.tar.gz" \
            https://github.com/acpica/acpica/archive/refs/tags/R2025_04_04.tar.gz
        tar -xzf "$TOOLS/acpica.tar.gz" -C "$TOOLS"
    fi
    make -C "$ACPICA/generate/unix" -j"$JOBS" iasl
fi

echo "== BaseTools =="
export WORKSPACE="$EDK2"
export PACKAGES_PATH="$EDK2:$ROOT"
export EDK_TOOLS_PATH="$EDK2/BaseTools"
export PATH="$ACPICA/generate/unix/bin:$PATH"
# shellcheck disable=SC1091
source "$EDK2/edksetup.sh" BaseTools
make -C "$EDK2/BaseTools" -j"$JOBS" >/dev/null

echo "== integrating SnapshotPkg =="
python3 "$ROOT/scripts/integrate_edk2.py" --edk2 "$EDK2" --mode "$MODE"

echo "== building OVMF =="
build -a X64 -t GCC -p OvmfPkg/OvmfPkgX64.dsc -D SMM_REQUIRE=TRUE -b DEBUG -n "$JOBS"

echo "== done =="
ls -lh "$EDK2/Build/OvmfX64/DEBUG_GCC/FV/OVMF_CODE.fd" \
       "$EDK2/Build/OvmfX64/DEBUG_GCC/FV/OVMF_VARS.fd"
