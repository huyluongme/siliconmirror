# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Boards. See docs/architecture.md.

add_library(machine STATIC
	board.c
	raspi3b.c)

target_link_libraries(machine PUBLIC boot cpu hw core)

# The boot stub of the raspi3b board, raspi3b_stub.s, is AArch64 code
# that runs inside the emulator. smirror is built from its bytes kept in
# raspi3b_stub.inc, so that building smirror needs no AArch64 tools.
#
# With the AArch64 binutils, every build also assembles the stub into
# the build tree, as the bare-metal tests are built: assemble, link at
# address 0, keep the raw bytes, and write them as C words with
# scripts/bin2inc.cmake. `make stub` copies the result over
# raspi3b_stub.inc, and the test baremetal.raspi3b-stub-up-to-date
# fails while the two differ.
if(AARCH64_BINUTILS_FOUND)
	set(stub ${CMAKE_CURRENT_BINARY_DIR}/raspi3b_stub)
	# add_custom_command() says how to make a file: the build runs the
	# COMMANDs when the OUTPUT is missing or older than its DEPENDS.
	add_custom_command(
		OUTPUT ${stub}.inc
		COMMAND ${AARCH64_AS} -o ${stub}.o
			${CMAKE_CURRENT_SOURCE_DIR}/raspi3b_stub.s
		COMMAND ${AARCH64_LD} -Ttext=0 -o ${stub}.elf ${stub}.o
		COMMAND ${AARCH64_OBJCOPY} -O binary ${stub}.elf ${stub}.bin
		COMMAND ${CMAKE_COMMAND} -DINPUT=${stub}.bin -DOUTPUT=${stub}.inc
			-DSOURCE=raspi3b_stub.s
			-P ${PROJECT_SOURCE_DIR}/scripts/bin2inc.cmake
		DEPENDS raspi3b_stub.s ${PROJECT_SOURCE_DIR}/scripts/bin2inc.cmake
		COMMENT "Assembling the raspi3b boot stub")
	# A target that the default build (ALL) makes, which needs the file.
	add_custom_target(raspi3b_stub ALL DEPENDS ${stub}.inc)
	# `make stub`, through cmake --build --target stub.
	add_custom_target(stub
		COMMAND ${CMAKE_COMMAND} -E copy ${stub}.inc
			${CMAKE_CURRENT_SOURCE_DIR}/raspi3b_stub.inc
		DEPENDS ${stub}.inc
		COMMENT "Updating src/machine/raspi3b_stub.inc")
else()
	add_custom_target(stub
		COMMAND ${CMAKE_COMMAND} -E echo
			"make stub needs the AArch64 binutils (binutils-aarch64-linux-gnu)"
		COMMAND ${CMAKE_COMMAND} -E false)
endif()
