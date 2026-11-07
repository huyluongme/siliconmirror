# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Boot methods. See docs/architecture.md.

add_library(boot STATIC
	direct.c)

target_link_libraries(boot PUBLIC core)
