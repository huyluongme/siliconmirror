# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# A U-Boot session on the raspi3b (session.txt), on a copy of the card
# that scripts/uboot-card.sh makes. It takes minutes, two boots of
# U-Boot, so only optimized builds have it; without the card it is
# skipped. It runs in each mode of -jit (uboot.session and, with the
# JIT, uboot.session-jit), each on its own copy of the card.

set(SMIRROR_UBOOT_CARD "$ENV{HOME}/.cache/siliconmirror-uboot/card.img"
	CACHE FILEPATH "The SD card of the U-Boot test (scripts/uboot-card.sh)")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
	add_executable(drive drive.c)
	foreach(jit ${SMIRROR_TEST_JIT_MODES})
		if(jit STREQUAL "on")
			set(s -jit)
		else()
			set(s "")
		endif()
		set(card ${CMAKE_CURRENT_BINARY_DIR}/card${s}.img)
		add_test(NAME uboot.session${s}
			COMMAND drive ${SMIRROR_UBOOT_CARD} ${card}
				${CMAKE_CURRENT_SOURCE_DIR}/session.txt
				$<TARGET_FILE:smirror> -M raspi3b -sd ${card} -jit ${jit})
		set_tests_properties(uboot.session${s} PROPERTIES
			SKIP_RETURN_CODE 77
			TIMEOUT 3600
			LABELS uboot)
	endforeach()
endif()
