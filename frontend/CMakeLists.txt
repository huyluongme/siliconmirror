# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

add_executable(smirror main.c)

target_compile_definitions(smirror PRIVATE
	SMIRROR_VERSION="${PROJECT_VERSION}")

install(TARGETS smirror RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
