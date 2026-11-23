# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Command-line front end.

add_test(NAME cli.version COMMAND smirror -version)
add_test(NAME cli.version-double-dash COMMAND smirror --version)
set_tests_properties(cli.version cli.version-double-dash PROPERTIES
	PASS_REGULAR_EXPRESSION "^smirror ${PROJECT_VERSION}\n")

add_test(NAME cli.help COMMAND smirror -help)
add_test(NAME cli.help-short COMMAND smirror -h)
set_tests_properties(cli.help cli.help-short PROPERTIES
	PASS_REGULAR_EXPRESSION "^Usage: smirror ")

add_test(NAME cli.unknown-option COMMAND smirror -no-such-option)
add_test(NAME cli.unexpected-argument COMMAND smirror file.img)
add_test(NAME cli.no-arguments COMMAND smirror)
set_tests_properties(
	cli.unknown-option cli.unexpected-argument cli.no-arguments
	PROPERTIES WILL_FAIL TRUE)

# Machines.
set(smirror $<TARGET_FILE:smirror>)

add_expect_test(cli.machine-help
	STDOUT "^Supported machines are:\nraspi3b +Raspberry Pi 3 Model B"
	COMMAND ${smirror} -M help)

add_expect_test(cli.machine-unknown
	STATUS 1
	STDERR "^smirror: unknown machine 'nosuch' \\(-M help lists them\\)\n$"
	COMMAND ${smirror} -M nosuch)

add_expect_test(cli.machine-missing
	STATUS 1
	STDERR "^smirror: no machine given"
	COMMAND ${smirror} -kernel image)

add_expect_test(cli.nothing-to-boot
	STATUS 1
	STDERR "^smirror: raspi3b: no SD card and no -kernel: the board has nothing to boot from\n$"
	COMMAND ${smirror} -M raspi3b)

add_expect_test(cli.sd-not-found
	STATUS 1
	STDERR "^smirror: cannot open no/such/card.img: "
	COMMAND ${smirror} -M raspi3b -sd no/such/card.img)

add_expect_test(cli.kernel-not-found
	STATUS 1
	STDERR "^smirror: raspi3b: no/such/file: "
	COMMAND ${smirror} -M raspi3b -kernel no/such/file)

add_expect_test(cli.option-needs-value
	STATUS 1
	STDERR "^smirror: option '-kernel' needs a value\n$"
	COMMAND ${smirror} -M raspi3b -kernel)

add_expect_test(cli.serial-unknown
	STATUS 1
	STDERR "^smirror: unknown -serial device 'bogus'\n$"
	COMMAND ${smirror} -M raspi3b -serial bogus)

# The card's image is closed when the command line fails after -sd.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/empty-card.img "")
add_expect_test(cli.sd-then-serial-unknown
	STATUS 1
	STDERR "^smirror: unknown -serial device 'bogus'\n$"
	COMMAND ${smirror} -M raspi3b -sd ${CMAKE_CURRENT_BINARY_DIR}/empty-card.img
		-serial bogus)

add_expect_test(cli.clock-unknown
	STATUS 1
	STDERR "^smirror: unknown clock 'bogus': use virtual or host\n$"
	COMMAND ${smirror} -M raspi3b -clock bogus)

add_expect_test(cli.serial-twice
	STATUS 1
	STDERR "^smirror: only one -serial is supported\n$"
	COMMAND ${smirror} -M raspi3b -serial null -serial null)

add_expect_test(cli.serial-file-not-writable
	STATUS 1
	STDERR "^smirror: no/such/dir/out.txt: "
	COMMAND ${smirror} -M raspi3b -serial file:no/such/dir/out.txt)

# -audio: what is plugged into the board's jack.
add_expect_test(cli.audio-unknown
	STATUS 1
	STDERR "^smirror: unknown -audio device 'bogus'\n$"
	COMMAND ${smirror} -M raspi3b -audio bogus)

add_expect_test(cli.audio-wave
	STATUS 1
	STDERR "^smirror: unknown -audio device 'wave:x'\n$"
	COMMAND ${smirror} -M raspi3b -audio wave:x)

add_expect_test(cli.audio-wav-not-writable
	STATUS 1
	STDERR "^smirror: cannot create no/such/dir/out.wav: [^\n]*\n$"
	COMMAND ${smirror} -M raspi3b -audio wav:no/such/dir/out.wav)

# The card's image and the serial console are closed when -audio fails.
add_expect_test(cli.sd-then-audio-unknown
	STATUS 1
	STDERR "^smirror: unknown -audio device 'alsa-'\n$"
	COMMAND ${smirror} -M raspi3b -sd ${CMAKE_CURRENT_BINARY_DIR}/empty-card.img
		-serial file:${CMAKE_CURRENT_BINARY_DIR}/serial.txt -audio alsa-)

# The board owns the output, and closes it when it cannot boot.
add_expect_test(cli.audio-wav-nothing-to-boot
	STATUS 1
	STDERR "^smirror: raspi3b: no SD card and no -kernel: "
	COMMAND ${smirror} -M raspi3b -audio wav:${CMAKE_CURRENT_BINARY_DIR}/out.wav)

add_expect_test(cli.audio-none
	STATUS 1
	STDERR "^smirror: raspi3b: no SD card and no -kernel: "
	COMMAND ${smirror} -M raspi3b -audio none)

# -net: the other end of the board's Ethernet cable.
add_expect_test(cli.net-unknown
	STATUS 1
	STDERR "^smirror: unknown -net device 'socket'\n$"
	COMMAND ${smirror} -M raspi3b -net socket)

# The user network opens without root; the board then wants its card.
add_expect_test(cli.net-user
	STATUS 1
	STDERR "^smirror: raspi3b: no SD card and no -kernel: "
	COMMAND ${smirror} -M raspi3b -net user)

add_expect_test(cli.net-user-option
	STATUS 1
	STDERR "^smirror: -net: user: unknown option 'restrict=on'\n$"
	COMMAND ${smirror} -M raspi3b -net user,restrict=on)

add_expect_test(cli.net-user-no-comma
	STATUS 1
	STDERR "^smirror: unknown -net device 'users'\n$"
	COMMAND ${smirror} -M raspi3b -net users)

add_expect_test(cli.net-pcap-not-writable
	STATUS 1
	STDERR "^smirror: -net: cannot create no/such/dir/out.pcap: [^\n]*\n$"
	COMMAND ${smirror} -M raspi3b -net pcap:no/such/dir/out.pcap)

add_expect_test(cli.net-tap-bad-name
	STATUS 1
	STDERR "^smirror: -net: '' is not an interface name\n$"
	COMMAND ${smirror} -M raspi3b -net tap:)

add_expect_test(cli.net-tap-long-name
	STATUS 1
	STDERR "^smirror: -net: '0123456789abcdef' is not an interface name\n$"
	COMMAND ${smirror} -M raspi3b -net tap:0123456789abcdef)

add_expect_test(cli.net-pcap-no-colon
	STATUS 1
	STDERR "^smirror: unknown -net device 'pcap/no/such/x.pcap'\n$"
	COMMAND ${smirror} -M raspi3b -net pcap/no/such/x.pcap)

# The card, the serial console and the audio output are closed when -net
# fails.
add_expect_test(cli.sd-audio-then-net-unknown
	STATUS 1
	STDERR "^smirror: unknown -net device 'tap'\n$"
	COMMAND ${smirror} -M raspi3b -sd ${CMAKE_CURRENT_BINARY_DIR}/empty-card.img
		-serial file:${CMAKE_CURRENT_BINARY_DIR}/serial.txt
		-audio wav:${CMAKE_CURRENT_BINARY_DIR}/out.wav -net tap)

# The board owns the link, and closes it when it cannot boot.
add_expect_test(cli.net-pcap-nothing-to-boot
	STATUS 1
	STDERR "^smirror: raspi3b: no SD card and no -kernel: "
	COMMAND ${smirror} -M raspi3b -net pcap:${CMAKE_CURRENT_BINARY_DIR}/out.pcap)

add_expect_test(cli.net-none
	STATUS 1
	STDERR "^smirror: raspi3b: no SD card and no -kernel: "
	COMMAND ${smirror} -M raspi3b -net none)

# ALSA's own message comes first; smirror's says which device.
find_package(ALSA QUIET)
if(ALSA_FOUND)
	add_expect_test(cli.audio-alsa-unknown
		STATUS 1
		STDERR "smirror: cannot open ALSA device smirror_no_such_pcm: [^\n]*\n$"
		COMMAND ${smirror} -M raspi3b -audio alsa:smirror_no_such_pcm)
else()
	add_expect_test(cli.audio-alsa-not-built
		STATUS 1
		STDERR "^smirror: -audio alsa: this smirror was built without ALSA\n$"
		COMMAND ${smirror} -M raspi3b -audio alsa)
endif()
