/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Data Processing -- Immediate: the instructions of C4.1 with bits 28:26
 * set to 100. Each one follows its description in C6.2.
 */

#include <assert.h>

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

/* Ones(n) for n from 1 to 64: n one bits, zero-extended. */
static uint64_t ones(unsigned n)
{
	return n == 64 ? UINT64_MAX : ((uint64_t)1 << n) - 1;
}

/* ROR(): rotate the low width bits of x right by shift, below width. */
static uint64_t rotate_right(uint64_t x, unsigned shift, unsigned width)
{
	if (shift == 0)
		return x;
	return ((x >> shift) | (x << (width - shift))) & ones(width);
}

/* Replicate(): repeat an esize-bit element to fill datasize bits. */
static uint64_t replicate(uint64_t element, unsigned esize, unsigned datasize)
{
	uint64_t result = 0;

	for (unsigned i = 0; i < datasize; i += esize)
		result |= element << i;
	return result;
}

/*
 * DecodeBitMasks() from J1.2.3.19, for M = datasize: the masks that
 * logical immediates and bitfield moves use. Returns false where the
 * pseudocode calls Undefined().
 *
 * The value is made of identical elements of esize bits, a power of two
 * from 2 to 64. Each holds s + 1 one bits rotated right by r. N:imms
 * encodes esize and s together; immr encodes r.
 */
static bool decode_bit_masks(unsigned datasize, unsigned imm_n, unsigned imms,
                             unsigned immr, bool immediate, uint64_t *wmask,
                             uint64_t *tmask)
{
	/* len = HighestSetBitNZ(N:NOT(imms)), so that esize = 2^len. */
	unsigned size_bits = imm_n << 6 | (~imms & 0x3f);

	if (size_bits < 2) /* 000000x */
		return false;

	unsigned len = 6;

	while (!((size_bits >> len) & 1))
		len--;

	unsigned esize = 1u << len;
	unsigned levels = esize - 1;

	assert(esize <= datasize); /* the callers' decode checks ensure it */

	/* All ones in each element would make a logical immediate useless. */
	if (immediate && (imms & levels) == levels)
		return false;

	unsigned s = imms & levels;
	unsigned r = immr & levels;
	unsigned d = (s - r) & levels; /* 6-bit subtract with borrow */

	*wmask = replicate(rotate_right(ones(s + 1), r, esize), esize, datasize);
	*tmask = replicate(ones(d + 1), esize, datasize);
	return true;
}

/*
 * AND, ORR, EOR and ANDS (immediate), C6.2.14, C6.2.301, C6.2.155 and
 * C6.2.16. TST and MOV (bitmask immediate) are aliases of these.
 */
static Arm64Exec logical_imm(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	unsigned opc = field32(insn, 30, 29);
	unsigned imm_n = bit32(insn, 22);
	unsigned n = field32(insn, 9, 5);
	unsigned d = field32(insn, 4, 0);
	uint64_t imm, tmask;

	if (datasize == 32 && imm_n)
		return ARM64_EXEC_UNDEFINED;
	if (!decode_bit_masks(datasize, imm_n, field32(insn, 15, 10),
	                      field32(insn, 21, 16), true, &imm, &tmask))
		return ARM64_EXEC_UNDEFINED;

	/* Rn is a general-purpose register here, never SP. */
	uint64_t operand1 = get_x(c, n) & datasize_mask(datasize);
	uint64_t result;

	switch (opc) {
	case 0: /* AND */
	case 3: /* ANDS */
		result = operand1 & imm;
		break;
	case 1: /* ORR */
		result = operand1 | imm;
		break;
	default: /* EOR */
		result = operand1 ^ imm;
		break;
	}

	/* Rd 31 is SP, except for ANDS where it is the zero register. */
	if (opc == 3) {
		set_x(c, d, result);
		c->nzcv = (result >> (datasize - 1) ? ARM64_NZCV_N : 0) |
		          (result == 0 ? ARM64_NZCV_Z : 0); /* C and V are 0 */
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

/*
 * SBFM, BFM and UBFM, C6.2.355, C6.2.39 and C6.2.487. Many common
 * instructions are aliases of these: ASR, LSL and LSR (immediate), SBFX,
 * UBFX, SBFIZ, UBFIZ, BFI, BFXIL, BFC, SXTB, SXTH, SXTW, UXTB and UXTH.
 */
static Arm64Exec bitfield(Arm64Cpu *c, uint32_t insn)
{
	unsigned sf = bit32(insn, 31);
	unsigned datasize = 32 << sf;
	unsigned opc = field32(insn, 30, 29);
	unsigned imm_n = bit32(insn, 22);
	unsigned immr = field32(insn, 21, 16);
	unsigned imms = field32(insn, 15, 10);
	unsigned n = field32(insn, 9, 5);
	unsigned d = field32(insn, 4, 0);
	uint64_t wmask, tmask;

	if (opc == 3) /* UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;
	/* N must equal sf, and a 32-bit form has no bit 32 to name. */
	if (imm_n != sf || (datasize == 32 && (immr >= 32 || imms >= 32)))
		return ARM64_EXEC_UNDEFINED;
	if (!decode_bit_masks(datasize, imm_n, imms, immr, false, &wmask, &tmask))
		return ARM64_EXEC_UNDEFINED;

	uint64_t mask = datasize_mask(datasize);
	uint64_t src = get_x(c, n) & mask;
	uint64_t rotated = rotate_right(src, immr, datasize);
	uint64_t result;

	switch (opc) {
	case 0: { /* SBFM: bit imms of the source fills the bits above */
		uint64_t top = (src >> imms) & 1 ? mask : 0;

		result = (top & ~tmask) | (rotated & wmask & tmask);
		break;
	}
	case 1: { /* BFM: the other bits of the destination stay */
		uint64_t dst = get_x(c, d) & mask;
		uint64_t bot = (dst & ~wmask) | (rotated & wmask);

		result = (dst & ~tmask) | (bot & tmask);
		break;
	}
	default: /* UBFM */
		result = rotated & wmask & tmask;
		break;
	}
	set_x(c, d, result & mask);
	return ARM64_EXEC_OK;
}

/* EXTR, C6.2.160, and its alias ROR (immediate). */
static Arm64Exec extract(Arm64Cpu *c, uint32_t insn)
{
	unsigned sf = bit32(insn, 31);
	unsigned datasize = 32 << sf;
	unsigned lsb = field32(insn, 15, 10);
	unsigned m = field32(insn, 20, 16);
	unsigned n = field32(insn, 9, 5);
	unsigned d = field32(insn, 4, 0);

	/* op21 (bits 30:29) and o0 (bit 21) are 0 in every allocated form. */
	if (field32(insn, 30, 29) != 0 || bit32(insn, 21))
		return ARM64_EXEC_UNDEFINED;
	if (bit32(insn, 22) != sf || (datasize == 32 && lsb >= 32))
		return ARM64_EXEC_UNDEFINED;

	/* Bits lsb + datasize - 1 to lsb of the concatenation Rn:Rm. */
	uint64_t mask = datasize_mask(datasize);
	uint64_t high = get_x(c, n) & mask;
	uint64_t low = get_x(c, m) & mask;
	uint64_t result = low;

	if (lsb != 0)
		result = (low >> lsb) | (high << (datasize - lsb));
	set_x(c, d, result & mask);
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
	case 0x8: /* 100x */
	case 0x9:
		return logical_imm(c, insn);
	case 0xa: /* 101x */
	case 0xb:
		return move_wide(c, insn);
	case 0xc: /* 110x */
	case 0xd:
		return bitfield(c, insn);
	default: /* 111x */
		return extract(c, insn);
	}
}
