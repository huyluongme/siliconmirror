#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The checks of every task, with a line of output each (failures in full):
# format, layering, each preset built and tested, gcc 13's warnings.
#
#   sh .claude/tools/check.sh [PRESET...]
#
# Default presets: debug asan ci-clang ci-release-gcc release (release
# runs the U-Boot and Linux sessions, a few minutes). Never run it while
# a mutation run is going: they share the CPU and time out.
cd "$(git rev-parse --show-toplevel)" || exit 2
presets=${*:-debug asan ci-clang ci-release-gcc release}
status=0

if make -s check-format >/dev/null 2>&1; then echo "format ok"
else echo "format FAILED"; make -s check-format 2>&1 | head -20; status=1; fi

if out=$(bash scripts/check-layering.sh src 2>&1); then echo "layering ok"
else echo "layering FAILED"; echo "$out" | head -20; status=1; fi

for p in $presets; do
	# ci-clang and ci-release-gcc are the ci presets with CC set, as CI
	# builds them (the Makefile names such directories PRESET-CC).
	case $p in
	ci-clang) conf="CC=clang cmake --preset ci -B build/ci-clang" ;;
	ci-release-gcc) conf="CC=gcc cmake --preset ci-release -B build/ci-release-gcc" ;;
	*) conf="cmake --preset $p" ;;
	esac
	[ -f "build/$p/CMakeCache.txt" ] || sh -c "$conf" >/dev/null || {
		echo "$p: configure FAILED"; status=1; continue; }
	if ! out=$(cmake --build "build/$p" 2>&1); then
		echo "$p: build FAILED"
		echo "$out" | grep -E 'error|warning' | head -20
		status=1
		continue
	fi
	out=$(ctest --test-dir "build/$p" -j4 --output-on-failure </dev/null 2>&1)
	echo "$p: $(echo "$out" | grep 'tests passed')"
	echo "$out" | grep -q '100% tests passed' || {
		echo "$out" | grep -E '\*\*\*|Failed|FAILED' | head -20; status=1; }
done

if [ -f build/ci-release-gcc/compile_commands.json ]; then
	echo "$(python3 .claude/tools/gcc13_check.py 2>&1 | tail -1)"
fi
exit $status
