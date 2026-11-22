# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# A Raspberry Pi OS session on the raspi3b (session.txt), on a copy of
# the card that scripts/raspios-card.sh makes, through drive
# (tests/uboot). It takes about ten minutes, two boots of Raspberry Pi
# OS, so only optimized builds have it; without the card it is skipped.
set(SMIRROR_RASPIOS_CARD "$ENV{HOME}/.cache/siliconmirror-raspios/card.img"
	CACHE FILEPATH "The SD card of the Raspberry Pi OS test (scripts/raspios-card.sh)")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
	set(card ${CMAKE_CURRENT_BINARY_DIR}/card.img)
	add_test(NAME raspios.session
		COMMAND drive ${SMIRROR_RASPIOS_CARD} ${card}
			${CMAKE_CURRENT_SOURCE_DIR}/session.txt
			$<TARGET_FILE:smirror> -M raspi3b -sd ${card})
	set_tests_properties(raspios.session PROPERTIES
		SKIP_RETURN_CODE 77
		TIMEOUT 5400
		LABELS raspios)
endif()
