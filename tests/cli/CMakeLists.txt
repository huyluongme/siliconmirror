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
