#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Fetch gcc 13's cc1 (CI's compiler, Ubuntu 24.04) for gcc13_check.py,
# without root: apt-get download + dpkg -x into the tools cache.
set -e
dir=$HOME/.cache/siliconmirror-tools/gcc13
[ -x "$dir/root/usr/libexec/gcc/x86_64-linux-gnu/13/cc1" ] && { echo "already there: $dir"; exit 0; }
mkdir -p "$dir" && cd "$dir"
apt-get download cpp-13-x86-64-linux-gnu
dpkg -x cpp-13-x86-64-linux-gnu_*.deb root
echo "cc1 in $dir/root/usr/libexec/gcc/x86_64-linux-gnu/13/"
