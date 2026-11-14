# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The exclusive monitors, LDAR and STLR, CLREX, LDTR and STTR (step C of
# roadmap item 4), in the working tree: the lines changed since c7948f5.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:c7948f5"} for p in [
	"src/cpu/arm64/exclusive.c",
	"src/cpu/arm64/cpu.c",
	"src/cpu/arm64/mem.c",
	"src/cpu/arm64/load_store.c",
	"src/cpu/arm64/branch_sys.c",
	"src/cpu/arm64/exception.c",
	"src/cpu/arm64/sysinsn.c",
	"src/cpu/arm64/mmu.c",
]]
WORKERS = 4
JOBS = 3
