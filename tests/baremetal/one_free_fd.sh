#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Run a command with one file descriptor free: the limit on descriptors
# is one above the lowest one that is not open now. The dynamic loader
# and the command's first file get it; a second file gets EMFILE.
# Usage: one_free_fd.sh PRLIMIT COMMAND [ARGUMENT]...
prlimit=$1
shift
fd=0
while [ -e "/proc/$$/fd/$fd" ]; do
	fd=$((fd + 1))
done
exec "$prlimit" --nofile=$((fd + 1)) "$@"
