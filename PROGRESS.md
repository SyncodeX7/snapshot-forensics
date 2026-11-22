# Progress log

Development history for the SMM snapshot forensics prototype. Dates are the
session dates on this machine (2026-11-20 → 2026-11-22). Raw command output
lives in `logs/`; final measured results are in `README.md`.

## Status: complete

- EDK II `SnapshotPkg` builds and runs under QEMU Q35 + OVMF with `SMM_REQUIRE=TRUE`.
- Modes A/B/C/D all capture a full 109,016-page (425.8 MiB) image with
  100% range coverage and zero page-hash mismatches.
- Volatility 3 `linux.pslist` resolves `init` and `snapw0`–`snapw3` from every
  extracted image.
- GNOME Boxes / libvirt support verified; the VM appears in Boxes as
  "SMM Snapshot Forensics".
- Web GUI implemented and verified for all tabs and the Analyze action.

## Timeline

### 2026-11-20 — environment and EDK II

- Verified host toolchain: gcc, nasm, QEMU 11.1.1, Python 3.14, clang,
  bison/flex, Go, pahole, `mkfs.vfat`/`mcopy`, `/dev/kvm`.
- Cloned EDK II, initialized submodules, built BaseTools.
- Built `iasl` locally from ACPICA `R2025_04_04`; no root installs.
- Found EDK II only ships the `GCC` toolchain (no GCC5); switched builds to `-t GCC`.
- Created `SnapshotPkg` with `SnapshotDxe`, `SmmSnapshotDriver`, and
  `SmmPeriodicTimerDxe`.
- Wrote `scripts/integrate_edk2.py` to inject PCDs, components, and the FDF
  DXEFV entry idempotently.

### 2026-11-21 — guest, ABI, first boot

- Built a minimal Linux 6.12.58 guest with DWARF4, built-in initramfs, and
  `console=ttyS0 nokaslr`.
- Built `dwarf2json` and created the Volatility ISF (`out/isf/linux-6.12.58.json`).
- Verified the packed ABI: header 2568 bytes, slot entry 72, slot header 120,
  page entry 16, memory range 24, config 1608.
- Fixed the FNV-1a seed to `14695981039346656037ULL`.
- First successful boot: config accepted, snapshots captured.

### 2026-11-22 — modes, benchmark, Boxes, GUI

- Mode A validation: 5 ticks, 109,016 pages, 100% coverage, 0 mismatches;
  Volatility PIDs matched the guest serial.
- Added `SmmPeriodicTimerDxe` because OVMF ships no periodic SMI producer;
  QEMU ICH9 supports it (`GEN_PMCON_1`, `SMI_EN` bit 14, `SMI_STS` W1C).
- Set `PcdCpuSmmRestrictedMemoryAccess=FALSE` so SMM can read guest RAM and the
  reserved buffer (deliberate prototype tradeoff).
- Capture conventional plus `EfiLoaderCode`/`EfiLoaderData`; exclude MMIO from
  `GuestRamTop` so tracking arrays fit in TSEG.
- Config is sent from a protocol notify on `gEfiDxeSmmReadyToLockProtocolGuid`.
- **Boxes support:** stock Boxes VMs do not enable SMM, so `scripts/boxes.sh`
  defines a libvirt *session* domain (`qemu+unix:///session`) with `q35`,
  `<smm state='on'/>`, and secure pflash. Verified by running Boxes headless
  and screenshotting the VM list (`docs/boxes.png`).
- Built the dependency-free web GUI (`gui/server.py` + vanilla JS) with tabs
  Overview, Snapshots, Memory map, Heatmap, Hex, Search, Logs, Volatility.
- Wrote `scripts/analyze.sh`, `scripts/benchmark.sh`, `scripts/bench_report.py`.
- **Boxes display fix:** GNOME Boxes 50 implements SPICE only, so the
  VNC graphics device produced `unsupported display of type vnc`. `boxes.sh`
  now emits `<graphics type='spice'>`; verified by launching Boxes with
  `--open-uuid` and screenshotting the connected display (`docs/boxes.png`).
- **Mode D bug:** after completing a sliced snapshot it immediately started
  another, overwriting the only full slot; a dump taken mid-rebuild had no
  complete image. Fixed to keep the completed image and idle. Re-ran: one
  complete snapshot, 24.3 ms worst-case SMI.
- **GUI Analyze bug:** the POST handler passed the URL-encoded run id to
  `analyze.sh`, writing to `out/analyzed/bench%2Frun-A`. Fixed with
  `urllib.parse.unquote`; Analyze verified end-to-end (job `done`, Volatility
  output visible immediately).
- Final benchmark (`logs/benchmark-final.log`, `logs/benchmark-D.log`) and a
  Boxes end-to-end run (`logs/boxes-final.log`).

## Benchmark summary

QEMU 11.1.1/KVM, Q35 + OVMF DEBUG, 1 GiB guest, 512 MiB buffer, 8 s period.

| Mode | Ticks | Slots | Pages | Copied | SMI total | SMI max | Overhead | Hash bad | Coverage |
|------|-------|-------|-------|--------|-----------|---------|----------|----------|----------|
| A full | 12 | 1 | 109,016 | 5,110 MiB | 1,203 ms | 294.2 ms | 1.253% | 0 | 100% |
| B hash | 12 | 5 | 109,016 | 904 MiB | 244.6 ms | 97.9 ms | 0.255% | 0 | 100% |
| C dirty | 12 | 5 | 109,016 | 904 MiB | 218.9 ms | 103.6 ms | 0.228% | 0 | 100% |
| D sliced | 21 | 1 | 109,016 | 425.8 MiB | 188.6 ms | 24.3 ms | 0.112% | 0 | 100% |

## Log map

| File | Contents |
|------|----------|
| `logs/benchmark-final.log` | Live progress log of the A/B/C benchmark; mode D failed here and led to the mode-D fix |
| `logs/benchmark-D.log` | Successful re-run of mode D after the fix |
| `logs/bench-build-{A,B,C,D}.log` | EDK II builds per mode |
| `logs/run-modeA.log` | First full mode-A QEMU run |
| `logs/boxes-final.log` | Boxes/libvirt end-to-end run |
| `logs/analyze-boxes.log` | Extraction + Volatility for the Boxes run |
| `logs/boot-test.log` | Early boot/config smoke test |
| `logs/build-edk2.log`, `logs/build-guest.log` | Full EDK II and guest-kernel builds |
| `logs/iasl-build.log`, `logs/edk2-clone.log`, `logs/submodules.log` | Toolchain setup |
| `logs/gui.log` | GUI server startup |
| `out/analyzed/<rid>/analyze.log` | Per-run GUI Analyze job log |

## Reproduce

```bash
scripts/setup_edk2.sh
scripts/build_guest.sh
scripts/integrate_edk2.py --mode 0 --period 8
scripts/benchmark.sh --modes A,B,C,D --seconds 100
scripts/boxes.sh run --seconds 20
scripts/start_gui.sh
```

## Known limitations

- Virtualized latency numbers only; no bare-metal measurements.
- `PcdCpuSmmRestrictedMemoryAccess=FALSE` is required for this prototype and
  is not production-safe.
- Mode D acquires one image per boot by design (single full slot).
- Mode C is best-effort: it never clears the OS dirty bit, so a tick can miss
  pages the OS later cleans without rewriting.
- Volatility symbol resolution depends on the exact `vmlinux` used to build
  the ISF.
