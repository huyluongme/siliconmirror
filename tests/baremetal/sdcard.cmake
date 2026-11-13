# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Make an SD card image with the tools that make real cards, so that
# smirror's MBR, FAT and devicetree code meet files it did not write:
#
#   cmake -DOUTPUT=card.img -DSIZE_MB=64 -DFAT=32
#         -DFILES="src1=dest1;src2=dir/dest2" -DDIRS="dir"
#         -DTRUNCATE=... -DSFDISK=... -DMKFS_FAT=... -DMCOPY=... -DMMD=...
#         -P sdcard.cmake
#
# The card has an MBR whose first partition starts at 1 MiB, as on a
# Raspberry Pi OS image, with a FAT16 or FAT32 file system of one sector
# a cluster (mkfs.fat -F, -s 1). bootcode.bin and start.elf are added
# empty: smirror never runs them, but the board needs them to boot.

function(run)
	execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
		OUTPUT_VARIABLE out ERROR_VARIABLE err)
	if(NOT status EQUAL 0)
		message(FATAL_ERROR "${ARGN}: ${status}\n${out}${err}")
	endif()
endfunction()

get_filename_component(dir ${OUTPUT} DIRECTORY)
set(work ${OUTPUT}.files)
file(REMOVE_RECURSE ${work})
file(MAKE_DIRECTORY ${work})
file(REMOVE ${OUTPUT})
run(${TRUNCATE} -s ${SIZE_MB}M ${OUTPUT})

# One partition from sector 2048 to the end; type 0x0c is FAT32 with LBA.
file(WRITE ${work}/table "label: dos\nstart=2048, type=c\n")
execute_process(COMMAND ${SFDISK} --quiet ${OUTPUT}
	INPUT_FILE ${work}/table RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
	message(FATAL_ERROR "sfdisk: ${err}")
endif()

# mkfs.fat counts the size in blocks of 1 KiB.
math(EXPR blocks "${SIZE_MB} * 1024 - 1024")
run(${MKFS_FAT} -F ${FAT} -s 1 -n BOOT --offset 2048 ${OUTPUT} ${blocks})

# mtools reach the partition at its byte offset; MTOOLS_SKIP_CHECK lets
# them use a file system that no floppy geometry describes.
set(mtools ${CMAKE_COMMAND} -E env MTOOLS_SKIP_CHECK=1)
set(fs "${OUTPUT}@@1M")
foreach(d ${DIRS})
	run(${mtools} ${MMD} -i ${fs} ::${d})
endforeach()
file(WRITE ${work}/empty "")
list(APPEND FILES "${work}/empty=bootcode.bin" "${work}/empty=start.elf")
foreach(pair ${FILES})
	string(FIND "${pair}" "=" eq REVERSE)
	string(SUBSTRING "${pair}" 0 ${eq} src)
	math(EXPR rest "${eq} + 1")
	string(SUBSTRING "${pair}" ${rest} -1 dest)
	run(${mtools} ${MCOPY} -i ${fs} ${src} ::${dest})
endforeach()
file(REMOVE_RECURSE ${work})
