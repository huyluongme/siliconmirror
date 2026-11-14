#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Make an SD card that boots Linux to a shell on the serial console: the
# Raspberry Pi firmware files and the kernel8.img of the same release,
# as on a real card, with an initramfs of BusyBox.
#
# Usage: scripts/linux-card.sh [DIR]
#
# DIR, ~/.cache/siliconmirror-linux by default, gets:
#   firmware-TAG/      the firmware files and the kernel, checked against
#                      their SHA-256; System8.map names the kernel's
#                      addresses
#   busybox-VERSION/   BusyBox's source, and build/ with its static build
#   initramfs/         the initramfs's files, and initramfs.gz
#   card.img           the card: an MBR, and a FAT32 partition at 4 MiB
# The downloads and the build are kept and used again; the initramfs
# and the card are made anew each time.
#
# Needs curl, sha256sum, tar, bzip2, make, a cross compiler
# (CROSS_COMPILE, aarch64-linux-gnu- by default), cpio and gzip, and
# sfdisk, mkfs.fat and mtools to make the card.

set -eu

# The release of scripts/uboot-card.sh. Its kernel8.img is built from
# raspberrypi/linux cff533aec2fa (extra/git_hash) with bcm2711_defconfig,
# the configuration of a 64-bit Raspberry Pi 3.
FIRMWARE_TAG=1.20260915
FIRMWARE_URL=https://raw.githubusercontent.com/raspberrypi/firmware/$FIRMWARE_TAG
BUSYBOX_VERSION=1.38.0
BUSYBOX_SHA256=34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2
BUSYBOX_URL=https://busybox.net/downloads/busybox-$BUSYBOX_VERSION.tar.bz2

DIR=${1:-$HOME/.cache/siliconmirror-linux}
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
FIRMWARE=$DIR/firmware-$FIRMWARE_TAG
BUSYBOX=$DIR/busybox-$BUSYBOX_VERSION
ROOT=$DIR/initramfs

need() {
	for tool in "$@"; do
		if ! command -v "$tool" > /dev/null; then
			echo "linux-card: $tool is missing" >&2
			exit 1
		fi
	done
}

mkdir -p "$DIR"

# The firmware files, the kernel and its symbols.
need curl sha256sum
mkdir -p "$FIRMWARE"
while read -r sum path; do
	file=$FIRMWARE/${path##*/}
	if ! echo "$sum  $file" | sha256sum -c --status > /dev/null 2>&1; then
		curl -fsSL -o "$file" "$FIRMWARE_URL/$path"
		echo "$sum  $file" | sha256sum -c --quiet
	fi
done << EOF
4245ec23b58158dc3e55f3e065eb4ecc0c0705d76c8db506e9f824941feee32e boot/bootcode.bin
32f990801e7765bae6fe5cb940ba5eeff02dd0dac8dc5474db5b3fd345281790 boot/start.elf
7853f2b51e4489f8b3ce6f9c52fd0afdda38c7383961524e0883d34afcb4827b boot/fixup.dat
cd922193672e391b6194d6d94e34ffab6fc154205375fcfb362cad191f7f8aae boot/bcm2710-rpi-3-b.dtb
c7283ff51f863d93a275c66e3b4cb08021a5dd4d8c1e7acc47d872fbe52d3d6b boot/LICENCE.broadcom
f80aa8bb092c0df9401065ec1b4ee8b8846c4d9a1bb3272662b50acbd89da68b boot/kernel8.img
f917a6c89f1e4b87935db96337e3864b74467ff51b000fe1456fa1f1e074ece2 extra/System8.map
EOF

# BusyBox, linked statically: the initramfs has no C library.
if [ ! -x "$BUSYBOX/build/busybox" ]; then
	need tar bzip2 make "${CROSS_COMPILE}gcc"
	tarball=$DIR/busybox-$BUSYBOX_VERSION.tar.bz2
	if ! echo "$BUSYBOX_SHA256  $tarball" | sha256sum -c --status \
		> /dev/null 2>&1; then
		curl -fsSL -o "$tarball" "$BUSYBOX_URL"
		echo "$BUSYBOX_SHA256  $tarball" | sha256sum -c --quiet
	fi
	rm -rf "$BUSYBOX"
	tar -xjf "$tarball" -C "$DIR"
	mkdir -p "$BUSYBOX/build"
	make -s -C "$BUSYBOX" O="$BUSYBOX/build" defconfig > /dev/null
	# tc needs CBQ, which the kernel headers of Linux 6.8 and later no
	# longer have.
	sed -i -e 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' \
		-e 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' \
		"$BUSYBOX/build/.config"
	make -s -C "$BUSYBOX/build" CROSS_COMPILE="$CROSS_COMPILE" \
		oldconfig < /dev/null > /dev/null
	make -s -C "$BUSYBOX/build" CROSS_COMPILE="$CROSS_COMPILE" \
		-j "$(nproc)" > /dev/null
fi

# The initramfs: BusyBox, and /init, which mounts the kernel's file
# systems and starts a shell on the console.
need cpio gzip
rm -rf "$ROOT"
mkdir -p "$ROOT/files"
make -s -C "$BUSYBOX/build" CROSS_COMPILE="$CROSS_COMPILE" \
	CONFIG_PREFIX="$ROOT/files" install > /dev/null
mkdir -p "$ROOT/files/dev" "$ROOT/files/proc" "$ROOT/files/sys" \
	"$ROOT/files/tmp"
cat > "$ROOT/files/init" << 'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo "linux-card: the initramfs runs"
exec setsid cttyhack sh
EOF
chmod 755 "$ROOT/files/init"
(cd "$ROOT/files" && find . | LC_ALL=C sort |
	cpio --quiet -o -H newc -R 0:0) | gzip -9 > "$ROOT/initramfs.gz"

# The card.
need sfdisk mkfs.fat mcopy
card=$DIR/card.img.new
part=$DIR/part.img.new
rm -f "$card" "$part"
truncate -s 128M "$card"
echo 'start=8192, type=c' | sfdisk -q "$card"
truncate -s 124M "$part"
mkfs.fat -F 32 -n BOOT "$part" > /dev/null
printf 'arm_64bit=1\nenable_uart=1\ninitramfs initramfs.gz followkernel\n' \
	> "$DIR/config.txt"
printf 'console=serial0,115200\n' > "$DIR/cmdline.txt"
mcopy -i "$part" "$FIRMWARE/bootcode.bin" "$FIRMWARE/start.elf" \
	"$FIRMWARE/fixup.dat" "$FIRMWARE/bcm2710-rpi-3-b.dtb" \
	"$FIRMWARE/LICENCE.broadcom" "$FIRMWARE/kernel8.img" \
	"$ROOT/initramfs.gz" "$DIR/config.txt" "$DIR/cmdline.txt" ::
dd if="$part" of="$card" bs=1M seek=4 conv=notrunc status=none
rm "$part"
mv "$card" "$DIR/card.img"
echo "linux-card: $DIR/card.img"
