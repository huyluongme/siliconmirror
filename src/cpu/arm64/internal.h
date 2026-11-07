/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Helpers shared by the source files of the AArch64 model. Not for use
 * outside src/cpu/arm64/, except by its unit tests.
 */

#ifndef CPU_ARM64_INTERNAL_H
#define CPU_ARM64_INTERNAL_H

#include <stdint.h>

#include "cpu/arm64/cpu.h"

/*
 * General-purpose registers, C1.2.6.1 and the X accessor in J1.2.3.260:
 * register number 31 reads as zero and writes to it are discarded. A
 * 32-bit write clears the upper 32 bits.
 */
static inline uint64_t get_x(const Arm64Cpu *c, unsigned n)
{
	return n == 31 ? 0 : c->x[n];
}

static inline void set_x(Arm64Cpu *c, unsigned n, uint64_t value)
{
	if (n != 31)
		c->x[n] = value;
}

static inline void set_w(Arm64Cpu *c, unsigned n, uint32_t value)
{
	set_x(c, n, value);
}

/*
 * The current stack pointer, from the SP accessor in J1.2.3.259:
 * PSTATE.SP == 0 selects SP_EL0, otherwise SP_ELx for the current EL.
 */
static inline uint64_t *current_sp(Arm64Cpu *c)
{
	return &c->sp_el[c->sp_sel ? c->el : 0];
}

/* Register n, where 31 means the current stack pointer. */
static inline uint64_t get_xsp(Arm64Cpu *c, unsigned n)
{
	return n == 31 ? *current_sp(c) : c->x[n];
}

static inline void set_xsp(Arm64Cpu *c, unsigned n, uint64_t value)
{
	if (n == 31)
		*current_sp(c) = value;
	else
		c->x[n] = value;
}

/* Instruction groups of the top-level A64 encoding table, C4.1. */
typedef enum Arm64Group {
	ARM64_GROUP_RESERVED,
	ARM64_GROUP_SME,
	ARM64_GROUP_SVE,
	ARM64_GROUP_UNALLOCATED,
	ARM64_GROUP_DP_IMM,
	ARM64_GROUP_BRANCH_SYS,
	ARM64_GROUP_DP_REG,
	ARM64_GROUP_SIMD_FP,
	ARM64_GROUP_LOAD_STORE,
} Arm64Group;

/* The group an instruction belongs to. */
Arm64Group arm64_group(uint32_t insn);

/* The group's name as the manual writes it, with ASCII dashes. */
const char *arm64_group_name(Arm64Group group);

/* What happened when the CPU tried to execute an instruction. */
typedef enum Arm64Exec {
	/* Executed; next_pc holds the address of the next instruction. */
	ARM64_EXEC_OK,
	/* UNDEFINED by the architecture, or by this CPU's feature set. */
	ARM64_EXEC_UNDEFINED,
	/* Valid, but this model does not implement it yet. */
	ARM64_EXEC_UNIMPLEMENTED,
} Arm64Exec;

/* Decode and execute one instruction; c->pc is its address. */
Arm64Exec arm64_execute(Arm64Cpu *c, uint32_t insn);

#endif /* CPU_ARM64_INTERNAL_H */
