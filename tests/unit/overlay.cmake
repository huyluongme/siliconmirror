# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Check src/boot/rpi_overlay.c on trees that dtc compiles, with fdtget
# reading the result; smirror wrote neither:
#
#   cmake -DDTC=... -DFDTGET=... -DTOOL=overlay_tool -DSRC=overlay
#         -DCASE=name.case -DWORK=dir -P overlay.cmake
#
# SRC holds base.dts, compiled with -@ as the firmware's trees are (and
# without it as plain.dtb, which has no __symbols__), NAME-overlay.dts
# for each overlay NAME.dtbo, and overlay_map.dts. A case file has a
# command a line, # starts a comment. Before each check, the lines so
# far apply to the base, and the warnings so far must be those:
#
#   base plain                the base without __symbols__
#   line TEXT                 a line of config.txt: dtoverlay= or dtparam=
#   warn TEXT                 the next warning; there must be no others
#   expect NODE PROP TYPE V   fdtget -t TYPE gives V
#   absent NODE [PROP]        the property, or the node, is not there
#   props NODE NAME...        the node's properties, in their order
#   nodes NODE NAME...        the node's children, in their order

function(run)
	execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
		OUTPUT_VARIABLE out ERROR_VARIABLE err)
	if(NOT status EQUAL 0)
		message(FATAL_ERROR "${ARGN}: ${status}\n${out}${err}")
	endif()
	set(out "${out}" PARENT_SCOPE)
endfunction()

# fdtget's answer, its lines joined by spaces; ok is false if it failed.
function(get)
	execute_process(COMMAND ${FDTGET} ${ARGN} RESULT_VARIABLE status
		OUTPUT_VARIABLE out ERROR_QUIET)
	string(STRIP "${out}" out)
	string(REPLACE "\n" " " out "${out}")
	set(out "${out}" PARENT_SCOPE)
	if(status EQUAL 0)
		set(ok TRUE PARENT_SCOPE)
	else()
		set(ok FALSE PARENT_SCOPE)
	endif()
endfunction()

file(MAKE_DIRECTORY ${WORK}/ov)
run(${DTC} -@ -q -I dts -O dtb -o ${WORK}/base.dtb ${SRC}/base.dts)
run(${DTC} -q -I dts -O dtb -o ${WORK}/plain.dtb ${SRC}/base.dts)
run(${DTC} -q -I dts -O dtb -o ${WORK}/ov/overlay_map.dtb
	${SRC}/overlay_map.dts)
file(GLOB sources ${SRC}/*-overlay.dts)
foreach(source ${sources})
	get_filename_component(name ${source} NAME)
	string(REGEX REPLACE "-overlay\\.dts$" "" name ${name})
	run(${DTC} -@ -q -I dts -O dtb -o ${WORK}/ov/${name}.dtbo ${source})
endforeach()

# Apply the lines so far to the base, from the start, and check the
# warnings so far.
function(apply)
	run(${TOOL} ${base} ${WORK}/ov ${WORK}/out.dtb ${lines})
	if(NOT "${out}" STREQUAL "${warnings}")
		message(FATAL_ERROR "the warnings are:\n${out}expected:\n${warnings}")
	endif()
endfunction()

file(STRINGS ${CASE} commands)
set(base ${WORK}/base.dtb)
set(lines)
set(warnings "")
set(applied FALSE)
foreach(command ${commands})
	if(command MATCHES "^base plain$")
		set(base ${WORK}/plain.dtb)
		continue()
	elseif(command MATCHES "^line (.*)$")
		list(APPEND lines "${CMAKE_MATCH_1}")
		set(applied FALSE)
		continue()
	elseif(command MATCHES "^warn (.*)$")
		string(APPEND warnings "warning: ${CMAKE_MATCH_1}\n")
		set(applied FALSE)
		continue()
	elseif(command MATCHES "^(#.*|)$")
		continue()
	endif()
	if(NOT applied)
		apply()
		set(applied TRUE)
	endif()
	if(command MATCHES "^expect ([^ ]+) ([^ ]+) ([^ ]+) (.*)$")
		get(-t ${CMAKE_MATCH_3} ${WORK}/out.dtb ${CMAKE_MATCH_1}
			${CMAKE_MATCH_2})
		if(NOT ok OR NOT "${out}" STREQUAL "${CMAKE_MATCH_4}")
			message(FATAL_ERROR
				"${CMAKE_MATCH_1} ${CMAKE_MATCH_2} is \"${out}\", "
				"expected \"${CMAKE_MATCH_4}\"")
		endif()
	elseif(command MATCHES "^absent ([^ ]+) ([^ ]+)$")
		get(${WORK}/out.dtb ${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
		if(ok)
			message(FATAL_ERROR "${CMAKE_MATCH_1} has ${CMAKE_MATCH_2}")
		endif()
	elseif(command MATCHES "^absent ([^ ]+)$")
		get(-l ${WORK}/out.dtb ${CMAKE_MATCH_1})
		if(ok)
			message(FATAL_ERROR "there is a node ${CMAKE_MATCH_1}")
		endif()
	elseif(command MATCHES "^(props|nodes) ([^ ]+) (.*)$")
		if(CMAKE_MATCH_1 STREQUAL "props")
			set(option -p)
		else()
			set(option -l)
		endif()
		set(expected "${CMAKE_MATCH_3}")
		get(${option} ${WORK}/out.dtb ${CMAKE_MATCH_2})
		if(NOT ok OR NOT "${out}" STREQUAL "${expected}")
			message(FATAL_ERROR
				"${CMAKE_MATCH_2} has ${CMAKE_MATCH_1} \"${out}\", "
				"expected \"${expected}\"")
		endif()
	else()
		message(FATAL_ERROR "what is \"${command}\"?")
	endif()
endforeach()
if(NOT applied)
	apply()
endif()
