#!/bin/sh
# Boot the Linux card with the local exploration hacks (never committed):
# unmapped peripherals read 0 / ignore writes, unemulated mailbox tags and
# missing System registers are ignored, with a log on stderr.
# Usage (from the repo): sh .claude/tools/linux_explore.sh CMDLINE_FILE OUT_PREFIX
# CMDLINE_FILE: ~/.cache/siliconmirror-linux/cmdline-explore.txt (4 CPUs, earlycon)
#               or cmdline-nosmp.txt (1 CPU). Writes OUT_PREFIX.out/.err.
set -u
L=$HOME/.cache/siliconmirror-linux
INC="--include=src/core/bus.c --include=src/machine/raspi3b_property.c --include=src/frontend/main.c --include=src/cpu/arm64/sysreg.c"
git apply $INC $L/explore.patch || exit 1
cmake --build build/release -j8 > /dev/null 2>&1
cp $L/card.img $L/run.img
mdel -i $L/run.img@@4M ::cmdline.txt
mcopy -i $L/run.img@@4M "$1" ::cmdline.txt
SMIRROR_EXPLORE=1 timeout 1500 ./build/release/smirror -M raspi3b -clock host -sd $L/run.img < /dev/null > "$2.out" 2> "$2.err"
echo "status=$?"
git apply -R $INC $L/explore.patch && echo reverted
cmake --build build/release -j8 > /dev/null 2>&1
tail -1 "$2.err"
