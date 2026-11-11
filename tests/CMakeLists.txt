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
	cmake_parse_arguments(PARSE_ARGV 1 arg "" "STATUS;STDOUT;STDERR;INPUT"
		"COMMAND")
	# The command is a list; escape its separators so that it stays one
	# -D argument.
	string(REPLACE ";" "\\;" command "${arg_COMMAND}")
	set(defines "-DCOMMAND=${command}")
	foreach(check STATUS STDOUT STDERR INPUT)
		if(DEFINED arg_${check})
			list(APPEND defines "-D${check}=${arg_${check}}")
		endif()
	endforeach()
	add_test(NAME ${name}
		COMMAND ${CMAKE_COMMAND} ${defines}
			-P ${PROJECT_SOURCE_DIR}/tests/expect.cmake)
endfunction()

add_subdirectory(unit)
add_subdirectory(cli)
add_subdirectory(baremetal)

# Each module under src/ may only include headers from the modules below
# it. See docs/architecture.md, "Layers".
add_test(NAME layering
	COMMAND sh ${PROJECT_SOURCE_DIR}/scripts/check-layering.sh
		${PROJECT_SOURCE_DIR}/src)
