# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The performance monitors and the self-hosted debug registers (step B
# of roadmap item 4), in the working tree: the lines changed since
# 9246208.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:9246208"} for p in [
	"src/cpu/arm64/pmu.c",
	"src/cpu/arm64/debug.c",
	"src/cpu/arm64/sysreg.c",
	"src/cpu/arm64/cpu.c",
]]
WORKERS = 4
JOBS = 3
