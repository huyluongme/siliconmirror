# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Host resources used by devices. See docs/architecture.md.
add_library(backend STATIC
	char/file.c)

target_link_libraries(backend PUBLIC core)
