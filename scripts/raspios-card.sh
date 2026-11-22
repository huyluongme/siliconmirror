#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Make an SD card of Raspberry Pi OS Lite (64-bit) as Raspberry Pi
# Imager writes one: the release image unchanged, on a card larger than
# the image, and a cloud-init user-data on the boot partition that makes
# a user and turns the serial console on. Nothing else is changed.
#
# Usage: scripts/raspios-card.sh [DIR]
#
# DIR, ~/.cache/siliconmirror-raspios by default, gets:
#   RELEASE.img.xz     the release, checked against its SHA-256
#   RELEASE.img        the image, unpacked once and kept as it is
#   user-data          what Imager would write
#   card.img           the card (sparse): the image, then empty space up
#                      to CARD_SIZE, with user-data on its boot partition
# The download and the unpacked image are kept and used again; the card
# is made anew each time from the image, so each boot starts from the
# card as it comes out of Imager (the first boot grows the root
# partition, and cloud-init writes to both partitions).
#
# The user is pi, password raspberry (the old default of Raspberry Pi OS;
# Imager asks for one). cloud-init's Raspberry Pi module, with
# rpi.interfaces.serial true, runs raspi-config do_serial_cons and
# do_serial_hw: enable_uart=1 in config.txt and console=serial0 in
# cmdline.txt, then a reboot. Until that reboot the serial console is
# silent after the firmware, as on the real board (enable_uart=0).
#
# Needs curl, sha256sum, xz, truncate, and mtools.

set -eu

RELEASE=2026-10-06-raspios-trixie-arm64-lite
RELEASE_SHA256=483db18a48da399b5b7022ffae9a07bc6d99daab30e593aedac09b69ff422843
RELEASE_URL=https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-10-06/$RELEASE.img.xz
# An "8 GB" card. Real ones are a little smaller and differ by maker;
# the size only sets how far the first boot grows the root partition.
CARD_SIZE=8G
# The boot partition (FAT32) starts at sector 16384 in this release.
BOOT_OFFSET=8M

DIR=${1:-$HOME/.cache/siliconmirror-raspios}

need() {
	for tool in "$@"; do
		if ! command -v "$tool" > /dev/null; then
			echo "raspios-card: $tool is missing" >&2
			exit 1
		fi
	done
}

need curl sha256sum xz truncate mcopy
mkdir -p "$DIR"
xzfile=$DIR/$RELEASE.img.xz
img=$DIR/$RELEASE.img
if [ ! -f "$img" ]; then
	if ! echo "$RELEASE_SHA256  $xzfile" | sha256sum -c --status \
		> /dev/null 2>&1; then
		curl -fsSL -o "$xzfile" "$RELEASE_URL"
		echo "$RELEASE_SHA256  $xzfile" | sha256sum -c --quiet
	fi
	xz -dkc "$xzfile" > "$img.new"
	mv "$img.new" "$img"
fi

# The password's SHA-512 crypt hash (openssl passwd -6 -salt smirror
# raspberry), with a fixed salt so that the card is the same each time.
cat > "$DIR/user-data" << 'EOF'
#cloud-config
hostname: raspberrypi
users:
- name: pi
  groups: users,adm,dialout,audio,netdev,video,plugdev,cdrom,games,input,gpio,spi,i2c,render,sudo
  shell: /bin/bash
  lock_passwd: false
  passwd: $6$smirror$dzgenq14C9CYzgmPPDYg6kL/qgaYyjKFFuOgO2SLbbjeOjl/YQ4pr9KESXeFv3Q7hEZbWkU43ibXPdQ3MENB50
rpi:
  interfaces:
    serial: true
EOF

card=$DIR/card.img.new
rm -f "$card"
cp --sparse=always "$img" "$card"
truncate -s "$CARD_SIZE" "$card"
MTOOLS_SKIP_CHECK=1 mcopy -o -i "$card@@$BOOT_OFFSET" "$DIR/user-data" ::user-data
mv "$card" "$DIR/card.img"
echo "raspios-card: $DIR/card.img"
