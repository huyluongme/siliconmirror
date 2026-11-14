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
