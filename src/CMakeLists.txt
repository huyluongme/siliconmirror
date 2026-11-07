# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# One library per module, from the bottom layer up. See
# docs/architecture.md.
add_subdirectory(util)
add_subdirectory(core)
add_subdirectory(backend)
add_subdirectory(hw)
add_subdirectory(frontend)
