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

	# The cable into a capture (net.txt), and what Linux sent (net_check).
	set(net_card ${CMAKE_CURRENT_BINARY_DIR}/net-card.img)
	set(pcap ${CMAKE_CURRENT_BINARY_DIR}/net.pcap)
	add_test(NAME linux.net
		COMMAND drive ${SMIRROR_LINUX_CARD} ${net_card}
			${CMAKE_CURRENT_SOURCE_DIR}/net.txt
			$<TARGET_FILE:smirror> -M raspi3b -sd ${net_card} -net pcap:${pcap})
	set_tests_properties(linux.net PROPERTIES
		SKIP_RETURN_CODE 77
		TIMEOUT 3600
		LABELS linux)
	# The user network (user.txt): DHCP, ping, a name through the host.
	set(user_card ${CMAKE_CURRENT_BINARY_DIR}/user-card.img)
	add_test(NAME linux.user
		COMMAND drive ${SMIRROR_LINUX_CARD} ${user_card}
			${CMAKE_CURRENT_SOURCE_DIR}/user.txt
			$<TARGET_FILE:smirror> -M raspi3b -sd ${user_card} -net user)
	set_tests_properties(linux.user PROPERTIES
		SKIP_RETURN_CODE 77
		TIMEOUT 3600
		LABELS linux)

	add_executable(net_check net_check.c)
	target_link_libraries(net_check PRIVATE util)
	add_test(NAME linux.net_check
		COMMAND net_check ${SMIRROR_LINUX_CARD} ${pcap})
	set_tests_properties(linux.net_check PROPERTIES
		DEPENDS linux.net
		SKIP_RETURN_CODE 77
		LABELS linux)
endif()
