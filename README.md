# ps2-usbhdl

[![build](https://github.com/binkynz/ps2-usbhdl/actions/workflows/build.yml/badge.svg)](https://github.com/binkynz/ps2-usbhdl/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/binkynz/ps2-usbhdl?include_prereleases&label=release)](https://github.com/binkynz/ps2-usbhdl/releases)

A PlayStation 2 ELF that installs PS2 ISO games from a USB
mass-storage device directly onto the internal HDD in HDLoader
format — running entirely on the console, with no PC required.

## Why this exists

The mature PS2 game-install toolchain (`hdl-dump`, HDL Batch
Installer, PFS BatchKit Manager, WinHIIP) is all PC-side. The one
PS2-side installer,
[HDLGameInstaller](https://github.com/ps2homebrew/HDLGameInstaller),
only supports installing from the DVD drive or from a networked PC
— not from USB.

For users without a working USB-to-SATA adapter and without a
network-capable PS2 expansion adapter (e.g. fat consoles with
HDD-only Gamestar adapters), there is no existing path to install
games onto the internal HDD. `ps2-usbhdl` fills that gap.

## Status

**Alpha.** End-to-end install + boot validated on real hardware
(fat PS2 + Gamestar HDD adapter), against both a hand-built test
ISO and a real game — Ratchet & Clank (`SCUS_971.99`, NTSC-US,
~4 GB single-layer DVD). The full pipeline (USB read →
ISO9660 / SYSTEM.CNF parse → APA partition create + HDL header
format → ~67 min stream → OPL launch into title screen) is
confirmed working.

What works:

- Loads the standard IOP module stack plus HDLGameInstaller's
  HDL-aware IRXes (`ps2hdd-hdl.irx`, `hdlfs.irx`).
- Reads PS2 ISOs from FAT32 USB sticks, including split-file
  sets (`<name>.iso.001` / `.002` / ...) for ISOs above the
  FAT32 4 GB single-file limit.
- Parses ISO9660 PVD (volume id, size) and walks the disc to
  extract the canonical startup id from `SYSTEM.CNF`.
- Plans the install: APA partition name (`PP.HDL.<startup>`),
  main + sub bucket sizing, and HDL `FormatArgs`.
- Controller-driven UI: top-level mode picker (install / manage
  / exit), D-pad navigable multi-select for ISOs and partitions,
  10-second abort countdown before any destructive op.
- Install mode: creates and formats an HDL partition with
  overwrite-on-conflict, chains sub-partitions for >4 GB games,
  streams the ISO with a visual progress bar (throughput,
  elapsed, ETA).
- Manage mode: lists existing HDL partitions on the HDD with
  sizes; multi-select + delete with the same 10-second abort
  window.
- Batch install: pick N ISOs in one run; per-game success /
  failure summary at the end.

## Hardware target

- Fat PS2 (SCPH-3xxxx through 5xxxx), mod-chipped to run unsigned
  ELFs, with a Sony Network Adapter or a compatible third-party
  adapter (notably the HDD-only Gamestar variants — those have no
  Ethernet, which is the case this tool was written for).
- Internal HDD formatted with the APA partition scheme. Tested on
  a 1 TB SATA-via-IDE-bridge drive.
- USB mass-storage device formatted FAT32, containing one or more
  PS2 ISO images at the root.

OPL (Open PS2 Loader) installed on the HDD is recommended for
launching the resulting partitions; HDD-OSD also works.

## Quick start (using the installer)

1. Build the installer ELF and a tiny test ISO:

   ```sh
   just all
   ```

2. Stage everything on a USB stick:

   ```sh
   just deploy /dev/sdb       # block device path or mount dir
   ```

   This copies `dist/ps2-usbhdl.elf` and any built test ISOs.
   `just usb-list` shows candidate device paths.

3. Unmount, plug into the PS2, run `ps2-usbhdl.elf` via uLaunchELF.

4. Pick a mode at the top-level menu — **X** to install games
   from USB, **Square** to manage existing installs (list +
   delete), **Triangle** to exit.

5. In install mode: pick ISOs with the D-pad, **Square** to
   toggle, **X** to start the batch (auto-selects current row
   if nothing toggled). The 10-second WET RUN countdown is your
   last chance to power-cycle if anything looks wrong; after
   that each partition is created, formatted, and streamed over
   USB 1.1 (~1 MB/s; ~67 minutes for a 4 GB ISO).

6. Boot OPL — installed titles appear in the HDD games list.

## Building from source

Build environment is the [`ps2dev/ps2dev`](https://hub.docker.com/r/ps2dev/ps2dev)
Docker image with `make`, `bash`, GCC's gmp/mpfr/mpc deps, and
`cdrkit` layered on top (see `Dockerfile`). Host-side task running
goes through [`just`](https://github.com/casey/just):

```text
just            # list recipes
just build      # compile dist/ps2-usbhdl.elf
just test-iso   # build dist/test.iso (~820 KB hello-world)
just all        # both
just deploy DEV # stage build outputs onto USB (mounts/unmounts as needed)
just usb-list   # find candidate USB block devices
just clean      # remove build/ and dist/
```

The Docker image is built and cached on first run; subsequent
builds are fast. No native PS2SDK install required on the host.

## Project layout

```
ps2-usbhdl/
├── CLAUDE.md          # working context for Claude Code sessions
├── Dockerfile         # ps2dev/ps2dev + make + cdrkit
├── Makefile           # PS2SDK build rules (runs inside container)
├── README.md
├── justfile           # host-side task runner
├── src/
│   └── main.c         # the installer
├── test/
│   ├── main.c         # hello-world ELF that goes inside test.iso
│   ├── system.cnf     # static SYSTEM.CNF for test.iso
│   └── build-iso.sh   # called by `just test-iso`
└── vendor/
    └── irx/
        ├── ps2hdd_hdl.irx   # HDL-aware fork of ps2hdd.irx
        ├── hdlfs.irx        # HDL filesystem driver (hdl0:)
        └── README.md        # provenance, GPLv2 note
```

The two vendored IRX modules come from
[HDLGameInstaller](https://github.com/ps2homebrew/HDLGameInstaller);
see `vendor/irx/README.md` for provenance and the license note.

## How it works

The installer brings up the standard PS2 IOP module stack
(iomanX/fileXio/poweroff/ps2dev9/ps2atad), plus HDLGameInstaller's
forked `ps2hdd-hdl.irx` instead of the standard `ps2hdd.irx`,
plus `hdlfs.irx`. After SBV patches enable `LoadModuleBuffer`,
modules load from RAM and the install flow is:

1. Mount USB via `bdmfs_fatfs` → list `*.iso` files.
2. Read ISO9660 PVD from sector 16 → volume id, size in 2 KB
   sectors.
3. Walk the root directory to find `SYSTEM.CNF` → extract
   startup id (e.g. `SCUS_971.99`).
4. Pick an APA size bucket (`128M` / `256M` / `512M` / `1G` /
   `2G` / `4G`) for the main partition; spill into sub-partitions
   if the ISO + 4 MB header reserve exceeds 4 GB.
5. `fileXioOpen("hdd0:<name>,,,<size>,HDL", O_WRONLY | O_CREAT |
   O_TRUNC, 0644)` creates the partition.
6. Build a 232-byte `HDLFS_FormatArgs` (CompatFlags, DiscType,
   NumSectors, Layer1Start, GameTitle, StartupPath) and call
   `fileXioFormat("hdl0:", "hdd0:<name>", &args, sizeof(args))`
   to write the HDL header zone.
7. `fileXioMount("hdl0:", "hdd0:<name>", FIO_MT_RDWR)` exposes
   the partition's data area; `fileXioOpen("hdl0:", O_RDWR)`
   opens it for sequential I/O at offset 0 = start of game data.
8. Stream the ISO in 1 MB chunks USB → `hdl0:`. `hdlfs.irx`
   routes writes across the main + sub-partitions transparently.

## Limitations and future work

The path that's been validated covers single-layer DVDs up to
~4 GB on a single APA partition. Beyond that, several things are
known-incomplete:

- **Sub-partitions for >4 GB games.** Implemented but
  unverified on real hardware — the only large ISO this session
  exercised was 4 GB (single-partition path). `execute_install`
  chains `HIOCADDSUB` ioctls for each planned sub before format.
  Exercising the multi-partition path needs a DVD9 / >4 GB ISO
  and a real install run.
- **DVD9 layer-break detection.** No automatic detection from
  the ISO file (the layer break lives in physical-layer
  descriptors most rip tools strip). Sidecar override accepted:
  `mass:/<iso>.iso.layer_break` containing the layer-1 start
  sector as a decimal integer. ISOs over 4.5 GB without the
  sidecar trigger a `WARNING` line in the install plan; install
  still proceeds with `Layer1Start = 0`, which works for
  single-layer discs misidentified by size and breaks for true
  DVD9.
- **Non-FAT32 USB.** `bdmfs_fatfs` is the only filesystem driver
  embedded; exFAT/NTFS sticks won't enumerate. For ISOs over the
  FAT32 4 GB single-file limit, use the split-file workflow:

  ```sh
  just split-iso ~/Downloads/big-game.iso
  # produces big-game.iso.001, big-game.iso.002, ...
  cp ~/Downloads/big-game.iso.* /mnt/usb/
  ```

  The installer detects the `.001` first-part suffix and streams
  across the parts as one logical ISO.
- **Batch install.** Currently one ISO per run. Multi-ISO mode
  with a "stage all, install all" flow is plausible but unwritten.
- **Resumable installs.** If streaming fails partway through,
  the partition is left in a half-installed state; current
  recovery is to rerun and let the overwrite-on-conflict path
  delete + recreate.
- **PCSX2 in-emulator testing.** Possible but blocked on
  PCSX2's spotty USB mass-storage support; would require
  refactoring the ISO source path (`cdrom0:` or embedded blob).

## Credits

- [HDLGameInstaller](https://github.com/ps2homebrew/HDLGameInstaller)
  by SP193 — the source of the vendored IRXes and the canonical
  reference implementation of the PS2-side HDL install protocol.
- [hdl-dump](https://github.com/ps2homebrew/hdl-dump) — reference
  for the HDL on-disk format and the APA bucket sizing rules.
- [PS2SDK](https://github.com/ps2dev/ps2sdk) — the IRX modules
  and EE/IOP toolchains.
- [Open PS2 Loader](https://github.com/ps2homebrew/Open-PS2-Loader)
  — the launcher for the resulting HDL partitions.

## License

GPLv2 by inheritance from the vendored HDLGameInstaller IRX
modules.
