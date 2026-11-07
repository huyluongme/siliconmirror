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

add_executable(test_pl011 test_pl011.c)
target_link_libraries(test_pl011 PRIVATE hw)
add_test(NAME unit.pl011 COMMAND test_pl011)

add_executable(test_arm64_cpu test_arm64_cpu.c)
target_link_libraries(test_arm64_cpu PRIVATE cpu)
add_test(NAME unit.arm64_cpu COMMAND test_arm64_cpu)

add_executable(test_arm64_dp_imm test_arm64_dp_imm.c)
target_link_libraries(test_arm64_dp_imm PRIVATE cpu)
add_test(NAME unit.arm64_dp_imm COMMAND test_arm64_dp_imm)
