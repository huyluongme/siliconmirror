# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Helpers that need nothing else of smirror: mostly headers, and the
# devicetree code.
add_library(util STATIC
	crc32.c
	fdt.c
	inflate.c)

# Anything that links util, directly or through another module, can
# include headers by their path from src/, for example "core/bus.h".
# PUBLIC: util's own files and those that link it.
target_include_directories(util PUBLIC ${PROJECT_SOURCE_DIR}/src)
