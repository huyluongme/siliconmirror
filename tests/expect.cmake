# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Run a command and check how it ends: its exit status, and its standard
# output and standard error, each on its own. CTest alone only checks
# both outputs together. Use it through add_expect_test() in
# tests/CMakeLists.txt, or directly:
#
#   cmake "-DCOMMAND=<program>;<argument>;..." [-DSTATUS=<status>]
#         [-DSTDOUT=<regex>] [-DSTDERR=<regex>] [-DINPUT=<file>]
#         [-DMIN_SECONDS=<seconds>] -P expect.cmake
#
# The command reads its standard input from INPUT, or from /dev/null, so
# that it never waits for a terminal.
#
# STATUS is 0 when not given; STDOUT and STDERR are only checked when
# given. MIN_SECONDS, when given, is how long the command must take at
# least on the host's clock, in whole seconds of it: a command that
# takes S seconds or more always sees S of them go by.

if(NOT DEFINED STATUS)
	set(STATUS 0)
endif()

if(NOT DEFINED INPUT)
	set(INPUT /dev/null)
endif()
string(TIMESTAMP start "%s" UTC)
execute_process(COMMAND ${COMMAND}
	INPUT_FILE ${INPUT}
	RESULT_VARIABLE status
	OUTPUT_VARIABLE stdout
	ERROR_VARIABLE stderr)
string(TIMESTAMP end "%s" UTC)

set(report "\n--- stdout:\n${stdout}\n--- stderr:\n${stderr}")
if(NOT status STREQUAL STATUS)
	message(FATAL_ERROR "exit status ${status}, expected ${STATUS}${report}")
endif()
if(DEFINED STDOUT AND NOT stdout MATCHES "${STDOUT}")
	message(FATAL_ERROR "stdout does not match \"${STDOUT}\"${report}")
endif()
if(DEFINED STDERR AND NOT stderr MATCHES "${STDERR}")
	message(FATAL_ERROR "stderr does not match \"${STDERR}\"${report}")
endif()
if(DEFINED MIN_SECONDS)
	math(EXPR took "${end} - ${start}")
	if(took LESS MIN_SECONDS)
		message(FATAL_ERROR
			"took ${took} s, expected ${MIN_SECONDS} s at least${report}")
	endif()
endif()
