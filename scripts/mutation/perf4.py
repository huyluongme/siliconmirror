# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The JIT's x86-64 emitter and executable memory (performance phase
# 3a.1), new files, in the working tree.
SNAPSHOT = None
FILES = [{"path": p, "lines": "all"} for p in [
	"src/cpu/jit/x64.c",
	"src/cpu/jit/exec_mem.c",
]]
WORKERS = 4
JOBS = 3
