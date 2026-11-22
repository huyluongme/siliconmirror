#!/bin/sh
# Boot a debug copy of the Raspberry Pi OS card (scripts/raspios-card.sh)
# with the local exploration hacks of linux_explore.sh (never committed):
# enable_uart=1 and the console on serial only, so that the first boot
# and systemd's status show; unmapped peripherals read 0 with a log line.
# Runs on the virtual clock (with -clock host, systemd's watchdog of a
# minute resets the board in the middle of the first boot).
# Usage (from the repo): sh .claude/tools/raspios_explore.sh OUT_PREFIX
# Writes OUT_PREFIX.out (serial) and .err; T=seconds limits the run.
set -u
L=$HOME/.cache/siliconmirror-linux
R=$HOME/.cache/siliconmirror-raspios
OUT=$1
INC="--include=src/core/bus.c --include=src/machine/raspi3b_property.c --include=src/frontend/main.c --include=src/cpu/arm64/sysreg.c"
git apply $INC $L/explore.patch || exit 1
cmake --build build/release -j8 > /dev/null 2>&1
cp --sparse=always $R/card.img $R/run.img
export MTOOLS_SKIP_CHECK=1
mtype -i $R/run.img@@8M ::config.txt > $R/config.explore
echo enable_uart=1 >> $R/config.explore
mcopy -o -i $R/run.img@@8M $R/config.explore ::config.txt
mtype -i $R/run.img@@8M ::cmdline.txt | tr -d "\r\n" |
	sed "s/ console=tty1//; s/$/ systemd.show_status=1\n/" > $R/cmdline.explore
mcopy -o -i $R/run.img@@8M $R/cmdline.explore ::cmdline.txt
SMIRROR_EXPLORE=1 timeout ${T:-5400} ./build/release/smirror -M raspi3b -sd $R/run.img < /dev/null > "$OUT.out" 2> "$OUT.err"
echo "status=$?"
git apply -R $INC $L/explore.patch && echo reverted
cmake --build build/release -j8 > /dev/null 2>&1
rm -f $R/run.img
echo DONE
