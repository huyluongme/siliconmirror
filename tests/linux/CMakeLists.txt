# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# A Linux session on the raspi3b (session.txt), on a copy of the card
# that scripts/linux-card.sh makes, through drive (tests/uboot). It
# takes minutes, two boots of Linux, so only optimized builds have it;
# without the card it is skipped.
set(SMIRROR_LINUX_CARD "$ENV{HOME}/.cache/siliconmirror-linux/card.img"
	CACHE FILEPATH "The SD card of the Linux test (scripts/linux-card.sh)")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
	set(card ${CMAKE_CURRENT_BINARY_DIR}/card.img)
	add_test(NAME linux.session
		COMMAND drive ${SMIRROR_LINUX_CARD} ${card}
			${CMAKE_CURRENT_SOURCE_DIR}/session.txt
			$<TARGET_FILE:smirror> -M raspi3b -sd ${card})
	set_tests_properties(linux.session PROPERTIES
		SKIP_RETURN_CODE 77
		TIMEOUT 3600
		LABELS linux)
endif()
