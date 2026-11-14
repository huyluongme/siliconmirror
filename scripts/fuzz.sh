#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Fuzz what smirror reads from an SD card: the MBR, FAT, config.txt and
# devicetree blobs (tests/fuzz). Each fuzzer is built with clang's
# libFuzzer, AddressSanitizer and UBSan, starts from seeds that real
# tools make, and runs for SECONDS (60 by default). A crash stops it and
# leaves the input that made it in build/fuzz/.
#
# Usage: scripts/fuzz.sh [SECONDS [FUZZER...]]
#   FUZZER: mbr, fat, config, fdt; all of them by default.
#
# Needs clang, sfdisk, mkfs.fat and mtools; the fdt fuzzer starts from
# ~/.cache/siliconmirror-uboot's firmware devicetree when it is there
# (scripts/uboot-card.sh).

set -eu

cd "$(dirname "$0")/.."
SECONDS_EACH=${1:-60}
[ $# -gt 0 ] && shift
FUZZERS=${*:-mbr fat config fdt}
OUT=build/fuzz
FLAGS="-std=c11 -g -O1 -fsanitize=fuzzer,address,undefined
	-fno-sanitize-recover=undefined -Isrc -Itests/fuzz"

sources() {
	case $1 in
	mbr) echo src/boot/mbr.c ;;
	fat | config) echo src/boot/fat.c src/boot/rpi_config.c ;;
	fdt) echo src/util/fdt.c ;;
	esac
}

# Seeds, as a real card has them.
seeds() {
	dir=$OUT/seeds/$1
	rm -rf "$dir"
	mkdir -p "$dir"
	case $1 in
	mbr)
		truncate -s 1M "$dir/one"
		echo 'start=64, type=c' | sfdisk -q "$dir/one"
		truncate -s 1M "$dir/two"
		printf 'start=64, size=512, type=c\nstart=1024, type=83\n' |
			sfdisk -q "$dir/two"
		;;
	fat)
		printf 'arm_64bit=1\n[pi3]\ninclude extra.txt\n[all]\nkernel=u-boot.bin\n' \
			> "$OUT/config.txt"
		printf 'enable_uart=1\n' > "$OUT/extra.txt"
		for spec in 12:64 16:2200; do
			img=$dir/fat${spec%%:*}
			mkfs.fat -C -F "${spec%%:*}" -s 1 -n BOOT "$img" "${spec#*:}" \
				> /dev/null
			mmd -i "$img" ::a ::a/b
			mcopy -i "$img" "$OUT/config.txt" "$OUT/extra.txt" ::
			mcopy -i "$img" "$OUT/extra.txt" ::a/b/c.txt
			mcopy -i "$img" "$OUT/extra.txt" ::LongFileName.txt
		done
		;;
	config)
		printf 'arm_64bit=1\nenable_uart=1\nkernel=u-boot.bin\n' > "$dir/plain"
		printf '[pi3]\nkernel=a # note\n[gpio4=1]\ninitramfs i.img followkernel\n[0x12345678]\n[none]\nx=1\n[all]\nkernel_address=0x80000\n' \
			> "$dir/filters"
		;;
	fdt)
		dtb=$HOME/.cache/siliconmirror-uboot/firmware-1.20260915/bcm2710-rpi-3-b.dtb
		if [ -f "$dtb" ]; then
			cp "$dtb" "$dir/"
		else
			printf '\320\015\376\355' > "$dir/magic"
		fi
		;;
	esac
}

mkdir -p "$OUT"
for f in $FUZZERS; do
	# shellcheck disable=SC2046,SC2086
	clang $FLAGS "tests/fuzz/fuzz_$f.c" $(sources "$f") -o "$OUT/fuzz_$f"
	seeds "$f"
	mkdir -p "$OUT/corpus/$f"
	max=$(find "$OUT/seeds/$f" -type f -printf '%s\n' | sort -n | tail -1)
	[ "$max" -lt 4096 ] && max=4096
	echo "fuzz_$f: $SECONDS_EACH s"
	(cd "$OUT" && "./fuzz_$f" -max_total_time="$SECONDS_EACH" \
		-max_len="$max" -print_final_stats=1 "corpus/$f" "seeds/$f" \
		2>&1 | grep -E "^(stat::number_of_executed_units|stat::peak_rss|==[0-9]+==ERROR|SUMMARY|artifact_prefix|Test unit written|#[0-9]+.*DONE)" || true)
	if ls "$OUT"/crash-* "$OUT"/leak-* "$OUT"/timeout-* > /dev/null 2>&1; then
		echo "fuzz_$f: found a problem, see $OUT"
		exit 1
	fi
done
