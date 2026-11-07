/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Data Processing -- Immediate: the instructions of C4.1 with bits 28:26
 * set to 100. Each one follows its description in C6.2.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

/* ADR and ADRP, C6.2.12 and C6.2.13. */
static Arm64Exec pc_rel_addressing(Arm64Cpu *c, uint32_t insn)
{
	unsigned d = field32(insn, 4, 0);
	uint64_t imm =
		sign_extend64(field32(insn, 23, 5) << 2 | field32(insn, 30, 29), 21);

	if (bit32(insn, 31)) /* ADRP: a 4KB page relative to the PC's page */
		set_x(c, d, (c->pc & ~(uint64_t)0xfff) + (imm << 12));
	else
		set_x(c, d, c->pc + imm);
	return ARM64_EXEC_OK;
}

/*
 * ADD, ADDS, SUB and SUBS (immediate), C6.2.5, C6.2.10, C6.2.457 and
 * C6.2.464. CMP, CMN and MOV (to/from SP) are aliases of these.
 */
static Arm64Exec add_sub_imm(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	bool sub = bit32(insn, 30);
	bool setflags = bit32(insn, 29);
	unsigned shift = bit32(insn, 22) ? 12 : 0;
	uint64_t imm = (uint64_t)field32(insn, 21, 10) << shift;
	unsigned n = field32(insn, 9, 5);
	unsigned d = field32(insn, 4, 0);

	/* Rn is always register or SP, and SUB adds NOT(imm) plus 1. */
	uint32_t nzcv;
	uint64_t result =
		add_with_carry(datasize, get_xsp(c, n), sub ? ~imm : imm, sub, &nzcv);

	/* Rd 31 is SP for ADD and SUB, the zero register for ADDS and SUBS. */
	if (setflags) {
		set_x(c, d, result);
		c->nzcv = nzcv;
	} else {
		set_xsp(c, d, result);
	}
	return ARM64_EXEC_OK;
}

/* MOVN, MOVZ and MOVK, C6.2.284, C6.2.285 and C6.2.283. */
static Arm64Exec move_wide(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	unsigned opc = field32(insn, 30, 29);
	unsigned hw = field32(insn, 22, 21);
	unsigned d = field32(insn, 4, 0);

	if (opc == 1) /* UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;
	if (datasize == 32 && hw >= 2) /* no 16-bit chunk at bit 32 or 48 */
		return ARM64_EXEC_UNDEFINED;

	unsigned pos = hw << 4;
	uint64_t imm = (uint64_t)field32(insn, 20, 5) << pos;
	uint64_t result;

	switch (opc) {
	case 0: /* MOVN */
		result = ~imm;
		break;
	case 2: /* MOVZ */
		result = imm;
		break;
	default: /* MOVK */
		result = (get_x(c, d) & ~((uint64_t)0xffff << pos)) | imm;
		break;
	}
	set_x(c, d, result & datasize_mask(datasize));
	return ARM64_EXEC_OK;
}

Arm64Exec arm64_exec_dp_imm(Arm64Cpu *c, uint32_t insn)
{
	unsigned op0 = field32(insn, 30, 29);
	unsigned op1 = field32(insn, 25, 22);

	/*
	 * Classes whose instructions all need a feature newer than Armv8.0
	 * are UNDEFINED. First, Data-processing (1 source immediate), which
	 * holds only FEAT_PAuth_LR instructions.
	 */
	if (op0 == 3 && op1 >= 0xe)
		return ARM64_EXEC_UNDEFINED;

	switch (op1) {
	case 0x0: /* 00xx */
	case 0x1:
	case 0x2:
	case 0x3:
		return pc_rel_addressing(c, insn);
	case 0x4: /* 010x */
	case 0x5:
		return add_sub_imm(c, insn);
	case 0x6: /* Add/subtract (immediate, with tags): only FEAT_MTE */
	case 0x7: /* Min/max (immediate): only FEAT_CSSC */
		return ARM64_EXEC_UNDEFINED;
	case 0xa: /* 101x */
	case 0xb:
		return move_wide(c, insn);
	default:
		/* Logical (immediate), Bitfield and Extract. */
		return ARM64_EXEC_UNIMPLEMENTED;
	}
}
