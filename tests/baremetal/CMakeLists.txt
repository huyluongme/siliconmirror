# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# AArch64 programs that run inside the emulator, through the smirror
# command line, and the check that the raspi3b boot stub's bytes are up
# to date. They are built with the GNU binutils for AArch64, the
# binutils-aarch64-linux-gnu package on Debian and Ubuntu. Without them,
# these tests are skipped, except on GitHub Actions: CI must run them,
# so there a missing tool is an error.

# The top-level CMakeLists.txt looks for the tools.
if(NOT AARCH64_BINUTILS_FOUND)
	# GitHub Actions sets GITHUB_ACTIONS to true in every workflow.
	if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
		message(FATAL_ERROR
			"AArch64 binutils not found: CI must run the bare-metal tests")
	endif()
	message(STATUS "AArch64 binutils not found: skipping bare-metal tests")
	return()
endif()

# Build <name>.bin from <name>.s: assemble, link at 0x80000, where the
# Raspberry Pi firmware loads kernel8.img, and keep the raw bytes.
function(add_baremetal_program name)
	set(source ${CMAKE_CURRENT_SOURCE_DIR}/${name}.s)
	set(output ${CMAKE_CURRENT_BINARY_DIR}/${name})
	add_custom_command(
		OUTPUT ${output}.bin
		COMMAND ${AARCH64_AS} -o ${output}.o ${source}
		COMMAND ${AARCH64_LD} -Ttext=0x80000 -o ${output}.elf ${output}.o
		COMMAND ${AARCH64_OBJCOPY} -O binary ${output}.elf ${output}.bin
		DEPENDS ${source}
		COMMENT "Building bare-metal program ${name}.bin")
	add_custom_target(baremetal_${name} ALL DEPENDS ${output}.bin)
endfunction()

add_baremetal_program(hello)
add_baremetal_program(unimplemented)
add_baremetal_program(exceptions)
add_baremetal_program(boot)
add_baremetal_program(timer)
add_baremetal_program(irq)
add_baremetal_program(uart_irq)
add_baremetal_program(echo)
add_baremetal_program(mmu)
add_baremetal_program(miniuart)
add_baremetal_program(mailbox)
add_baremetal_program(sdboot)
add_baremetal_program(sdcard)

set(smirror $<TARGET_FILE:smirror>)

# The message goes to standard output, why the machine stopped to
# standard error.
add_expect_test(baremetal.hello
	STDOUT "^Hello, world!\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -kernel hello.bin)

add_expect_test(baremetal.hello-serial-null
	STDOUT "^$"
	STDERR "^smirror: idle: "
	COMMAND ${smirror} -M raspi3b -kernel hello.bin -serial null)

# Write to a file, then compare it with the expected output. The fixture
# makes the second test run after the first.
add_expect_test(baremetal.hello-serial-file
	STDOUT "^$"
	COMMAND ${smirror} -M raspi3b -kernel hello.bin -serial file:hello.out)
add_test(NAME baremetal.hello-serial-file-contents
	COMMAND ${CMAKE_COMMAND} -E compare_files hello.out
		${CMAKE_CURRENT_SOURCE_DIR}/hello.expected)
set_tests_properties(baremetal.hello-serial-file PROPERTIES
	FIXTURES_SETUP hello_out)
set_tests_properties(baremetal.hello-serial-file-contents PROPERTIES
	FIXTURES_REQUIRED hello_out)

# Exceptions at EL2, EL1 and EL0, and the returns from them. The handler
# prints each one as "<Exception level>:<exception class>"; see the
# comments in exceptions.s.
add_expect_test(baremetal.exceptions
	STDOUT "^2:15\n2:3c\n2:00\n2:00\n1:15\n2:16\n1:25\n2:18\n1:15\n1:18\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -kernel exceptions.bin)

# A kernel starts in the state that the boot stub leaves the cores in,
# and releases the other cores through the spin table. See boot.s.
add_expect_test(baremetal.boot
	STDOUT "^boot state ok\n123\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -kernel boot.bin)

# The Generic Timer as the boot stub sets it up: its frequency, how fast
# the counts go, a timer and an event stream. See timer.s.
add_expect_test(baremetal.timer
	STDOUT "^timer ok\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -kernel timer.bin)

# Interrupts through the ARM local peripherals: the Generic Timer, the
# mailboxes, the local timer and the AXI outstanding interrupt, to IRQ
# and FIQ, masked and not, and from core 0 to core 1. See irq.s.
add_expect_test(baremetal.irq
	STDOUT "^irq ok\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -kernel irq.bin)

# The transmit interrupt of UART0, through the BCM2835 interrupt
# controller and the ARM local peripherals, to the IRQ and the FIQ of
# core 0. See uart_irq.s.
add_expect_test(baremetal.uart-irq
	STDOUT "^uart irq ok\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -kernel uart_irq.bin)

add_expect_test(baremetal.mmu
	STDOUT "^mmu ok\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -kernel mmu.bin)

# The console as U-Boot uses it: the mini UART on GPIO 14 and 15, as the
# firmware sets it up. What miniuart.input sends comes back. See
# miniuart.s.
add_expect_test(baremetal.miniuart
	STDOUT "^mini uart\nabc\nmini uart ok\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	INPUT ${CMAKE_CURRENT_SOURCE_DIR}/miniuart.input
	COMMAND ${smirror} -M raspi3b -kernel miniuart.bin)

# The firmware's answers on the property channel of the mailboxes, and
# the mailbox interrupt. See mailbox.s.
add_expect_test(baremetal.mailbox
	STDOUT "^mailbox ok\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -kernel mailbox.bin)

# Receiving on UART0: what echo.input sends comes back, through the
# receive timeout interrupt. See echo.s. As on the board, its first byte,
# '~', goes to the mini UART, which has GPIO 15 as the firmware set it up
# until echo.s gives the pin to the PL011.
add_expect_test(baremetal.echo
	STDOUT "^hello\necho ok\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	INPUT ${CMAKE_CURRENT_SOURCE_DIR}/echo.input
	COMMAND ${smirror} -M raspi3b -kernel echo.bin)

# On a terminal, smirror waits for keys while the kernel listens, and
# Ctrl-A x quits it. terminal.c drives smirror on a pseudo-terminal.
add_executable(terminal terminal.c)
add_test(NAME baremetal.terminal-quit
	COMMAND terminal $<TARGET_FILE:smirror> hello.bin "Hello, world!")

# smirror is built from src/machine/raspi3b_stub.inc; this build has
# assembled raspi3b_stub.s again (see src/machine/CMakeLists.txt). If
# they differ, run `make stub`.
add_test(NAME baremetal.raspi3b-stub-up-to-date
	COMMAND ${CMAKE_COMMAND} -E compare_files
		${PROJECT_BINARY_DIR}/src/machine/raspi3b_stub.inc
		${PROJECT_SOURCE_DIR}/src/machine/raspi3b_stub.inc)

# An instruction that is not implemented stops the machine with an error.
add_expect_test(baremetal.unimplemented
	STATUS 1
	STDERR "^smirror: error: cpu0: instruction 0x1e622820 at 0x0000000000080000 is not implemented yet"
	COMMAND ${smirror} -M raspi3b -kernel unimplemented.bin)

# Boot from SD card images that sfdisk, mkfs.fat and mtools make, with a
# device tree that dtc compiles, as on a real card. See sdboot.s and
# sdcard.cmake. Without the tools these tests are skipped, except on
# GitHub Actions.
if(NOT SD_TOOLS_FOUND)
	if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
		message(FATAL_ERROR "SD card tools not found: CI must run the tests "
			"that boot from a card")
	endif()
	message(STATUS "SD card tools not found: skipping the SD card tests")
	return()
endif()

set(card_dir ${CMAKE_CURRENT_SOURCE_DIR}/sdcard)
add_custom_command(
	OUTPUT board.dtb
	COMMAND ${DTC} -I dts -O dtb -o board.dtb ${card_dir}/board.dts
	DEPENDS ${card_dir}/board.dts
	COMMENT "Compiling the device tree of the test cards")

# make_card(<image> <size in MB> <FAT bits> FILES <src=dest>... [DIRS ...])
function(make_card image size fat)
	cmake_parse_arguments(PARSE_ARGV 3 arg "" "" "FILES;DIRS")
	set(sources)
	foreach(pair ${arg_FILES})
		string(REGEX REPLACE "=.*" "" src "${pair}")
		list(APPEND sources ${src})
	endforeach()
	string(REPLACE ";" "\\;" files "${arg_FILES}")
	string(REPLACE ";" "\\;" dirs "${arg_DIRS}")
	add_custom_command(
		OUTPUT ${image}
		COMMAND ${CMAKE_COMMAND} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${image}
			-DSIZE_MB=${size} -DFAT=${fat} "-DFILES=${files}" "-DDIRS=${dirs}"
			-DTRUNCATE=${TRUNCATE} -DSFDISK=${SFDISK} -DMKFS_FAT=${MKFS_FAT}
			-DMCOPY=${MCOPY} -DMMD=${MMD}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/sdcard.cmake
		DEPENDS ${sources} ${CMAKE_CURRENT_SOURCE_DIR}/sdcard.cmake
		COMMENT "Making the SD card image ${image}")
	add_custom_target(card_${image} ALL DEPENDS ${image})
endfunction()

set(bin ${CMAKE_CURRENT_BINARY_DIR})
make_card(sdboot-fat32.img 64 32
	FILES ${card_dir}/config-fat32.txt=config.txt
		${card_dir}/cmdline-fat32.txt=cmdline.txt
		${bin}/sdboot.bin=kernel8.img
		${bin}/board.dtb=bcm2710-rpi-3-b.dtb
		${card_dir}/board.dts=overlays/README
	DIRS overlays)
make_card(sdboot-fat16.img 16 16
	FILES ${card_dir}/config-fat16.txt=config.txt
		${card_dir}/cmdline-fat16.txt=command-line.txt
		${bin}/sdboot.bin=images/sd-boot-kernel.bin
		${bin}/board.dtb=dtbs/raspberry-pi-3.dtb
	DIRS images dtbs)

# config.txt filters, a setting that is not applied, cmdline.txt, and the
# device tree on a FAT32 card like Raspberry Pi OS's.
add_expect_test(baremetal.sd-boot-fat32
	STDOUT "^cmdline: console=serial0,115200 smirror\n\
sd boot ok\n$"
	STDERR "^smirror: warning: config.txt:7: dtoverlay=vc4-kms-v3d is not \
applied\nsmirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -sd sdboot-fat32.img)

# Long names in directories, on FAT16.
add_expect_test(baremetal.sd-boot-fat16
	STDOUT "^cmdline: quiet smirror-fat16\nsd boot ok\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -sd sdboot-fat16.img)

make_card(sdcard-fat32.img 64 32
	FILES ${card_dir}/config-sdcard.txt=config.txt
		${bin}/sdcard.bin=kernel8.img
		${bin}/board.dtb=bcm2710-rpi-3-b.dtb)

# The kernel reads and writes the card it came from, through SDHOST, as
# U-Boot does. See sdcard.s.
add_expect_test(baremetal.sd-card
	STDOUT "^sd card ok\n$"
	STDERR "^smirror: idle: every CPU is waiting and no event is pending\n$"
	COMMAND ${smirror} -M raspi3b -sd sdcard-fat32.img)
