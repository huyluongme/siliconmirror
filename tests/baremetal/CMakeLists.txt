# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# AArch64 programs that run inside the emulator, through the smirror
# command line. They are built with the GNU binutils for AArch64, the
# binutils-aarch64-linux-gnu package on Debian and Ubuntu. Without them,
# these tests are skipped, except on GitHub Actions: CI must run them,
# so there a missing tool is an error.

find_program(AARCH64_AS NAMES aarch64-linux-gnu-as aarch64-none-elf-as)
find_program(AARCH64_LD NAMES aarch64-linux-gnu-ld aarch64-none-elf-ld)
find_program(AARCH64_OBJCOPY
	NAMES aarch64-linux-gnu-objcopy aarch64-none-elf-objcopy)

if(NOT AARCH64_AS OR NOT AARCH64_LD OR NOT AARCH64_OBJCOPY)
	# GitHub Actions sets GITHUB_ACTIONS to true in every workflow.
	if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
		message(FATAL_ERROR
			"AArch64 binutils not found: CI must run the bare-metal tests")
	endif()
	message(STATUS "AArch64 binutils not found: skipping bare-metal tests")
	return()
endif()

# Build <name>.bin from <name>.s: assemble, link at 0x80000, where the
# Raspberry Pi firmware loads kernel8.img, and keep the raw bytes.
function(add_baremetal_program name)
	set(source ${CMAKE_CURRENT_SOURCE_DIR}/${name}.s)
	set(output ${CMAKE_CURRENT_BINARY_DIR}/${name})
	add_custom_command(
		OUTPUT ${output}.bin
		COMMAND ${AARCH64_AS} -o ${output}.o ${source}
		COMMAND ${AARCH64_LD} -Ttext=0x80000 -o ${output}.elf ${output}.o
		COMMAND ${AARCH64_OBJCOPY} -O binary ${output}.elf ${output}.bin
		DEPENDS ${source}
		COMMENT "Building bare-metal program ${name}.bin")
	add_custom_target(baremetal_${name} ALL DEPENDS ${output}.bin)
endfunction()

add_baremetal_program(hello)
add_baremetal_program(unimplemented)

set(smirror $<TARGET_FILE:smirror>)

# The message goes to standard output, why the machine stopped to
# standard error.
add_expect_test(baremetal.hello
	STDOUT "^Hello, world!\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -kernel hello.bin)

add_expect_test(baremetal.hello-serial-null
	STDOUT "^$"
	STDERR "^smirror: idle: "
	COMMAND ${smirror} -M raspi3b -kernel hello.bin -serial null)

# Write to a file, then compare it with the expected output. The fixture
# makes the second test run after the first.
add_expect_test(baremetal.hello-serial-file
	STDOUT "^$"
	COMMAND ${smirror} -M raspi3b -kernel hello.bin -serial file:hello.out)
add_test(NAME baremetal.hello-serial-file-contents
	COMMAND ${CMAKE_COMMAND} -E compare_files hello.out
		${CMAKE_CURRENT_SOURCE_DIR}/hello.expected)
set_tests_properties(baremetal.hello-serial-file PROPERTIES
	FIXTURES_SETUP hello_out)
set_tests_properties(baremetal.hello-serial-file-contents PROPERTIES
	FIXTURES_REQUIRED hello_out)

# An instruction that is not implemented stops the machine with an error.
add_expect_test(baremetal.unimplemented
	STATUS 1
	STDERR "^smirror: error: cpu0: instruction 0x1e622820 at 0x0000000000080000 is not implemented yet"
	COMMAND ${smirror} -M raspi3b -kernel unimplemented.bin)
