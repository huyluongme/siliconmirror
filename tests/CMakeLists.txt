# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# add_expect_test(<name> [STATUS <status>] [STDOUT <regex>]
#                 [STDERR <regex>] [INPUT <file>]
#                 COMMAND <program> <argument>...)
#
# A test that runs the command through expect.cmake, which checks the
# exit status (0 by default), and the standard output and standard error
# when a regular expression is given for them. The command reads its
# standard input from the file INPUT, or from /dev/null.
function(add_expect_test name)
	cmake_parse_arguments(PARSE_ARGV 1 arg ""
		"STATUS;STDOUT;STDERR;INPUT;MIN_SECONDS"
		"COMMAND")
	# The command is a list; escape its separators so that it stays one
	# -D argument.
	string(REPLACE ";" "\\;" command "${arg_COMMAND}")
	set(defines "-DCOMMAND=${command}")
	foreach(check STATUS STDOUT STDERR INPUT MIN_SECONDS)
		if(DEFINED arg_${check})
			list(APPEND defines "-D${check}=${arg_${check}}")
		endif()
	endforeach()
	add_test(NAME ${name}
		COMMAND ${CMAKE_COMMAND} ${defines}
			-P ${PROJECT_SOURCE_DIR}/tests/expect.cmake)
endfunction()

# Where the JIT runs (performance phase 3a): Linux x86-64 hosts. Elsewhere
# -jit on stops smirror with an error.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
	set(SMIRROR_TEST_JIT TRUE)
else()
	set(SMIRROR_TEST_JIT FALSE)
endif()

# The modes of -jit in which the sessions of tests/uboot, tests/linux and
# tests/raspios run: a test <name> with -jit off, and <name>-jit with -jit on.
set(SMIRROR_TEST_JIT_MODES off)
if(SMIRROR_TEST_JIT)
	list(APPEND SMIRROR_TEST_JIT_MODES on)
endif()

# add_guest_test(<name> <the arguments of add_expect_test>...)
#
# A test of guest code, which must give the same in both modes: as
# add_expect_test() with the interpreter, and where the JIT runs, again as
# <name>-jit with -jit on added to the command. The two take turns, as
# they use the same files, such as an SD card that smirror locks.
function(add_guest_test name)
	add_expect_test(${name} ${ARGN})
	if(SMIRROR_TEST_JIT)
		add_expect_test(${name}-jit ${ARGN} -jit on)
		set_tests_properties(${name} ${name}-jit PROPERTIES
			RESOURCE_LOCK ${name})
	endif()
endfunction()

add_subdirectory(unit)
add_subdirectory(cli)
add_subdirectory(baremetal)
add_subdirectory(uboot)
add_subdirectory(linux)
add_subdirectory(raspios)

# Each module under src/ may only include headers from the modules below
# it. See docs/architecture.md, "Layers".
add_test(NAME layering
	COMMAND sh ${PROJECT_SOURCE_DIR}/scripts/check-layering.sh
		${PROJECT_SOURCE_DIR}/src)
