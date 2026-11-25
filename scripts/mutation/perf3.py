# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# MRS of CNTPCT_EL0 and CNTVCT_EL0 ahead of the System register table
# search (before the JIT), in the working tree: the lines changed since
# d77d2e4.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:d77d2e4"} for p in [
	"src/cpu/arm64/sysreg.c",
	"src/cpu/arm64/timer.c",
]]
WORKERS = 4
JOBS = 3
