# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Check src/util/fdt.c against dtc and fdtget, which smirror did not
# write:
#
#   cmake -DDTC=... -DFDTGET=... -DTOOL=fdt_tool -DDTS=board.dts
#         -DWORK=dir -P fdt_dtc.cmake
#
# 1. A tree that fdt.c reads and writes back unchanged decompiles to the
#    same source as the tree that dtc compiled.
# 2. The firmware's changes, made with fdt.c, read back with fdtget.

function(run)
	execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
		OUTPUT_VARIABLE out ERROR_VARIABLE err)
	if(NOT status EQUAL 0)
		message(FATAL_ERROR "${ARGN}: ${status}\n${out}${err}")
	endif()
	set(out "${out}" PARENT_SCOPE)
endfunction()

# fdtget's answer for a property, as a check.
function(expect type node property expected)
	run(${FDTGET} -t ${type} ${WORK}/edited.dtb ${node} ${property})
	string(STRIP "${out}" out)
	if(NOT out STREQUAL expected)
		message(FATAL_ERROR
			"${node} ${property} is \"${out}\", expected \"${expected}\"")
	endif()
endfunction()

file(MAKE_DIRECTORY ${WORK})
run(${DTC} -q -I dts -O dtb -o ${WORK}/in.dtb ${DTS})
run(${TOOL} copy ${WORK}/in.dtb ${WORK}/copy.dtb)
run(${DTC} -q -I dtb -O dts ${WORK}/in.dtb)
set(original "${out}")
run(${DTC} -q -I dtb -O dts ${WORK}/copy.dtb)
if(NOT out STREQUAL original)
	message(FATAL_ERROR "the copy differs:\n${original}\n---\n${out}")
endif()

run(${TOOL} edit ${WORK}/in.dtb ${WORK}/edited.dtb)
run(${DTC} -q -I dtb -O dts ${WORK}/edited.dtb) # dtc accepts the blob
expect(s /chosen bootargs "coherent_pool=1M quiet")
expect(x /chosen linux,initrd-start "81000")
expect(x /memory@0 reg "0 3b400000")
expect(bx /soc/usb@7e980000/usb1@1/ethernet@1 local-mac-address
	"b8 27 eb ff ee 3")
expect(x /system linux,revision "a02082")
expect(s / model "Raspberry Pi 3 Model B")
