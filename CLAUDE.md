# Working on ps2-usbhdl

Project context for Claude Code sessions. Versioned with the repo.

## What this is

A PlayStation 2 ELF that installs PS2 ISO games from a USB
mass-storage device directly to the internal HDD in HDLoader format,
running entirely on the console — no PC required.

## Why it exists

Every existing PS2 game-install tool is PC-side (`hdl-dump`, HDL Batch
Installer, PFS BatchKit, WinHIIP). The one PS2-side installer,
HDLGameInstaller by SP193, only accepts DVD-drive or networked-PC
sources — not USB. Users without a USB-to-SATA adapter and without an
Ethernet-capable PS2 expansion adapter have no path to install games
to the HDD. This tool fills that gap.

## Hardware target

- Fat PS2 (SCPH-3xxxx through 5xxxx), mod-chipped to run unsigned ELFs
- Sony Network Adapter or compatible (specifically tested with
  Gamestar HDD-only adapter — IRX-compatible with Sony's NA)
- Internal HDD formatted with PS2's APA partition scheme
- USB mass-storage device, FAT32, holding PS2 ISO images under
  `/CD/` and `/DVD/` (matching the OPL convention)

## Build environment

No native PS2SDK on the dev machine — cross-compile via the
[`ps2dev/ps2dev`](https://hub.docker.com/r/ps2dev/ps2dev) Docker
image, which ships PS2SDK + EE/IOP toolchains. The project's
`Dockerfile` layers in `make`, `bash`, GCC's gmp/mpfr/mpc deps,
and `cdrkit` (for genisoimage) on top.

Host-side task running goes through `just`. Recipes:

- `just build` — compile installer ELF to `dist/ps2-usbhdl.elf`
- `just test-iso` — build a tiny hello-world test ISO at
  `dist/test.iso` for fast streaming-pipeline iteration
- `just all` — both
- `just deploy <USB-mount-dir>` — copy outputs onto a mounted
  USB stick (mounts/unmounts via sudo as needed)
- `just clean` — remove `build/` and `dist/`

Inside the container, `make` (driven by the project `Makefile`)
does the actual compilation.

## Test workflow

1. `just all` — builds ELF + test ISO into `dist/`.
2. `just deploy /run/media/$USER/<stick>` — stages everything
   onto USB including the wet-run sentinel.
3. Plug stick into PS2, run `ps2-usbhdl.elf` via uLaunchELF.

For fast iteration on streaming/install code, use `dist/test.iso`
(a ~820 KB hello-world wrapped in ISO9660). At USB 1.1 it streams
in under a second; a real PS2 DVD ISO takes ~67 minutes.

No emulator path — PCSX2's HDD/IRX behavior diverges enough from real
hardware that bugs only surface on the console.

## Safety rules for HDD writes

The PS2's APA partition table is fragile. A buggy partition-create
path can corrupt the table and wipe every installed game on the disk.

**Always:**

1. Implement a dry-run mode first that logs every byte that *would*
   be written and the exact LBA range it would target, without
   touching the disk.
2. Cross-check dry-run output bit-for-bit against `hdl-dump`'s output
   for the same ISO (run `hdl-dump` on a desktop against a dump of
   the same partition).
3. Before any wet-run, dump the existing APA table to a file on USB
   so it can be restored manually if something goes wrong.
4. Refuse to overwrite or extend an existing partition. New
   partitions only, with a name that doesn't collide.

## Relevant external sources

- [`hdl-dump` source](https://github.com/ps2homebrew/hdl-dump) —
  reference implementation of the on-disk HDL format
- [`HDLGameInstaller`](https://github.com/ps2homebrew/HDLGameInstaller) —
  PS2-side install over network/DVD; useful as IRX-loading reference
- [`Open-PS2-Loader`](https://github.com/ps2homebrew/Open-PS2-Loader) —
  reference for the consumer side (how installed games get launched)
- [PS2SDK](https://github.com/ps2dev/ps2sdk) — IRX modules and EE/IOP
  APIs used by this project
