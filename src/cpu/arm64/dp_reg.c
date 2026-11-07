/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Data Processing -- Register: the instructions of C4.1 with bits 27:25
 * set to 101. Each one follows its description in C6.2.
 *
 * Implemented so far: Logical (shifted register), Add/subtract (shifted
 * register), Add/subtract (extended register), Conditional compare and
 * Conditional select. Add/subtract (with carry) and Data-processing (1,
 * 2 and 3 source) are not implemented yet. Classes that only hold
 * instructions newer than Armv8.0 are UNDEFINED.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

/*
 * ShiftReg() from J1.2.3.262: register m shifted by amount, which is
 * below datasize. Type 0 to 3 is LSL, LSR, ASR, ROR (DecodeShift()).
 */
static uint64_t shift_reg(Arm64Cpu *c, unsigned m, unsigned type,
                          unsigned amount, unsigned datasize)
{
	uint64_t mask = datasize_mask(datasize);
	uint64_t value = get_x(c, m) & mask;

	if (amount == 0)
		return value;

	switch (type) {
	case 0: /* LSL */
		return (value << amount) & mask;
	case 1: /* LSR */
		return value >> amount;
	case 2: { /* ASR: copies of the sign bit come in from the top */
		uint64_t extended = sign_extend64(value, datasize);
		uint64_t result = extended >> amount;

		if (extended >> 63)
			result |= ~(UINT64_MAX >> amount);
		return result & mask;
	}
	default: /* ROR */
		return ((value >> amount) | (value << (datasize - amount))) & mask;
	}
}

/*
 * ExtendReg() from J1.2.3.42: the low 8, 16, 32 or 64 bits of register
 * m, zero- or sign-extended, then shifted left by 0 to 4. option is
 * UXTB, UXTH, UXTW, UXTX, SXTB, SXTH, SXTW, SXTX for 0 to 7
 * (DecodeRegExtend()).
 */
static uint64_t extend_reg(Arm64Cpu *c, unsigned m, unsigned option,
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
 * AND, BIC, ORR, ORN, EOR, EON, ANDS and BICS (shifted register),
 * C6.2.15, C6.2.41, C6.2.302, C6.2.300, C6.2.156, C6.2.154, C6.2.17 and
 * C6.2.42. MOV, MVN and TST (shifted register) are aliases of these.
 */
static Arm64Exec logical_shifted(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	unsigned opc = field32(insn, 30, 29);
	unsigned amount = field32(insn, 15, 10);
	unsigned d = field32(insn, 4, 0);

	if (datasize == 32 && amount >= 32)
		return ARM64_EXEC_UNDEFINED;

	uint64_t mask = datasize_mask(datasize);
	uint64_t operand1 = get_x(c, field32(insn, 9, 5)) & mask;
	uint64_t operand2 = shift_reg(c, field32(insn, 20, 16),
	                              field32(insn, 23, 22), amount, datasize);
	uint64_t result;

	if (bit32(insn, 21)) /* N: BIC, ORN, EON, BICS invert operand 2 */
		operand2 = ~operand2 & mask;

	switch (opc) {
	case 0: /* AND, BIC */
	case 3: /* ANDS, BICS */
		result = operand1 & operand2;
		break;
	case 1: /* ORR, ORN */
		result = operand1 | operand2;
		break;
	default: /* EOR, EON */
		result = operand1 ^ operand2;
		break;
	}

	/* Rd 31 is the zero register in all of these. */
	set_x(c, d, result);
	if (opc == 3)
		c->nzcv = (result >> (datasize - 1) ? ARM64_NZCV_N : 0) |
		          (result == 0 ? ARM64_NZCV_Z : 0); /* C and V are 0 */
	return ARM64_EXEC_OK;
}

/*
 * ADD, ADDS, SUB and SUBS (shifted register), C6.2.6, C6.2.11, C6.2.457
 * and C6.2.465. CMP, CMN, NEG and NEGS (shifted register) are aliases.
 */
static Arm64Exec add_sub_shifted(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	bool sub = bit32(insn, 30);
	unsigned type = field32(insn, 23, 22);
	unsigned amount = field32(insn, 15, 10);
	unsigned d = field32(insn, 4, 0);

	if (type == 3) /* no ROR here */
		return ARM64_EXEC_UNDEFINED;
	if (datasize == 32 && amount >= 32)
		return ARM64_EXEC_UNDEFINED;

	uint64_t operand2 =
		shift_reg(c, field32(insn, 20, 16), type, amount, datasize);
	uint32_t nzcv;
	uint64_t result = add_with_carry(datasize, get_x(c, field32(insn, 9, 5)),
	                                 sub ? ~operand2 : operand2, sub, &nzcv);

	/* Rn and Rd 31 are the zero register here, never SP. */
	set_x(c, d, result);
	if (bit32(insn, 29))
		c->nzcv = nzcv;
	return ARM64_EXEC_OK;
}

/*
 * ADD, ADDS, SUB and SUBS (extended register), C6.2.4, C6.2.9, C6.2.456
 * and C6.2.464. CMP and CMN (extended register) are aliases.
 */
static Arm64Exec add_sub_extended(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	bool sub = bit32(insn, 30);
	unsigned shift = field32(insn, 12, 10);
	unsigned d = field32(insn, 4, 0);

	if (field32(insn, 23, 22) != 0) /* opt */
		return ARM64_EXEC_UNDEFINED;
	if (shift > 4)
		return ARM64_EXEC_UNDEFINED;

	uint64_t operand2 = extend_reg(c, field32(insn, 20, 16),
	                               field32(insn, 15, 13), shift, datasize);
	uint32_t nzcv;
	uint64_t result = add_with_carry(datasize, get_xsp(c, field32(insn, 9, 5)),
	                                 sub ? ~operand2 : operand2, sub, &nzcv);

	/* Rn 31 is SP; Rd 31 is SP for ADD and SUB, the zero register else. */
	if (bit32(insn, 29)) {
		set_x(c, d, result);
		c->nzcv = nzcv;
	} else {
		set_xsp(c, d, result);
	}
	return ARM64_EXEC_OK;
}

/*
 * CCMN and CCMP, register and immediate forms, C6.2.80, C6.2.82, C6.2.79
 * and C6.2.81: the flags of a comparison if the condition holds, and the
 * nzcv field otherwise.
 */
static Arm64Exec conditional_compare(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	bool sub = bit32(insn, 30); /* CCMP; CCMN adds */

	/* S must be 1, o2 and o3 must be 0. */
	if (!bit32(insn, 29) || bit32(insn, 10) || bit32(insn, 4))
		return ARM64_EXEC_UNDEFINED;

	if (!condition_holds(c, field32(insn, 15, 12))) {
		c->nzcv = field32(insn, 3, 0) << 28;
		return ARM64_EXEC_OK;
	}

	unsigned field = field32(insn, 20, 16); /* Rm or imm5 */
	uint64_t operand2 = bit32(insn, 11) ? field : get_x(c, field);

	add_with_carry(datasize, get_x(c, field32(insn, 9, 5)),
	               sub ? ~operand2 : operand2, sub, &c->nzcv);
	return ARM64_EXEC_OK;
}

/*
 * CSEL, CSINC, CSINV and CSNEG, C6.2.138, C6.2.141, C6.2.142 and
 * C6.2.143. CSET, CSETM, CINC, CINV and CNEG are aliases of these.
 */
static Arm64Exec conditional_select(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	unsigned op2 = field32(insn, 11, 10);
	uint64_t result;

	if (bit32(insn, 29) || op2 >= 2) /* S = 1 or op2 = 1x: UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;

	if (condition_holds(c, field32(insn, 15, 12))) {
		result = get_x(c, field32(insn, 9, 5));
	} else {
		result = get_x(c, field32(insn, 20, 16));
		if (bit32(insn, 30)) /* CSINV, CSNEG */
			result = ~result;
		if (op2 & 1) /* CSINC, CSNEG */
			result += 1;
	}
	set_x(c, field32(insn, 4, 0), result & datasize_mask(datasize));
	return ARM64_EXEC_OK;
}

Arm64Exec arm64_exec_dp_reg(Arm64Cpu *c, uint32_t insn)
{
	unsigned op2 = field32(insn, 24, 21);

	/* op1, bit 28, clear: the logical and add/subtract classes. */
	if (!bit32(insn, 28)) {
		if (!(op2 & 8)) /* 0xxx */
			return logical_shifted(c, insn);
		if (op2 & 1) /* 1xx1 */
			return add_sub_extended(c, insn);
		return add_sub_shifted(c, insn); /* 1xx0 */
	}

	switch (op2) {
	case 0x0:
		/*
		 * op3 = 000000 is Add/subtract (with carry). The other values
		 * hold FEAT_CPA and FEAT_FlagM instructions, or are UNALLOCATED.
		 */
		if (field32(insn, 15, 10) == 0)
			return ARM64_EXEC_UNIMPLEMENTED;
		return ARM64_EXEC_UNDEFINED;
	case 0x2:
		return conditional_compare(c, insn);
	case 0x4:
		return conditional_select(c, insn);
	case 0x6: /* Data-processing (2 source) and (1 source) */
		return ARM64_EXEC_UNIMPLEMENTED;
	default:
		if (op2 & 8) /* Data-processing (3 source) */
			return ARM64_EXEC_UNIMPLEMENTED;
		return ARM64_EXEC_UNDEFINED; /* 0xx1: UNALLOCATED */
	}
}
