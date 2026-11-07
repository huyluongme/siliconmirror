# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
#
# Thin wrapper around the CMake presets in CMakePresets.json.
# Run "make help" for the list of targets.

PRESET ?= debug

# CMake remembers the compiler a build directory was configured with, so a
# compiler chosen with CC=... gets its own build directory.
ifeq ($(filter command line environment,$(origin CC)),)
BUILD_DIR := build/$(PRESET)
else
BUILD_DIR := build/$(PRESET)-$(notdir $(CC))
CMAKE_ENV := CC=$(CC)
endif

# C sources tracked by git, plus new files not yet added.
C_SOURCES = git ls-files --cached --others --exclude-standard '*.c' '*.h'

.PHONY: all build test run format check-format clean help

all: build

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

run: build
	./$(BUILD_DIR)/smirror $(ARGS)

# Configure only once. After that, CMake reconfigures by itself whenever
# a CMakeLists.txt changes.
$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE_ENV) cmake --preset $(PRESET) -B $(BUILD_DIR)

format:
	$(C_SOURCES) | xargs -r clang-format -i

check-format:
	$(C_SOURCES) | xargs -r clang-format --dry-run --Werror

clean:
	rm -rf build

help:
	@echo "Targets:"
	@echo "  make                  Build (same as 'make build')"
	@echo "  make test             Build and run the tests"
	@echo "  make run ARGS=\"...\"   Build and run smirror with arguments"
	@echo "  make format           Format all C sources with clang-format"
	@echo "  make check-format     Check formatting without changing files"
	@echo "  make clean            Remove all build directories"
	@echo ""
	@echo "Variables:"
	@echo "  PRESET=<name>         debug (default), release, asan, ci or"
	@echo "                        ci-release"
	@echo "  CC=<compiler>         Build with another compiler, e.g. clang"
	@echo ""
	@echo "Build directory: build/<preset>, or build/<preset>-<compiler>"
	@echo "when CC is given."
