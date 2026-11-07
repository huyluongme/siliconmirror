/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Data Processing -- Register: the instructions of C4.1 with bits 27:25
 * set to 101. Each one follows its description in C6.2.
 *
 * The model provides the Armv8.0 instructions and FEAT_CRC32, as the
 * Cortex-A53 of the Raspberry Pi 3 does. Classes that only hold newer
 * instructions are UNDEFINED.
 *
 * SMULH and UMULH have Ra as a should-be-one field. When it is not
 * 11111, behavior is CONSTRAINED UNPREDICTABLE (K1.2.2); this model
 * executes them as if it were.
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

/*
 * ADC, ADCS, SBC and SBCS, C6.2.2, C6.2.3, C6.2.352 and C6.2.353. NGC
 * and NGCS are aliases of these.
 */
static Arm64Exec add_sub_carry(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	bool sub = bit32(insn, 30);
	uint64_t operand2 = get_x(c, field32(insn, 20, 16));
	uint32_t nzcv;
	uint64_t result = add_with_carry(datasize, get_x(c, field32(insn, 9, 5)),
	                                 sub ? ~operand2 : operand2,
	                                 (c->nzcv & ARM64_NZCV_C) ? 1 : 0, &nzcv);

	set_x(c, field32(insn, 4, 0), result);
	if (bit32(insn, 29))
		c->nzcv = nzcv;
	return ARM64_EXEC_OK;
}

/* Reverse the order of the low width bits of x: BitReverse(), J1.4.3.34. */
static uint64_t reverse_bits(uint64_t x, unsigned width)
{
	uint64_t result = 0;

	for (unsigned i = 0; i < width; i++)
		result |= ((x >> i) & 1) << (width - 1 - i);
	return result;
}

/*
 * The CRC32 instructions, C6.2.135 and C6.2.136: the low size bits of
 * val added to the CRC in acc, with no inversion before or after.
 *
 * The pseudocode reverses the bits of acc and val, divides by the
 * polynomial with Poly32Mod2() and reverses the remainder. Dividing the
 * bits as they are, least significant first, by the bit-reversed
 * polynomial gives the same result.
 */
static uint32_t crc32_update(uint32_t acc, uint64_t val, unsigned size,
                             uint32_t poly)
{
	uint32_t reversed = (uint32_t)reverse_bits(poly, 32);

	for (unsigned i = 0; i < size; i++) {
		bool carry = (acc ^ (uint32_t)(val >> i)) & 1;

		acc = (acc >> 1) ^ (carry ? reversed : 0);
	}
	return acc;
}

/*
 * Data-processing (2 source): UDIV, SDIV, LSLV, LSRV, ASRV, RORV and
 * the CRC32 instructions, C6.2.490, C6.2.357, C6.2.271, C6.2.274,
 * C6.2.22, C6.2.349, C6.2.135 and C6.2.136. LSL, LSR, ASR and ROR
 * (register) are aliases of the shifts.
 */
static Arm64Exec dp_2source(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	unsigned opcode = field32(insn, 15, 10);
	unsigned n = field32(insn, 9, 5);
	unsigned d = field32(insn, 4, 0);
	uint64_t mask = datasize_mask(datasize);
	uint64_t operand1 = get_x(c, n) & mask;
	uint64_t operand2 = get_x(c, field32(insn, 20, 16)) & mask;
	uint64_t result;

	if (bit32(insn, 29)) /* S = 1: SUBPS (FEAT_MTE), or UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;

	switch (opcode) {
	case 0x02: /* UDIV */
		result = operand2 == 0 ? 0 : operand1 / operand2;
		break;
	case 0x03: { /* SDIV: rounds towards zero */
		uint64_t sign = (uint64_t)1 << (datasize - 1);
		bool negative1 = operand1 & sign, negative2 = operand2 & sign;
		uint64_t abs1 = negative1 ? -operand1 & mask : operand1;
		uint64_t abs2 = negative2 ? -operand2 & mask : operand2;

		if (operand2 == 0) {
			result = 0;
		} else {
			/* The most negative value divided by -1 wraps to itself. */
			result = abs1 / abs2;
			if (negative1 != negative2)
				result = -result;
		}
		break;
	}
	case 0x08: /* LSLV */
	case 0x09: /* LSRV */
	case 0x0a: /* ASRV */
	case 0x0b: /* RORV */
		result = shift_reg(c, n, opcode & 3, operand2 % datasize, datasize);
		break;
	case 0x10: /* CRC32B, CRC32H, CRC32W, CRC32X */
	case 0x11:
	case 0x12:
	case 0x13:
	case 0x14: /* CRC32CB, CRC32CH, CRC32CW, CRC32CX */
	case 0x15:
	case 0x16:
	case 0x17: {
		unsigned sz = opcode & 3;

		/* Only the X forms are 64-bit. */
		if ((sz == 3) != (datasize == 64))
			return ARM64_EXEC_UNDEFINED;
		result =
			crc32_update((uint32_t)get_x(c, n), get_x(c, field32(insn, 20, 16)),
		                 8u << sz, (opcode & 4) ? 0x1edc6f41 : 0x04c11db7);
		set_x(c, d, result);
		return ARM64_EXEC_OK;
	}
	default:
		/*
		 * SUBP, IRG and GMI (FEAT_MTE), PACGA (FEAT_PAuth), SMAX, UMAX,
		 * SMIN and UMIN (FEAT_CSSC), or UNALLOCATED.
		 */
		return ARM64_EXEC_UNDEFINED;
	}
	set_x(c, d, result & mask);
	return ARM64_EXEC_OK;
}

/*
 * Data-processing (1 source): RBIT, REV16, REV32, REV, CLZ and CLS,
 * C6.2.321, C6.2.343, C6.2.344, C6.2.342, C6.2.91 and C6.2.90. REV64 is
 * an alias of REV.
 */
static Arm64Exec dp_1source(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	unsigned opcode = field32(insn, 15, 10);
	uint64_t operand = get_x(c, field32(insn, 9, 5)) & datasize_mask(datasize);
	uint64_t result = 0;

	/* S = 1, or opcode2 != 00000: FEAT_PAuth instructions, UNALLOCATED. */
	if (bit32(insn, 29) || field32(insn, 20, 16) != 0)
		return ARM64_EXEC_UNDEFINED;

	switch (opcode) {
	case 0x00: /* RBIT */
		result = reverse_bits(operand, datasize);
		break;
	case 0x01:   /* REV16 */
	case 0x02:   /* REV32, and REV for 32 bits */
	case 0x03: { /* REV for 64 bits */
		unsigned container = 8u << opcode;

		if (container > datasize)
			return ARM64_EXEC_UNDEFINED;
		/* Reverse the bytes of each container. */
		for (unsigned i = 0; i < datasize; i += container)
			for (unsigned b = 0; b < container; b += 8)
				result |= ((operand >> (i + b)) & 0xff)
				          << (i + container - 8 - b);
		break;
	}
	case 0x04: /* CLZ: CountLeadingZeroBits() */
		for (int i = datasize - 1; i >= 0 && !((operand >> i) & 1); i--)
			result++;
		break;
	case 0x05: { /* CLS: CountLeadingSignBits() */
		uint64_t sign = (operand >> (datasize - 1)) & 1;

		for (int i = datasize - 2; i >= 0 && ((operand >> i) & 1) == sign; i--)
			result++;
		break;
	}
	default: /* CTZ, CNT and ABS (FEAT_CSSC), or UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;
	}
	set_x(c, field32(insn, 4, 0), result);
	return ARM64_EXEC_OK;
}

/* The high 64 bits of the 128-bit product of x and y, unsigned. */
static uint64_t multiply_high(uint64_t x, uint64_t y)
{
	uint64_t x_lo = x & UINT32_MAX, x_hi = x >> 32;
	uint64_t y_lo = y & UINT32_MAX, y_hi = y >> 32;
	uint64_t lo_lo = x_lo * y_lo, hi_lo = x_hi * y_lo;
	uint64_t lo_hi = x_lo * y_hi, hi_hi = x_hi * y_hi;

	/* The middle column, with the carry from the low one; no overflow. */
	uint64_t middle = (lo_lo >> 32) + (hi_lo & UINT32_MAX) + lo_hi;

	return hi_hi + (hi_lo >> 32) + (middle >> 32);
}

/*
 * Data-processing (3 source): MADD, MSUB, SMADDL, SMSUBL, UMADDL,
 * UMSUBL, SMULH and UMULH, C6.2.275, C6.2.291, C6.2.369, C6.2.375,
 * C6.2.496, C6.2.497, C6.2.379 and C6.2.498. MUL, MNEG, SMULL, SMNEGL,
 * UMULL and UMNEGL are aliases.
 */
static Arm64Exec dp_3source(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	unsigned op31 = field32(insn, 23, 21);
	bool sub = bit32(insn, 15); /* o0 */
	uint64_t operand1 = get_x(c, field32(insn, 9, 5));
	uint64_t operand2 = get_x(c, field32(insn, 20, 16));
	uint64_t addend = get_x(c, field32(insn, 14, 10));
	uint64_t product;

	if (field32(insn, 30, 29) != 0) /* op54 */
		return ARM64_EXEC_UNDEFINED;
	if (datasize == 32 && op31 != 0)
		return ARM64_EXEC_UNDEFINED;

	switch (op31) {
	case 0: /* MADD, MSUB: the low datasize bits of the result */
		product = operand1 * operand2;
		break;
	case 1: /* SMADDL, SMSUBL */
		product = sign_extend64(operand1, 32) * sign_extend64(operand2, 32);
		break;
	case 5: /* UMADDL, UMSUBL */
		product = (operand1 & UINT32_MAX) * (operand2 & UINT32_MAX);
		break;
	case 2: /* SMULH */
	case 6: /* UMULH */
		if (sub)
			return ARM64_EXEC_UNDEFINED;
		product = multiply_high(operand1, operand2);
		if (op31 == 2) {
			/* From unsigned to signed: subtract y if x < 0, x if y < 0. */
			if (operand1 >> 63)
				product -= operand2;
			if (operand2 >> 63)
				product -= operand1;
		}
		set_x(c, field32(insn, 4, 0), product);
		return ARM64_EXEC_OK;
	default: /* MADDPT, MSUBPT (FEAT_CPA), or UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;
	}

	uint64_t result = sub ? addend - product : addend + product;

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
			return add_sub_carry(c, insn);
		return ARM64_EXEC_UNDEFINED;
	case 0x2:
		return conditional_compare(c, insn);
	case 0x4:
		return conditional_select(c, insn);
	case 0x6: /* op0, bit 30, selects 1 source or 2 source */
		return bit32(insn, 30) ? dp_1source(c, insn) : dp_2source(c, insn);
	default:
		if (op2 & 8)
			return dp_3source(c, insn);
		return ARM64_EXEC_UNDEFINED; /* 0xx1: UNALLOCATED */
	}
}
