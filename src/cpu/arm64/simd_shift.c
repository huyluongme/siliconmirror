/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Advanced SIMD shift by immediate and its scalar form, two classes of
 * C4.1 that simd.c gives to this file: 0 Q U 011110 immh immb opcode 1
 * Rn Rd and 01 U 111110 immh immb opcode 1 Rn Rd. Each instruction
 * follows its description in C7.2. immh gives the element size by its
 * highest set bit, and immh:immb the shift. The conversions to and from
 * fixed point (SCVTF, UCVTF, FCVTZS, FCVTZU) stop the machine as not
 * implemented yet; with half precision, FEAT_FP16, they are UNDEFINED.
 *
 * A saturating instruction sets FPSR.QC when a result saturates.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

__extension__ typedef __int128 int128;

#define FPSR_QC (UINT32_C(1) << 27)

/* The kinds of instruction of the classes. */
typedef enum Kind {
	RIGHT,  /* SSHR, USHR, SRSHR, URSHR and the accumulating SSRA ... */
	INSERT, /* SRI and SLI */
	LEFT,   /* SHL */
	SAT,    /* SQSHL, UQSHL, SQSHLU (immediate) */
	NARROW, /* SHRN, RSHRN and the saturating ones */
	LONG,   /* SSHLL, USHLL */
	FIXED,  /* SCVTF, UCVTF, FCVTZS, FCVTZU (fixed-point) */
	NONE,   /* UNALLOCATED */
} Kind;

static int128 to_int(uint64_t value, unsigned size, bool is_unsigned)
{
	value &= bytes_mask(size);
	return is_unsigned ? (int128)value
	                   : (int128)(int64_t)sign_extend64(value, 8 * size);
}

/* SatQ() of J1.3, setting FPSR.QC when it saturates. */
static uint64_t saturate(Arm64Cpu *c, int128 value, unsigned size,
                         bool is_unsigned)
{
	int128 high = is_unsigned ? ((int128)1 << (8 * size)) - 1
	                          : ((int128)1 << (8 * size - 1)) - 1;
	int128 low = is_unsigned ? 0 : -high - 1;

	if (value > high || value < low) {
		value = value > high ? high : low;
		c->fpsr |= FPSR_QC;
	}
	return (uint64_t)value & bytes_mask(size);
}

/* The kind of the instruction of U and opcode, from the tables of C4.1. */
static Kind kind_of(bool u, unsigned opcode, bool scalar)
{
	switch (opcode) {
	case 0x00: /* SSHR, USHR */
	case 0x02: /* SSRA, USRA */
	case 0x04: /* SRSHR, URSHR */
	case 0x06: /* SRSRA, URSRA */
		return RIGHT;
	case 0x08: /* SRI */
		return u ? INSERT : NONE;
	case 0x0a: /* SHL, SLI */
		return u ? INSERT : LEFT;
	case 0x0c: /* SQSHLU */
		return u ? SAT : NONE;
	case 0x0e: /* SQSHL, UQSHL (immediate) */
		return SAT;
	case 0x10: /* SHRN; SQSHRUN */
	case 0x11: /* RSHRN; SQRSHRUN */
		return u || !scalar ? NARROW : NONE;
	case 0x12: /* SQSHRN; UQSHRN */
	case 0x13: /* SQRSHRN; UQRSHRN */
		return NARROW;
	case 0x14: /* SSHLL, USHLL */
		return scalar ? NONE : LONG;
	case 0x1c: /* SCVTF, UCVTF */
	case 0x1f: /* FCVTZS, FCVTZU */
		return FIXED;
	default:
		return NONE;
	}
}

/*
 * One element of size bytes of a right shift by shift, 1 to 8 * size,
 * rounded for SRSHR and the other R forms: (element + round) >> shift in
 * an integer wide enough for it, as the pseudocode has it.
 */
static int128 shift_right(uint64_t element, unsigned size, bool is_unsigned,
                          unsigned shift, bool round)
{
	int128 value = to_int(element, size, is_unsigned);

	if (round)
		value += (int128)1 << (shift - 1);
	return value >> shift;
}

/*
 * One element of a RIGHT, INSERT, LEFT or SAT instruction. acc is the
 * element of Vd, which the accumulating and inserting forms read.
 */
static uint64_t element_op(Arm64Cpu *c, Kind kind, bool u, unsigned opcode,
                           uint64_t element, uint64_t acc, unsigned size,
                           unsigned shift)
{
	uint64_t mask = bytes_mask(size);

	switch (kind) {
	case RIGHT: {
		uint64_t value =
			(uint64_t)shift_right(element, size, u, shift, opcode & 4);

		return ((opcode & 2) ? acc + value : value) & mask;
	}
	case INSERT: {
		/* SRI: LSR(Ones(esize), shift); SLI: LSL(Ones(esize), shift). */
		uint64_t keep, value;

		if (opcode == 0x08) {
			keep = shift == 8 * size ? 0 : mask >> shift;
			value = shift == 8 * size ? 0 : (element & mask) >> shift;
		} else {
			keep = (mask << shift) & mask;
			value = element << shift;
		}
		return ((acc & ~keep) | (value & keep)) & mask;
	}
	case LEFT:
		return (element << shift) & mask;
	default: /* SAT: SQSHLU reads a signed element, saturates unsigned */
		return saturate(c,
		                to_int(element, size, u && opcode == 0x0e) *
		                    ((int128)1 << shift),
		                size, u);
	}
}

/* One narrowed element of size bytes, from one of 2 * size bytes. */
static uint64_t narrow(Arm64Cpu *c, bool u, unsigned opcode, uint64_t element,
                       unsigned size, unsigned shift)
{
	bool round = opcode & 1;

	if (opcode < 0x12 && !u) /* SHRN, RSHRN: truncate */
		return (uint64_t)shift_right(element, 2 * size, true, shift, round) &
		       bytes_mask(size);
	/* SQSHRUN, SQRSHRUN: signed to unsigned; else as U says. */
	return saturate(
		c, shift_right(element, 2 * size, u && opcode >= 0x12, shift, round),
		size, u);
}

Arm64Exec arm64_simd_shift_imm(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), u = bit32(insn, 29);
	unsigned immh = field32(insn, 22, 19), immhb = field32(insn, 22, 16);
	unsigned opcode = field32(insn, 15, 11);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	Kind kind = kind_of(u, opcode, scalar);
	unsigned log2 = 0;
	Arm64Exec result;

	for (unsigned bit = immh; bit > 1; bit >>= 1)
		log2++; /* HighestSetBit(immh) */

	unsigned esize = 1u << log2; /* in bytes */
	unsigned datasize = q ? 16 : 8;

	if (kind == NONE || immh == 0)
		return ARM64_EXEC_UNDEFINED;
	if (kind == FIXED) {
		/* immh = 001x is half precision, FEAT_FP16; 000x reserved. */
		if (immh < 4 || (!scalar && (immh & 8) && !q))
			return ARM64_EXEC_UNDEFINED;
		return arm64_simd_not_yet(c, scalar
		                                 ? "Advanced SIMD scalar shift by "
		                                   "immediate"
		                                 : "Advanced SIMD shift by immediate");
	}
	if (kind == NARROW || kind == LONG) {
		if (immh & 8) /* no doubleword to narrow, or to widen */
			return ARM64_EXEC_UNDEFINED;
	} else if (scalar ? kind != SAT && !(immh & 8) : (immh & 8) && !q) {
		/* The scalar forms other than SAT shift doublewords only. */
		return ARM64_EXEC_UNDEFINED;
	}
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	/* Right shifts by 2 * esize - immh:immb, left by immh:immb - esize. */
	bool right = kind == RIGHT || kind == NARROW || opcode == 0x08;
	unsigned shift = right ? 16 * esize - immhb : immhb - 8 * esize;
	const Arm64Vreg op = c->v[n], acc = c->v[d];
	Arm64Vreg r = {{0, 0}};

	if (scalar && kind == NARROW) {
		set_scalar(
			c, d, esize,
			narrow(c, u, opcode, vreg_elem(&op, 0, 2 * esize), esize, shift));
		return ARM64_EXEC_OK;
	}
	if (scalar) {
		set_scalar(c, d, esize,
		           element_op(c, kind, u, opcode, vreg_elem(&op, 0, esize),
		                      vreg_elem(&acc, 0, esize), esize, shift));
		return ARM64_EXEC_OK;
	}
	if (kind == NARROW) {
		/* Into the lower half, or for SHRN2 and the others the upper. */
		for (unsigned e = 0; e < 8 / esize; e++)
			vreg_set_elem(&r, e, esize,
			              narrow(c, u, opcode, vreg_elem(&op, e, 2 * esize),
			                     esize, shift));
		if (q)
			c->v[d].d[1] = r.d[0];
		else
			set_v(c, d, r.d[0], 0);
		return ARM64_EXEC_OK;
	}
	if (kind == LONG) {
		/* The lower half, or for SSHLL2 and USHLL2 the upper, widened. */
		for (unsigned e = 0; e < 8 / esize; e++) {
			int128 value = to_int(
				vreg_elem(&op, (q ? 8 / esize : 0) + e, esize), esize, u);

			vreg_set_elem(&r, e, 2 * esize, (uint64_t)value << shift);
		}
		set_v(c, d, r.d[0], r.d[1]);
		return ARM64_EXEC_OK;
	}
	for (unsigned e = 0; e < datasize / esize; e++)
		vreg_set_elem(&r, e, esize,
		              element_op(c, kind, u, opcode, vreg_elem(&op, e, esize),
		                         vreg_elem(&acc, e, esize), esize, shift));
	set_v(c, d, r.d[0], q ? r.d[1] : 0);
	return ARM64_EXEC_OK;
}
