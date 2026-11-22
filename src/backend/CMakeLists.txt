# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Host resources used by devices. See docs/architecture.md.
add_library(backend STATIC
	audio/wav.c
	block/file.c
	clock/host.c
	char/file.c
	char/stdio.c)

target_link_libraries(backend PUBLIC core)

# The host's sound card (-audio alsa), when ALSA's library is there; the
# Debian and Ubuntu package is libasound2-dev.
find_package(ALSA)
if(ALSA_FOUND)
	target_sources(backend PRIVATE audio/alsa.c)
	target_link_libraries(backend PUBLIC ALSA::ALSA)
	target_compile_definitions(backend PUBLIC SMIRROR_HAVE_ALSA)
else()
	message(STATUS "ALSA not found: smirror is built without -audio alsa")
endif()
