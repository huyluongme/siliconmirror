# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# CPU models, one directory per architecture. See docs/architecture.md.
add_library(cpu STATIC
	arm64/branch_sys.c
	arm64/cpu.c
	arm64/decode.c
	arm64/dp_imm.c
	arm64/dp_reg.c
	arm64/load_store.c
	arm64/mem.c)

target_link_libraries(cpu PUBLIC core)
