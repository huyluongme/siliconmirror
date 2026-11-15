#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Boot the whole chain, the firmware, U-Boot and Linux, on the card of
# scripts/uboot-linux-card.sh, and keep what the serial console shows in
# docs/boot-logs/raspi3b-uboot-linux.log, so that each change shows in
# that file's history how far the boot gets. The boot runs on the
# virtual clock: the same build prints the same log.
#
# Usage: scripts/boot-log.sh [BUILD]
#
# BUILD, build/release by default, holds smirror and drive. The log ends
# with what drive says when the boot does not reach the shell, such as
# the error that stopped the machine.

set -eu

BUILD=${1:-build/release}
DIR=$HOME/.cache/siliconmirror-uboot-linux
LOG=docs/boot-logs/raspi3b-uboot-linux.log

sh scripts/uboot-linux-card.sh "$DIR" > /dev/null
mkdir -p docs/boot-logs
status=0
"$BUILD/drive" "$DIR/card.img" "$DIR/run.img" scripts/boot-log.txt \
	"$BUILD/smirror" -M raspi3b -sd "$DIR/run.img" > "$DIR/boot.out" \
	2> "$DIR/boot.err" || status=$?
tr -d '\r' < "$DIR/boot.out" > "$LOG"
if [ "$status" -ne 0 ]; then
	printf '\n' >> "$LOG"
	cat "$DIR/boot.err" >> "$LOG"
fi
rm -f "$DIR/run.img"
echo "boot-log: $LOG (drive status $status)"
