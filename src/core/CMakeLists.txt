# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# A static library: the .c files are compiled once into libcore.a, which
# smirror and the unit tests link.
add_library(core STATIC
	bus.c)

# PUBLIC: core uses util, and so does anything that links core.
target_link_libraries(core PUBLIC util)
