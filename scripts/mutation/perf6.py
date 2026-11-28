# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The JIT sees code that is written (performance phase 3a.3): watched
# pages of RAM on the bus, the cache's pages and drops, the write page
# cache and the blocks that stop after a drop; the lines changed since
# 5701d4d, in the working tree.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:5701d4d"} for p in [
	"src/core/bus.c",
	"src/cpu/jit/cache.c",
	"src/cpu/arm64/jit.c",
	"src/cpu/arm64/cpu.c",
	"src/cpu/arm64/mmu.c",
	"src/machine/raspi3b.c",
]]
WORKERS = 3
JOBS = 3
