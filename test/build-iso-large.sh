#!/bin/sh
# Build a synthetic ~5 GB ISO that exercises the multi-partition
# install path on real hardware (single 4G main + one ~1G sub).
# Same booting hello-world ELF as test.iso, just padded with a big
# zero-filled file so the ISO size pushes past the 4 GB single-
# partition cap.
#
# Run inside the ps2-usbhdl-build container with the project at /src.
set -eu

# Padding size in MB. 5120 MB = 5 GB total disc, which yields:
#   main 4G  -> 4 G - 4 MB header = 4092 MB usable
#   sub  1G  -> ~1023 MB usable
# Plenty of headroom for verification but small enough that the
# stream finishes in a reasonable time on USB 1.1 (~85 min).
PAD_MB=${PAD_MB:-5120}

out=/tmp/iso_root_large
rm -rf "$out"
mkdir -p "$out"

# Boot ELF — same hello-world as the regular test ISO.
mips64r5900el-ps2-elf-gcc \
	-D_EE -G0 -O2 -Wall -gdwarf-2 -gz \
	-I"$PS2SDK"/ee/include -I"$PS2SDK"/common/include \
	-T"$PS2SDK"/ee/startup/linkfile \
	-L"$PS2SDK"/ee/lib \
	-Wl,-zmax-page-size=128 \
	test/main.c -o "$out/BOOT.ELF" -ldebug

cp test/system.cnf "$out/SYSTEM.CNF"

# Filler. dd is the path of least resistance; sparse files would
# defeat the point (genisoimage materializes the bytes regardless).
echo "  generating ${PAD_MB} MB of filler (this takes a moment)..."
dd if=/dev/zero of="$out/PAD.BIN" bs=1M count="$PAD_MB" status=none

mkdir -p /src/dist
echo "  building ISO..."
genisoimage -quiet -iso-level 1 -V USBHDL_LARGE \
	-o /src/dist/test-large.iso "$out"

# Clean up the staging dir so we don't keep ~5 GB of zeros around.
rm -rf "$out"

ls -la /src/dist/test-large.iso
