# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The integer instructions of Advanced SIMD (step E2 of roadmap item 4),
# in the working tree: the lines changed since 3b3801f.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:3b3801f"} for p in [
	"src/cpu/arm64/simd_int.c",
	"src/cpu/arm64/simd_shift.c",
	"src/cpu/arm64/simd_same.c",
	"src/cpu/arm64/simd_perm.c",
	"src/cpu/arm64/simd_long.c",
	"src/cpu/arm64/simd.c",
	"src/cpu/arm64/internal.h",
]]
WORKERS = 4
JOBS = 3
