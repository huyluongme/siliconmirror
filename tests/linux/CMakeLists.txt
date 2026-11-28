# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# A Linux session on the raspi3b (session.txt), on a copy of the card
# that scripts/linux-card.sh makes, through drive (tests/uboot). It
# takes minutes, two boots of Linux, so only optimized builds have it;
# without the card it is skipped. Each test runs in each mode of -jit
# (linux.session and, with the JIT, linux.session-jit), each on its own
# copy of the card; the two of linux.user take turns, as they forward
# the same ports of the host.

set(SMIRROR_LINUX_CARD "$ENV{HOME}/.cache/siliconmirror-linux/card.img"
	CACHE FILEPATH "The SD card of the Linux test (scripts/linux-card.sh)")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
	add_executable(net_check net_check.c)
	target_link_libraries(net_check PRIVATE util)

	foreach(jit ${SMIRROR_TEST_JIT_MODES})
		if(jit STREQUAL "on")
			set(s -jit)
		else()
			set(s "")
		endif()
		set(bin ${CMAKE_CURRENT_BINARY_DIR})
		set(smirror $<TARGET_FILE:smirror> -M raspi3b -jit ${jit})

		add_test(NAME linux.session${s}
			COMMAND drive ${SMIRROR_LINUX_CARD} ${bin}/card${s}.img
				${CMAKE_CURRENT_SOURCE_DIR}/session.txt
				${smirror} -sd ${bin}/card${s}.img)

		# The cable into a capture (net.txt), and what Linux sent
		# (net_check).
		set(pcap ${bin}/net${s}.pcap)
		add_test(NAME linux.net${s}
			COMMAND drive ${SMIRROR_LINUX_CARD} ${bin}/net-card${s}.img
				${CMAKE_CURRENT_SOURCE_DIR}/net.txt
				${smirror} -sd ${bin}/net-card${s}.img -net pcap:${pcap})

		# The user network (user.txt): DHCP, ping, a name through the
		# host, and TCP out through the host's loopback and back in by a
		# forward.
		add_test(NAME linux.user${s}
			COMMAND drive ${SMIRROR_LINUX_CARD} ${bin}/user-card${s}.img
				${CMAKE_CURRENT_SOURCE_DIR}/user.txt
				${smirror} -sd ${bin}/user-card${s}.img
				-net user,hostfwd=tcp::10007-:7,hostfwd=tcp::10009-:9)

		set_tests_properties(linux.session${s} linux.net${s} linux.user${s}
			PROPERTIES
			SKIP_RETURN_CODE 77
			TIMEOUT 3600
			LABELS linux)
		set_tests_properties(linux.user${s} PROPERTIES
			RESOURCE_LOCK host-ports-10007-10009)

		add_test(NAME linux.net_check${s}
			COMMAND net_check ${SMIRROR_LINUX_CARD} ${pcap})
		set_tests_properties(linux.net_check${s} PROPERTIES
			DEPENDS linux.net${s}
			SKIP_RETURN_CODE 77
			LABELS linux)
	endforeach()
endif()
