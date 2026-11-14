# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The SIMD&FP registers, their loads, stores and moves, FPCR and FPSR and
# their traps (step E1 of roadmap item 4), in the working tree: the lines
# changed since 49f5d18.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:49f5d18"} for p in [
	"src/cpu/arm64/simd.c",
	"src/cpu/arm64/simd_ldst.c",
	"src/cpu/arm64/load_store.c",
	"src/cpu/arm64/mem.c",
	"src/cpu/arm64/exception.c",
	"src/cpu/arm64/sysreg.c",
	"src/cpu/arm64/cpu.c",
	"src/cpu/arm64/internal.h",
]]
WORKERS = 4
JOBS = 3
