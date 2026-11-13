# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Keep test programs in build/<preset>/tests/unit/, away from smirror.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# One program per module, linked against that module only.
add_executable(test_byteorder test_byteorder.c)
target_link_libraries(test_byteorder PRIVATE util)
add_test(NAME unit.byteorder COMMAND test_byteorder)

add_executable(test_bits test_bits.c)
target_link_libraries(test_bits PRIVATE util)
add_test(NAME unit.bits COMMAND test_bits)

add_executable(test_fdt test_fdt.c)
target_link_libraries(test_fdt PRIVATE util)
add_test(NAME unit.fdt COMMAND test_fdt)

# fdt.c against dtc and fdtget, when they are installed.
add_executable(fdt_tool fdt_tool.c)
target_link_libraries(fdt_tool PRIVATE util)
if(DTC AND FDTGET)
	add_test(NAME unit.fdt-dtc
		COMMAND ${CMAKE_COMMAND} -DDTC=${DTC} -DFDTGET=${FDTGET}
			-DTOOL=$<TARGET_FILE:fdt_tool>
			-DDTS=${PROJECT_SOURCE_DIR}/tests/baremetal/sdcard/board.dts
			-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fdt_dtc
			-P ${CMAKE_CURRENT_SOURCE_DIR}/fdt_dtc.cmake)
endif()

add_executable(test_bus test_bus.c)
target_link_libraries(test_bus PRIVATE core)
add_test(NAME unit.bus COMMAND test_bus)

add_executable(test_irq test_irq.c)
target_link_libraries(test_irq PRIVATE core)
add_test(NAME unit.irq COMMAND test_irq)

add_executable(test_event test_event.c)
target_link_libraries(test_event PRIVATE core)
add_test(NAME unit.event COMMAND test_event)

add_executable(test_machine test_machine.c)
target_link_libraries(test_machine PRIVATE core)
add_test(NAME unit.machine COMMAND test_machine)

add_executable(test_char_file test_char_file.c)
target_link_libraries(test_char_file PRIVATE backend)
add_test(NAME unit.char_file COMMAND test_char_file)

add_executable(test_char_stdio test_char_stdio.c)
target_link_libraries(test_char_stdio PRIVATE backend)
add_test(NAME unit.char_stdio COMMAND test_char_stdio)

add_executable(test_block_file test_block_file.c)
target_link_libraries(test_block_file PRIVATE backend)
add_test(NAME unit.block_file COMMAND test_block_file)

add_executable(test_bcm2835_gpio test_bcm2835_gpio.c)
target_link_libraries(test_bcm2835_gpio PRIVATE hw)
add_test(NAME unit.bcm2835_gpio COMMAND test_bcm2835_gpio)

add_executable(test_bcm2835_aux test_bcm2835_aux.c)
target_link_libraries(test_bcm2835_aux PRIVATE hw)
add_test(NAME unit.bcm2835_aux COMMAND test_bcm2835_aux)

add_executable(test_pl011 test_pl011.c)
target_link_libraries(test_pl011 PRIVATE hw)
add_test(NAME unit.pl011 COMMAND test_pl011)

add_executable(test_bcm2836_local test_bcm2836_local.c)
target_link_libraries(test_bcm2836_local PRIVATE hw)
add_test(NAME unit.bcm2836_local COMMAND test_bcm2836_local)

add_executable(test_bcm2835_ic test_bcm2835_ic.c)
target_link_libraries(test_bcm2835_ic PRIVATE hw)
add_test(NAME unit.bcm2835_ic COMMAND test_bcm2835_ic)

add_executable(test_bcm2835_mbox test_bcm2835_mbox.c)
target_link_libraries(test_bcm2835_mbox PRIVATE hw)
add_test(NAME unit.bcm2835_mbox COMMAND test_bcm2835_mbox)

add_executable(test_sd_card test_sd_card.c)
target_link_libraries(test_sd_card PRIVATE hw)
add_test(NAME unit.sd_card COMMAND test_sd_card)

add_executable(test_bcm2835_sdhost test_bcm2835_sdhost.c)
target_link_libraries(test_bcm2835_sdhost PRIVATE hw)
add_test(NAME unit.bcm2835_sdhost COMMAND test_bcm2835_sdhost)

add_executable(test_boot_direct test_boot_direct.c)
target_link_libraries(test_boot_direct PRIVATE boot)
add_test(NAME unit.boot_direct COMMAND test_boot_direct)

add_executable(test_mbr test_mbr.c)
target_link_libraries(test_mbr PRIVATE boot)
add_test(NAME unit.mbr COMMAND test_mbr)

add_executable(test_fat test_fat.c)
target_link_libraries(test_fat PRIVATE boot)
add_test(NAME unit.fat COMMAND test_fat)

add_executable(test_rpi_config test_rpi_config.c)
target_link_libraries(test_rpi_config PRIVATE boot)
add_test(NAME unit.rpi_config COMMAND test_rpi_config)

add_executable(test_raspi3b_serial test_raspi3b_serial.c)
target_link_libraries(test_raspi3b_serial PRIVATE machine)
add_test(NAME unit.raspi3b_serial COMMAND test_raspi3b_serial)

add_executable(test_raspi3b_property test_raspi3b_property.c)
target_link_libraries(test_raspi3b_property PRIVATE machine)
add_test(NAME unit.raspi3b_property COMMAND test_raspi3b_property)

add_executable(test_raspi3b_firmware test_raspi3b_firmware.c)
target_link_libraries(test_raspi3b_firmware PRIVATE machine backend)
add_test(NAME unit.raspi3b_firmware COMMAND test_raspi3b_firmware)

add_executable(test_raspi3b test_raspi3b.c)
target_link_libraries(test_raspi3b PRIVATE machine backend)
add_test(NAME unit.raspi3b COMMAND test_raspi3b)

add_executable(test_arm64_cpu test_arm64_cpu.c)
target_link_libraries(test_arm64_cpu PRIVATE cpu)
add_test(NAME unit.arm64_cpu COMMAND test_arm64_cpu)

add_executable(test_arm64_dp_imm test_arm64_dp_imm.c)
target_link_libraries(test_arm64_dp_imm PRIVATE cpu)
add_test(NAME unit.arm64_dp_imm COMMAND test_arm64_dp_imm)

add_executable(test_arm64_dp_reg test_arm64_dp_reg.c)
target_link_libraries(test_arm64_dp_reg PRIVATE cpu)
add_test(NAME unit.arm64_dp_reg COMMAND test_arm64_dp_reg)

add_executable(test_arm64_timer test_arm64_timer.c)
target_link_libraries(test_arm64_timer PRIVATE cpu)
add_test(NAME unit.arm64_timer COMMAND test_arm64_timer)

add_executable(test_arm64_interrupt test_arm64_interrupt.c)
target_link_libraries(test_arm64_interrupt PRIVATE cpu)
add_test(NAME unit.arm64_interrupt COMMAND test_arm64_interrupt)

add_executable(test_arm64_mmu test_arm64_mmu.c)
target_link_libraries(test_arm64_mmu PRIVATE cpu)
add_test(NAME unit.arm64_mmu COMMAND test_arm64_mmu)

add_executable(test_arm64_exception test_arm64_exception.c)
target_link_libraries(test_arm64_exception PRIVATE cpu)
add_test(NAME unit.arm64_exception COMMAND test_arm64_exception)

add_executable(test_arm64_sysreg test_arm64_sysreg.c)
target_link_libraries(test_arm64_sysreg PRIVATE cpu)
add_test(NAME unit.arm64_sysreg COMMAND test_arm64_sysreg)

add_executable(test_arm64_branch_sys test_arm64_branch_sys.c)
target_link_libraries(test_arm64_branch_sys PRIVATE cpu)
add_test(NAME unit.arm64_branch_sys COMMAND test_arm64_branch_sys)

add_executable(test_arm64_load_store test_arm64_load_store.c)
target_link_libraries(test_arm64_load_store PRIVATE cpu)
add_test(NAME unit.arm64_load_store COMMAND test_arm64_load_store)
