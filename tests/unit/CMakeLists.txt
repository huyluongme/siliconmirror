# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Keep test programs in build/<preset>/tests/unit/, away from smirror.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# One program per module, linked against that module only.
add_executable(test_byteorder test_byteorder.c)
target_link_libraries(test_byteorder PRIVATE util)
add_test(NAME unit.byteorder COMMAND test_byteorder)

add_executable(test_bus test_bus.c)
target_link_libraries(test_bus PRIVATE core)
add_test(NAME unit.bus COMMAND test_bus)
