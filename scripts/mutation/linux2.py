# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The firmware's GPIO expander, and gunzip()'s outputs set for gcc 13,
# in the working tree: the lines changed since 808e7a5.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:808e7a5"} for p in [
	"src/util/inflate.c",
	"src/machine/raspi3b_property.c",
]]
WORKERS = 4
JOBS = 3
