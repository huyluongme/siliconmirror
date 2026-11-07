/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * AArch64 CPU model.
 *
 * Written from the Arm Architecture Reference Manual for A-profile
 * architecture (ARM DDI 0487), issue M.d. Section numbers in the source
 * refer to that manual.
 *
 * The model runs in AArch64 state only: AArch32 is not supported.
 * Exceptions are not implemented yet; anything that would take one stops
 * the machine with an error message instead.
 */

#ifndef CPU_ARM64_CPU_H
#define CPU_ARM64_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "core/cpu.h"

/* PSTATE.{N,Z,C,V}, in their positions in the NZCV register. */
#define ARM64_NZCV_V (1u << 28)
#define ARM64_NZCV_C (1u << 29)
#define ARM64_NZCV_Z (1u << 30)
#define ARM64_NZCV_N (1u << 31)

/* PSTATE.{D,A,I,F}, in their positions in the DAIF register. */
#define ARM64_DAIF_F (1u << 6)
#define ARM64_DAIF_I (1u << 7)
#define ARM64_DAIF_A (1u << 8)
#define ARM64_DAIF_D (1u << 9)

typedef struct Arm64Config {
	/*
	 * Address of the first instruction after reset: the IMPLEMENTATION
	 * DEFINED reset vector, RVBAR_ELx.
	 */
	uint64_t reset_pc;

	/* Highest implemented Exception level, 1 to 3. Reset enters it. */
	unsigned highest_el;
} Arm64Config;

typedef struct Arm64Cpu {
	Cpu cpu;
	Arm64Config config;

	uint64_t x[31];    /* R0-R30; number 31 is the zero register or SP */
	uint64_t sp_el[4]; /* SP_EL0 to SP_EL3 */
	uint64_t pc;

	/* PSTATE fields, see the PSTATE pseudocode in J1. */
	uint32_t nzcv; /* N, Z, C and V in bits 31:28, as in NZCV */
	uint32_t daif; /* D, A, I and F in bits 9:6, as in DAIF */
	unsigned el;   /* PSTATE.EL, the current Exception level */
	bool sp_sel;   /* PSTATE.SP: false selects SP_EL0, true SP_ELx */

	/* Address of the next instruction, set while one executes. */
	uint64_t next_pc;
} Arm64Cpu;

/*
 * Create a CPU in its reset state. Add it to a machine with
 * machine_add_cpu() before running it. Returns NULL when config is
 * invalid or memory runs out.
 */
Cpu *arm64_cpu_create(const Arm64Config *config);

/* The Arm64Cpu that contains cpu, which must come from arm64_cpu_create(). */
Arm64Cpu *arm64_cpu_of(Cpu *cpu);

#endif /* CPU_ARM64_CPU_H */
