# Contributing

ps2-usbhdl is small and the contribution scope is correspondingly
small — bug reports against real hardware, fixes for things the
[README's Limitations
section](README.md#limitations-and-future-work) names, and
incremental polish.

## Setup

You need:

- A modded fat PS2 with an HDD (Sony Network Adapter or compatible)
- A FAT32 USB stick with at least one PS2 ISO at the root
- Linux on the build host
- Docker (the build environment runs inside a `ps2dev/ps2dev`-based
  image; nothing PS2-specific is installed on the host)
- [`just`](https://github.com/casey/just) for the task runner

## Editor / LSP setup

PS2SDK headers live inside the build container, so out-of-the-box
clangd will fail to resolve `<kernel.h>`, `<fileXio_rpc.h>`, and
similar. One-time setup:

```sh
just lsp
```

extracts `$PS2SDK/ee/include` and `$PS2SDK/common/include` from the
container into `.ps2sdk/` on the host. The committed `.clangd`
config picks them up via `-I.ps2sdk/ee/include` /
`-I.ps2sdk/common/include`. `.ps2sdk/` is gitignored. Re-run `just
lsp` if the upstream PS2SDK in the docker image changes.

## Build / test cycle

```sh
just all                          # ELF + test ISO into dist/
just deploy /dev/sdX1             # stage onto USB (sudo prompt once)
sudo umount /mnt/usb              # if your distro auto-mounted
# unplug, plug into PS2, run via uLaunchELF
```

`dist/test.iso` is an 820 KB hello-world wrapper used to validate
the install pipeline end-to-end without committing to a 67-minute
real-DVD stream. Most iterations want this.

For sub-partition (>4 GB ISO) testing without a DVD9 game, use
the synthetic large-ISO recipe (`just test-iso-large`).

## What to expect on real hardware

The PS2's debug screen is 27 lines tall, no scroll — anything that
overflows wraps to the top. The streaming UI clears the screen at
the start of the install for this reason. If you add new output,
keep that wrap behavior in mind.

USB 1.1 throughput is ~1 MB/s. A 4 GB DVD takes ~67 minutes to
stream. Test new code paths against `test.iso` first.

## Formatting

C sources are formatted with `clang-format` per the `.clang-format`
at the root (LLVM defaults). Two ways to keep your local clean:

```sh
just fmt          # format everything in-place
just fmt-check    # CI-style check (also run by GHA on every push)
just install-hooks # symlink hooks/pre-commit into .git/hooks/
```

The pre-commit hook runs `clang-format --dry-run --Werror` against
staged files only and refuses the commit on any unformatted hunk.

## House style for commits

Imperative subjects, body explains *why* not *what*, lines wrap
around 72 columns. See `git log` for the existing pattern. Force
pushes to `main` are fine on this project — there are no other
contributors yet.

## Hardware-validated changes

PRs that touch the install/stream/HDD code paths should include a
photo of the screen showing the relevant output on real hardware.
PCSX2 doesn't reliably emulate USB mass storage, so the CI build
only validates compilation, not behavior.
