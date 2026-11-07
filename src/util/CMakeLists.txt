# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# util has only headers for now, so it is an INTERFACE library: it
# compiles nothing and only passes settings on to whatever links it.
add_library(util INTERFACE)

# Anything that links util, directly or through another module, can
# include headers by their path from src/, for example "core/bus.h".
target_include_directories(util INTERFACE ${PROJECT_SOURCE_DIR}/src)
