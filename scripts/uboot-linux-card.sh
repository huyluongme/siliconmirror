#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Make an SD card on which the whole chain boots, as on a real card that
# uses U-Boot: the Raspberry Pi firmware starts U-Boot, whose boot.scr
# loads Linux and the BusyBox initramfs and starts the kernel with
# booti. U-Boot is that of scripts/uboot-card.sh; the kernel, the
# initramfs and the firmware are those of scripts/linux-card.sh, which
# both scripts make first in their own directories.
#
# Usage: scripts/uboot-linux-card.sh [DIR]
#
# DIR, ~/.cache/siliconmirror-uboot-linux by default, gets:
#   Image              the kernel, uncompressed, as booti wants it
#   boot.cmd, boot.scr U-Boot's boot script, and its image
#   card.img           the card: an MBR, and a FAT32 partition at 4 MiB
# The card is made anew each time, the same each time: its disk
# identifier, its file system's volume ID and its files' dates are
# fixed, the dates at the firmware release, 2026-09-15 00:00 UTC.
#
# Needs what both scripts need, and gzip.

set -eu

UBOOT_VERSION=2026.07
FIRMWARE_TAG=1.20260915

DIR=${1:-$HOME/.cache/siliconmirror-uboot-linux}
UBOOT_DIR=$HOME/.cache/siliconmirror-uboot
LINUX_DIR=$HOME/.cache/siliconmirror-linux
SCRIPTS=$(dirname "$0")
BUILD=$UBOOT_DIR/build-$UBOOT_VERSION
FIRMWARE=$LINUX_DIR/firmware-$FIRMWARE_TAG

sh "$SCRIPTS/uboot-card.sh" "$UBOOT_DIR" > /dev/null
sh "$SCRIPTS/linux-card.sh" "$LINUX_DIR" > /dev/null

# The date that mkimage and mtools give what they make.
SOURCE_DATE_EPOCH=1789430400
export SOURCE_DATE_EPOCH

mkdir -p "$DIR"
gzip -dc "$FIRMWARE/kernel8.img" > "$DIR/Image"

# The console is the mini UART, ttyS0: U-Boot does not replace serial0
# in the command line as the firmware does. earlycon, on the device
# tree's stdout-path, shows the kernel's messages before ttyS0 is up.
cat > "$DIR/boot.cmd" << 'EOF'
fatload mmc 0:1 ${kernel_addr_r} Image
fatload mmc 0:1 ${ramdisk_addr_r} initramfs.gz
setenv bootargs console=ttyS0,115200 earlycon
booti ${kernel_addr_r} ${ramdisk_addr_r}:${filesize} ${fdt_addr}
EOF
"$BUILD/tools/mkimage" -A arm64 -T script -C none -d "$DIR/boot.cmd" \
	"$DIR/boot.scr" > /dev/null

card=$DIR/card.img.new
part=$DIR/part.img.new
rm -f "$card" "$part"
truncate -s 128M "$card"
printf 'label-id: 0x736d6972\nstart=8192, type=c\n' | sfdisk -q "$card"
truncate -s 124M "$part"
mkfs.fat -F 32 --invariant -n BOOT "$part" > /dev/null
printf 'arm_64bit=1\nenable_uart=1\nkernel=u-boot.bin\n' > "$DIR/config.txt"
mcopy -i "$part" "$FIRMWARE/bootcode.bin" "$FIRMWARE/start.elf" \
	"$FIRMWARE/fixup.dat" "$FIRMWARE/bcm2710-rpi-3-b.dtb" \
	"$FIRMWARE/LICENCE.broadcom" "$BUILD/u-boot.bin" "$DIR/config.txt" \
	"$DIR/Image" "$LINUX_DIR/initramfs/initramfs.gz" "$DIR/boot.scr" ::
dd if="$part" of="$card" bs=1M seek=4 conv=notrunc status=none
rm "$part"
mv "$card" "$DIR/card.img"
echo "uboot-linux-card: $DIR/card.img"
