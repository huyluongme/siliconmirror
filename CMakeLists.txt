# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

cmake_minimum_required(VERSION 3.25)

project(siliconmirror
	VERSION 0.1.0
	DESCRIPTION "Generic full-system emulator"
	LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Put executables at the top of the build directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

option(SMIRROR_WERROR "Treat compiler warnings as errors" OFF)
option(SMIRROR_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	add_compile_options(
		-Wall
		-Wextra
		-Wpedantic
		-Wshadow
		-Wstrict-prototypes
		-Wmissing-prototypes)
	if(SMIRROR_WERROR)
		add_compile_options(-Werror)
	endif()
	if(SMIRROR_SANITIZE)
		add_compile_options(-fsanitize=address,undefined
			-fno-omit-frame-pointer)
		add_link_options(-fsanitize=address,undefined)
	endif()
endif()

include(GNUInstallDirs)

# The GNU binutils for AArch64 (the binutils-aarch64-linux-gnu package on
# Debian and Ubuntu) assemble code that runs inside the emulator: the
# bare-metal tests, and the boot stub of the raspi3b board. They are
# optional: smirror is built from the stub bytes kept in the source tree.
# find_program() stores the tool's full path in the variable, or
# <name>-NOTFOUND, which if() treats as false.
find_program(AARCH64_AS NAMES aarch64-linux-gnu-as aarch64-none-elf-as)
find_program(AARCH64_LD NAMES aarch64-linux-gnu-ld aarch64-none-elf-ld)
find_program(AARCH64_OBJCOPY
	NAMES aarch64-linux-gnu-objcopy aarch64-none-elf-objcopy)
if(AARCH64_AS AND AARCH64_LD AND AARCH64_OBJCOPY)
	set(AARCH64_BINUTILS_FOUND TRUE)
else()
	set(AARCH64_BINUTILS_FOUND FALSE)
endif()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
