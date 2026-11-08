# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Turn a raw binary of little-endian 32-bit words, such as an assembled
# boot stub, into the lines of a C array initializer, to #include
# between the braces of a uint32_t array:
#
#   cmake -DINPUT=stub.bin -DOUTPUT=stub.inc -DSOURCE=stub.s -P bin2inc.cmake
#
# SOURCE is the name of the file that INPUT was built from; it goes in
# the comment at the top of OUTPUT.

foreach(var INPUT OUTPUT SOURCE)
	if(NOT DEFINED ${var})
		message(FATAL_ERROR "bin2inc.cmake: ${var} is not set")
	endif()
endforeach()

# HEX reads the file as a string of hexadecimal digits, two per byte.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR remainder "${digits} % 8")
if(digits EQUAL 0 OR NOT remainder EQUAL 0)
	message(FATAL_ERROR "${INPUT}: the size is not a multiple of 4 bytes")
endif()

set(text "/* SPDX-License-Identifier: GPL-2.0-or-later */\n")
string(APPEND text "/* Generated from ${SOURCE} by `make stub`: do not edit. */\n")

# Four words per line, followed by the offset of the first one.
math(EXPR last "${digits} - 8")
foreach(start RANGE 0 ${last} 8)
	math(EXPR column "${start} / 8 % 4")
	if(column EQUAL 0)
		string(APPEND text "\t")
	else()
		string(APPEND text " ")
	endif()

	# The bytes come lowest address first: reverse them into a word.
	set(word "")
	foreach(byte 0 1 2 3)
		math(EXPR at "${start} + ${byte} * 2")
		string(SUBSTRING "${hex}" ${at} 2 digit_pair)
		set(word "${digit_pair}${word}")
	endforeach()
	string(APPEND text "0x${word},")

	if(column EQUAL 3 OR start EQUAL last)
		math(EXPR offset "${start} / 2 - ${column} * 4")
		math(EXPR offset "${offset}" OUTPUT_FORMAT HEXADECIMAL)
		string(APPEND text " /* ${offset} */\n")
	endif()
endforeach()

file(WRITE "${OUTPUT}" "${text}")
