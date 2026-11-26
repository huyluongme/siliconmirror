# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The JIT's blocks, cache and dispatcher, and -jit (performance phase
# 3a.2): new files whole, the changed ones since a50e8cd, in the working
# tree.
SNAPSHOT = None
FILES = [{"path": p, "lines": "all"} for p in [
	"src/cpu/jit/cache.c",
	"src/cpu/arm64/jit.c",
]] + [{"path": p, "lines": "since:a50e8cd"} for p in [
	"src/cpu/arm64/cpu.c",
	"src/machine/raspi3b.c",
	"src/frontend/main.c",
]]
WORKERS = 3
JOBS = 3
