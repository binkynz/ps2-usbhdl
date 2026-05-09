# ps2-usbhdl

A PlayStation 2 ELF that installs PS2 ISO games from a USB mass-storage
device directly onto the internal HDD in HDLoader format — running
entirely on the console, with no PC required.

## Why this exists

The mature PS2 game-install toolchain (`hdl-dump`, HDL Batch Installer,
PFS BatchKit Manager, WinHIIP) is all PC-side. The one PS2-side
installer, [HDLGameInstaller](https://github.com/ps2homebrew/HDLGameInstaller),
only supports installing from the DVD drive or from a networked PC —
not from a USB mass-storage device.

For users without a working USB-to-SATA adapter and without a network
adapter on their PS2 (e.g. fat consoles with HDD-only Gamestar
expansion adapters), there is no existing path to install games to the
internal HDD. This project fills that gap.

## Hardware target

- PS2 fat (SCPH-3xxxx through 5xxxx) with Sony Network Adapter or a
  compatible third-party adapter (e.g. Gamestar HDD adapter)
- Internal HDD formatted with the APA partition scheme (PS2 standard)
- USB mass-storage device formatted FAT32, containing one or more
  PS2 ISO images

## Status

Pre-alpha. Toolchain setup in progress.

## Building

Build environment is the [`ps2dev/ps2dev`](https://hub.docker.com/r/ps2dev/ps2dev)
Docker image, which provides PS2SDK and the EE/IOP cross-compilers.
Details TBD as the build is wired up.
