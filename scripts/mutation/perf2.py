# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The host clock (phase 2 of the speed-up), in the working tree: the
# lines changed since 6240de8.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:6240de8"} for p in [
	"src/backend/clock/host.c",
	"src/core/machine.c",
	"src/frontend/main.c",
]]
WORKERS = 4
JOBS = 3
