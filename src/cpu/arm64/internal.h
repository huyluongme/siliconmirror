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
#include "util/bits.h"

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

/* All ones in the low datasize bits; datasize is 32 or 64. */
static inline uint64_t datasize_mask(unsigned datasize)
{
	return datasize == 64 ? UINT64_MAX : UINT32_MAX;
}

/*
 * ExtendReg() from J1.2.3.42: the low 8, 16, 32 or 64 bits of register
 * m, zero- or sign-extended, then shifted left by 0 to 4. option is
 * UXTB, UXTH, UXTW, UXTX, SXTB, SXTH, SXTW, SXTX for 0 to 7
 * (DecodeRegExtend()).
 */
static inline uint64_t extend_reg(Arm64Cpu *c, unsigned m, unsigned option,
                                  unsigned shift, unsigned datasize)
{
	unsigned len = 8u << (option & 3);
	uint64_t value = get_x(c, m);

	if (len < 64) {
		if (option & 4)
			value = sign_extend64(value, len);
		else
			value &= ((uint64_t)1 << len) - 1;
	}
	return (value << shift) & datasize_mask(datasize);
}

/*
 * AddWithCarry() from J1.4.3.218, for datasize 32 or 64: returns the low
 * datasize bits of x + y + carry_in, zero-extended, and stores N, Z, C
 * and V in *nzcv, in their positions in the NZCV register.
 */
static inline uint64_t add_with_carry(unsigned datasize, uint64_t x, uint64_t y,
                                      unsigned carry_in, uint32_t *nzcv)
{
	uint64_t mask = datasize_mask(datasize);
	uint64_t sign = (uint64_t)1 << (datasize - 1);

	x &= mask;
	y &= mask;

	uint64_t result = (x + y + carry_in) & mask;

	/*
	 * The carry out of the top bit is 1 when both operands have it set,
	 * or when one has and the carry into it cleared it in the result.
	 * The signed result overflows when both operands have the same sign
	 * and the result has the other one.
	 */
	uint64_t carry = ((x & y) | ((x | y) & ~result)) & sign;
	uint64_t overflow = (x ^ result) & (y ^ result) & sign;

	*nzcv = (result & sign ? ARM64_NZCV_N : 0) |
	        (result == 0 ? ARM64_NZCV_Z : 0) | (carry ? ARM64_NZCV_C : 0) |
	        (overflow ? ARM64_NZCV_V : 0);
	return result;
}

/* ConditionHolds() from J1.4.3.363: whether cond holds for PSTATE.NZCV. */
static inline bool condition_holds(const Arm64Cpu *c, unsigned cond)
{
	bool n = c->nzcv & ARM64_NZCV_N, z = c->nzcv & ARM64_NZCV_Z;
	bool cf = c->nzcv & ARM64_NZCV_C, v = c->nzcv & ARM64_NZCV_V;
	bool result;

	switch (cond >> 1) {
	case 0: /* EQ or NE */
		result = z;
		break;
	case 1: /* CS or CC */
		result = cf;
		break;
	case 2: /* MI or PL */
		result = n;
		break;
	case 3: /* VS or VC */
		result = v;
		break;
	case 4: /* HI or LS */
		result = cf && !z;
		break;
	case 5: /* GE or LT */
		result = n == v;
		break;
	case 6: /* GT or LE */
		result = n == v && !z;
		break;
	default:         /* AL */
		return true; /* 1111 is also always true */
	}
	return (cond & 1) ? !result : result;
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
	/* A data access failed; c->data_abort describes it. */
	ARM64_EXEC_DATA_ABORT,
} Arm64Exec;

/* Decode and execute one instruction; c->pc is its address. */
Arm64Exec arm64_execute(Arm64Cpu *c, uint32_t insn);

/* The same for each group of C4.1, in the source file named after it. */
Arm64Exec arm64_exec_dp_imm(Arm64Cpu *c, uint32_t insn);
Arm64Exec arm64_exec_branch_sys(Arm64Cpu *c, uint32_t insn);
Arm64Exec arm64_exec_dp_reg(Arm64Cpu *c, uint32_t insn);
Arm64Exec arm64_exec_load_store(Arm64Cpu *c, uint32_t insn);

/* SEV, from cpu.c: signal an event to every PE of the machine. */
void arm64_send_event(Arm64Cpu *c);

/*
 * Data accesses of size 1, 2, 4 or 8 bytes, from mem.c. On failure they
 * fill in c->data_abort and return false; the instruction then returns
 * ARM64_EXEC_DATA_ABORT.
 */
bool arm64_mem_read(Arm64Cpu *c, uint64_t address, unsigned size,
                    uint64_t *value);
bool arm64_mem_write(Arm64Cpu *c, uint64_t address, unsigned size,
                     uint64_t value);

#endif /* CPU_ARM64_INTERNAL_H */
