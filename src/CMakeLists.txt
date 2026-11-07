# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Headers are included by their path from src/, for example
# #include "core/bus.h". See docs/architecture.md.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(frontend)
