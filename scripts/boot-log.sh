#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Boot the whole chains and keep what the serial console shows, so that
# each change shows in these files' history how far the boots get:
#   docs/boot-logs/raspi3b-uboot-linux.log: the firmware, U-Boot and
#     Linux, on the card of scripts/uboot-linux-card.sh, to the shell
#     of its initramfs (scripts/boot-log.txt);
#   docs/boot-logs/raspi3b-raspios.log: the firmware and Raspberry Pi
#     OS, on the card of scripts/raspios-card.sh, to its login prompt
#     (scripts/boot-log-raspios.txt), about ten minutes. Its first boot
#     says nothing on the serial console, as on the real board.
# The boots run on the virtual clock: the same build prints the same
# logs. Each chain boots twice at once, with the interpreter (-jit off)
# and with the JIT (-jit on), which must print the same log byte for
# byte: virtual time would show any difference in what the guest ran.
# The JIT's log is kept as DIR/boot-jit.log when the two differ.
#
# Usage: scripts/boot-log.sh [BUILD]
#
# BUILD, build/release by default, holds smirror and drive. A log ends
# with what drive says when the boot does not get there, such as the
# error that stopped the machine.

set -eu

BUILD=${1:-build/release}

# run DIR NAME SCRIPT JIT: boot DIR/card.img through SCRIPT with -jit JIT
# into DIR/NAME.log; DIR/NAME.status holds drive's exit status.
run() {
	status=0
	"$BUILD/drive" "$1/card.img" "$1/$2.img" "$3" \
		"$BUILD/smirror" -M raspi3b -sd "$1/$2.img" -jit "$4" \
		> "$1/$2.out" 2> "$1/$2.err" || status=$?
	tr -d '\r' < "$1/$2.out" > "$1/$2.log"
	if [ "$status" -ne 0 ]; then
		printf '\n' >> "$1/$2.log"
		cat "$1/$2.err" >> "$1/$2.log"
	fi
	echo "$status" > "$1/$2.status"
	rm -f "$1/$2.img"
}

# log DIR LOG SCRIPT: boot DIR/card.img through SCRIPT into LOG, in both
# modes; fails when the JIT's log is not the interpreter's.
log() {
	run "$1" boot "$3" off &
	run "$1" boot-jit "$3" on &
	wait
	mv "$1/boot.log" "$2"
	echo "boot-log: $2 (drive status $(cat "$1/boot.status"))"
	if ! cmp -s "$2" "$1/boot-jit.log"; then
		echo "boot-log: -jit on gives another log: $1/boot-jit.log" \
			"(drive status $(cat "$1/boot-jit.status"))" >&2
		exit 1
	fi
	rm -f "$1/boot-jit.log"
}

mkdir -p docs/boot-logs
DIR=$HOME/.cache/siliconmirror-uboot-linux
sh scripts/uboot-linux-card.sh "$DIR" > /dev/null
log "$DIR" docs/boot-logs/raspi3b-uboot-linux.log scripts/boot-log.txt
DIR=$HOME/.cache/siliconmirror-raspios
sh scripts/raspios-card.sh "$DIR" > /dev/null
log "$DIR" docs/boot-logs/raspi3b-raspios.log scripts/boot-log-raspios.txt
