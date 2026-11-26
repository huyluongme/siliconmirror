# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# CPU models, one directory per architecture. See docs/architecture.md.
add_library(cpu STATIC
	arm64/branch_sys.c
	arm64/cortex_a53.c
	arm64/cpu.c
	arm64/debug.c
	arm64/decode.c
	arm64/dp_imm.c
	arm64/dp_reg.c
	arm64/exception.c
	arm64/exclusive.c
	arm64/fp.c
	arm64/fp_scalar.c
	arm64/interrupt.c
	arm64/load_store.c
	arm64/mem.c
	arm64/mmu.c
	arm64/sysinsn.c
	arm64/sysreg.c
	arm64/pmu.c
	arm64/simd.c
	arm64/simd_int.c
	arm64/simd_same.c
	arm64/simd_shift.c
	arm64/simd_ldst.c
	arm64/simd_long.c
	arm64/simd_perm.c
	arm64/simd_fp.c
	arm64/timer.c
	jit/exec_mem.c
	jit/x64.c)

target_link_libraries(cpu PUBLIC core)
