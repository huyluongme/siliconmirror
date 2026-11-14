#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Measure how fast smirror runs a guest: a loop of 200 million
# instructions, and U-Boot from power on to its prompt.
#
# Usage: scripts/bench.sh [BUILD]
#
# BUILD is build/release by default (make PRESET=release): it has
# smirror and the drive tool of tests/uboot. Each measure takes the
# best of 3 runs. The U-Boot boot needs the card of scripts/uboot-card.sh
# (SMIRROR_UBOOT_CARD, ~/.cache/siliconmirror-uboot/card.img by default)
# and is skipped without it. Needs the GNU binutils for AArch64.

set -eu

BUILD=${1:-build/release}
SMIRROR=$BUILD/smirror
DRIVE=$BUILD/drive
CARD=${SMIRROR_UBOOT_CARD:-$HOME/.cache/siliconmirror-uboot/card.img}
CROSS=${CROSS_COMPILE:-aarch64-linux-gnu-}
WORK=$BUILD/bench
RUNS=3

if [ ! -x "$SMIRROR" ]; then
	echo "bench: no $SMIRROR: build it with make PRESET=release" >&2
	exit 1
fi
mkdir -p "$WORK"

now() {
	date +%s.%N
}

# The best of RUNS runs of the command, in seconds.
best() {
	b=
	i=0
	while [ $i -lt $RUNS ]; do
		start=$(now)
		if ! "$@" > /dev/null 2>&1 < /dev/null; then
			echo "bench: $1 failed" >&2
			exit 1
		fi
		t=$(echo "$start $(now)" | awk '{ printf "%.3f", $2 - $1 }')
		if [ -z "$b" ] || [ "$(echo "$t $b" | awk '{ print ($1 < $2) }')" = 1 ]
		then
			b=$t
		fi
		i=$((i + 1))
	done
	echo "$b"
}

# The loop: 100 million SUBS and B.NE, then WFI with nothing to wake
# the core, so that the machine stops, idle.
cat > "$WORK/loop.s" << 'EOF'
	movz x0, #0x05f5, lsl #16
	movk x0, #0xe100
loop:	subs x0, x0, #1
	b.ne loop
done:	wfi
	b done
EOF
"${CROSS}as" -o "$WORK/loop.o" "$WORK/loop.s"
"${CROSS}ld" -Ttext=0x80000 -e 0x80000 -o "$WORK/loop.elf" "$WORK/loop.o"
"${CROSS}objcopy" -O binary "$WORK/loop.elf" "$WORK/loop.bin"
t=$(best "$SMIRROR" -M raspi3b -kernel "$WORK/loop.bin" -serial null)
echo "$t" | awk '{ printf "loop:   %7.3f s, %5.1f M instructions/s\n", $1, 200 / $1 }'

if [ ! -f "$CARD" ] || [ ! -x "$DRIVE" ]; then
	echo "u-boot: skipped, no $CARD or $DRIVE"
	exit 0
fi
cat > "$WORK/uboot.txt" << 'EOF'
timeout 900
expect Hit any key to stop autoboot
type \x20
expect U-Boot>
quit
EOF
t=$(best "$DRIVE" "$CARD" "$WORK/card.img" "$WORK/uboot.txt" "$SMIRROR" \
	-M raspi3b -sd "$WORK/card.img")
echo "$t" | awk '{ printf "u-boot: %7.3f s to the prompt\n", $1 }'
