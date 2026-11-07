# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Run a command and check how it ends: its exit status, and its standard
# output and standard error, each on its own. CTest alone only checks
# both outputs together. Use it through add_expect_test() in
# tests/CMakeLists.txt, or directly:
#
#   cmake "-DCOMMAND=<program>;<argument>;..." [-DSTATUS=<status>]
#         [-DSTDOUT=<regex>] [-DSTDERR=<regex>] -P expect.cmake
#
# STATUS is 0 when not given; STDOUT and STDERR are only checked when
# given.

if(NOT DEFINED STATUS)
	set(STATUS 0)
endif()

execute_process(COMMAND ${COMMAND}
	RESULT_VARIABLE status
	OUTPUT_VARIABLE stdout
	ERROR_VARIABLE stderr)

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
