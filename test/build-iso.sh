#!/bin/sh
# Compile test/main.c into BOOT.ELF and wrap it with SYSTEM.CNF
# in a minimal ISO9660 image at dist/test.iso.
#
# Run this inside the ps2-usbhdl-build container; it expects the
# project mounted at /src and PS2SDK on PATH.
set -eu

out=/tmp/iso_root
rm -rf "$out"
mkdir -p "$out"

mips64r5900el-ps2-elf-gcc \
	-D_EE -G0 -O2 -Wall -gdwarf-2 -gz \
	-I"$PS2SDK"/ee/include -I"$PS2SDK"/common/include \
	-T"$PS2SDK"/ee/startup/linkfile \
	-L"$PS2SDK"/ee/lib \
	-Wl,-zmax-page-size=128 \
	test/main.c -o "$out/BOOT.ELF" -ldebug

cp test/system.cnf "$out/SYSTEM.CNF"

mkdir -p /src/dist
genisoimage -quiet -iso-level 1 -V USBHDL_TEST \
	-o /src/dist/test.iso "$out"
