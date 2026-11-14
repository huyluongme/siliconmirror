# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The fast paths of the interpreter (phase 1 of the speed-up), in the
# working tree, and the BMCR change before them: the lines changed since
# 068ff73.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:068ff73"} for p in [
	"src/core/bus.c",
	"src/cpu/arm64/cpu.c",
	"src/cpu/arm64/internal.h",
	"src/cpu/arm64/mem.c",
	"src/cpu/arm64/mmu.c",
	"src/hw/intc/bcm2836_local.c",
	"src/hw/net/lan95xx.c",
	"src/util/muldiv.h",
]]
WORKERS = 4
JOBS = 3
