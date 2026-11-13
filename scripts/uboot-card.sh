#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Make the SD card of the U-Boot test (tests/uboot): U-Boot for the
# Raspberry Pi 3, as its rpi_3_defconfig sets it up, beside the
# Raspberry Pi firmware files, as on a real card.
#
# Usage: scripts/uboot-card.sh [DIR]
#
# DIR, ~/.cache/siliconmirror-uboot by default, gets:
#   src-VERSION/       U-Boot's source, from its Git repository
#   build-VERSION/     U-Boot's build, with u-boot.bin
#   firmware-TAG/      the firmware files, checked against their SHA-256
#   card.img           the card: an MBR, and a FAT32 partition at 4 MiB
# The source, the build and the firmware are kept and used again; the
# card is made anew each time.
#
# Needs git, curl, sha256sum, make, a cross compiler (CROSS_COMPILE,
# aarch64-linux-gnu- by default), bison and flex to configure U-Boot,
# and sfdisk, mkfs.fat and mtools to make the card.

set -eu

# U-Boot 2026.10 does not start on a Pi 3: its board code writes a
# variable in BSS before relocation (commit d19d9e1c06), which corrupts
# the relocation entries there.
UBOOT_VERSION=v2026.07
UBOOT_REPO=https://github.com/u-boot/u-boot.git
FIRMWARE_TAG=1.20260915
FIRMWARE_URL=https://raw.githubusercontent.com/raspberrypi/firmware/$FIRMWARE_TAG/boot

DIR=${1:-$HOME/.cache/siliconmirror-uboot}
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
SRC=$DIR/src-${UBOOT_VERSION#v}
BUILD=$DIR/build-${UBOOT_VERSION#v}
FIRMWARE=$DIR/firmware-$FIRMWARE_TAG

need() {
	for tool in "$@"; do
		if ! command -v "$tool" > /dev/null; then
			echo "uboot-card: $tool is missing" >&2
			exit 1
		fi
	done
}

mkdir -p "$DIR"

# U-Boot.
if [ ! -f "$BUILD/u-boot.bin" ]; then
	need git make bison flex "${CROSS_COMPILE}gcc"
	if [ ! -d "$SRC" ]; then
		git clone --quiet --depth 1 --branch "$UBOOT_VERSION" "$UBOOT_REPO" \
			"$SRC"
	fi
	make -s -C "$SRC" O="$BUILD" CROSS_COMPILE="$CROSS_COMPILE" \
		rpi_3_defconfig
	# Host tools that need the OpenSSL or GnuTLS headers, which a
	# Pi's U-Boot does not use.
	"$SRC/scripts/config" --file "$BUILD/.config" -d TOOLS_KWBIMAGE \
		-d TOOLS_LIBCRYPTO -d TOOLS_MKEFICAPSULE -d TOOLS_MKFWUMDATA
	make -s -C "$SRC" O="$BUILD" CROSS_COMPILE="$CROSS_COMPILE" \
		olddefconfig
	make -s -C "$SRC" O="$BUILD" CROSS_COMPILE="$CROSS_COMPILE" \
		-j "$(nproc)"
fi

# The firmware: what the card of a Pi 3 holds. smirror does not run
# bootcode.bin and start.elf, but the board's boot ROM needs them there.
need curl sha256sum
mkdir -p "$FIRMWARE"
while read -r sum file; do
	if ! echo "$sum  $FIRMWARE/$file" | sha256sum -c --status > /dev/null 2>&1
	then
		curl -fsSL -o "$FIRMWARE/$file" "$FIRMWARE_URL/$file"
		echo "$sum  $FIRMWARE/$file" | sha256sum -c --quiet
	fi
done << EOF
4245ec23b58158dc3e55f3e065eb4ecc0c0705d76c8db506e9f824941feee32e bootcode.bin
32f990801e7765bae6fe5cb940ba5eeff02dd0dac8dc5474db5b3fd345281790 start.elf
7853f2b51e4489f8b3ce6f9c52fd0afdda38c7383961524e0883d34afcb4827b fixup.dat
cd922193672e391b6194d6d94e34ffab6fc154205375fcfb362cad191f7f8aae bcm2710-rpi-3-b.dtb
c7283ff51f863d93a275c66e3b4cb08021a5dd4d8c1e7acc47d872fbe52d3d6b LICENCE.broadcom
EOF

# The card: 64 MiB, its partition from sector 8192, as the Raspberry Pi
# Imager leaves it.
need sfdisk mkfs.fat mcopy
card=$DIR/card.img.new
part=$DIR/part.img.new
rm -f "$card" "$part"
truncate -s 64M "$card"
echo 'start=8192, type=c' | sfdisk -q "$card"
truncate -s 60M "$part"
mkfs.fat -F 32 -n BOOT "$part" > /dev/null
printf 'arm_64bit=1\nenable_uart=1\nkernel=u-boot.bin\n' > "$DIR/config.txt"
mcopy -i "$part" "$FIRMWARE/bootcode.bin" "$FIRMWARE/start.elf" \
	"$FIRMWARE/fixup.dat" "$FIRMWARE/bcm2710-rpi-3-b.dtb" \
	"$FIRMWARE/LICENCE.broadcom" "$BUILD/u-boot.bin" "$DIR/config.txt" ::
dd if="$part" of="$card" bs=1M seek=4 conv=notrunc status=none
rm "$part"
mv "$card" "$DIR/card.img"
echo "uboot-card: $DIR/card.img"
