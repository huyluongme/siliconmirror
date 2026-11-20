/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The integer instructions of Advanced SIMD three same and of its scalar
 * form, two classes of C4.1 that simd.c gives to this file: 0 Q U 01110
 * size 1 Rm opcode 1 Rn Rd and 01 U 11110 size 1 Rm opcode 1 Rn Rd. Each
 * instruction follows its description in C7.2. The floating-point
 * instructions of the classes, opcodes 11xxx, stop the machine as not
 * implemented yet; those newer than Armv8.0 (FEAT_FHM, FEAT_FAMINMAX) are
 * UNDEFINED.
 *
 * A saturating instruction sets FPSR.QC when a result saturates.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

__extension__ typedef __int128 int128;

#define FPSR_QC (UINT32_C(1) << 27)

/* The key of an instruction of the class: U and opcode. */
#define KEY(u, opcode) ((u) << 5 | (opcode))

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

/*
 * The shifts by register, SSHL to UQRSHL: element << SInt(shift[7:0]),
 * a right shift when that is negative, rounded when round is set, as an
 * integer wide enough for it; saturated when sat is set, else truncated.
 * A left shift by the element size or more leaves no bit of a truncated
 * result and saturates any other value than 0; a right shift by more
 * than 64 gives what one by 65 gives, to which it is clamped.
 */
static uint64_t shift_register(Arm64Cpu *c, uint64_t element, uint64_t shift,
                               unsigned size, bool u, bool round, bool sat)
{
	int amount = (int8_t)(shift & 0xff);
	int128 value = to_int(element, size, u);

	if (amount >= (int)(8 * size)) {
		if (!sat || value == 0)
			return 0;
		value = value > 0 ? (int128)1 << 100 : -((int128)1 << 100);
	} else if (amount >= 0) {
		value *= (int128)1 << amount;
	} else {
		int right = -amount > 65 ? 65 : -amount;

		if (round)
			value += (int128)1 << (right - 1);
		value >>= right;
	}
	return sat ? saturate(c, value, size, u)
	           : (uint64_t)value & bytes_mask(size);
}

/* PolynomialMult() of J1.3 for bytes, the low 8 bits of the product. */
static uint64_t poly_mul(uint64_t a, uint64_t b)
{
	uint64_t result = 0;

	for (unsigned i = 0; i < 8; i++)
		if ((b >> i) & 1)
			result ^= a << i;
	return result & 0xff;
}

/*
 * SQDMULH and SQRDMULH: the high half of twice the product, rounded for
 * SQRDMULH, saturated.
 */
uint64_t arm64_simd_doubling_mul_high(Arm64Cpu *c, uint64_t a, uint64_t b,
                                      unsigned size, bool round)
{
	int128 product = 2 * to_int(a, size, false) * to_int(b, size, false);

	if (round)
		product += (int128)1 << (8 * size - 1);
	return saturate(c, product >> (8 * size), size, false);
}

/*
 * One element of size bytes of an instruction that works lane by lane:
 * a from Vn, b from Vm, acc from Vd.
 */
static uint64_t element_op(Arm64Cpu *c, unsigned key, uint64_t a, uint64_t b,
                           uint64_t acc, unsigned size)
{
	bool u = key >> 5;
	int128 x = to_int(a, size, u), y = to_int(b, size, u);
	uint64_t mask = bytes_mask(size);

	switch (key & 0x1f) {
	case 0x00: /* SHADD, UHADD */
		return (uint64_t)((x + y) >> 1) & mask;
	case 0x01: /* SQADD, UQADD */
		return saturate(c, x + y, size, u);
	case 0x02: /* SRHADD, URHADD */
		return (uint64_t)((x + y + 1) >> 1) & mask;
	case 0x04: /* SHSUB, UHSUB */
		return (uint64_t)((x - y) >> 1) & mask;
	case 0x05: /* SQSUB, UQSUB */
		return saturate(c, x - y, size, u);
	case 0x06: /* CMGT, CMHI (register) */
		return x > y ? mask : 0;
	case 0x07: /* CMGE, CMHS (register) */
		return x >= y ? mask : 0;
	case 0x08: /* SSHL, USHL */
	case 0x09: /* SQSHL, UQSHL (register) */
	case 0x0a: /* SRSHL, URSHL */
	case 0x0b: /* SQRSHL, UQRSHL */
		return shift_register(c, a, b, size, u, key & 2, key & 1);
	case 0x0c: /* SMAX, UMAX */
		return (uint64_t)(x > y ? x : y) & mask;
	case 0x0d: /* SMIN, UMIN */
		return (uint64_t)(x < y ? x : y) & mask;
	case 0x0e: /* SABD, UABD */
		return (uint64_t)(x > y ? x - y : y - x) & mask;
	case 0x0f: /* SABA, UABA */
		return (acc + (uint64_t)(x > y ? x - y : y - x)) & mask;
	case 0x10: /* ADD, SUB (vector) */
		return (u ? a - b : a + b) & mask;
	case 0x11: /* CMTST, CMEQ (register) */
		return (u ? (a & mask) == (b & mask) : (a & b & mask) != 0) ? mask : 0;
	case 0x12: /* MLA, MLS (vector) */
		return (u ? acc - a * b : acc + a * b) & mask;
	case 0x13: /* MUL (vector), PMUL */
		return u ? poly_mul(a, b) : (a * b) & mask;
	default: /* 0x16: SQDMULH, SQRDMULH (vector) */
		return arm64_simd_doubling_mul_high(c, a, b, size, u);
	}
}

/*
 * Whether opcode, 11xxx, is a floating-point instruction of Armv8.0 in
 * the vector class, from its table in C4.1; size[1] is part of the
 * opcode and size[0] is sz.
 */
static bool fp_same(bool u, unsigned size, unsigned opcode)
{
	bool high = size & 2;

	switch (opcode) {
	case 0x19: /* FMLA, FMLS; FMLAL2 (FEAT_FHM) with U = 1 */
	case 0x1b: /* FMULX, FAMAX; FMUL, FAMIN (FEAT_FAMINMAX) */
		return !u ? opcode == 0x19 || !high : !high && opcode == 0x1b;
	case 0x1c: /* FCMEQ; FCMGE, FCMGT */
		return u || !high;
	case 0x1d: /* FMLAL, FMLSL (FEAT_FHM); FACGE, FACGT */
		return u;
	case 0x1f: /* FRECPS, FRSQRTS; FDIV */
		return !u || !high;
	default: /* 11000, 11010, 11110: all of them */
		return true;
	}
}

/* The same for the scalar class. */
static bool fp_scalar_same(bool u, unsigned size, unsigned opcode)
{
	bool high = size & 2;

	switch (opcode) {
	case 0x1b: /* FMULX */
		return !u && !high;
	case 0x1c: /* FCMEQ; FCMGE, FCMGT */
		return u || !high;
	case 0x1d: /* FACGE, FACGT */
		return u;
	case 0x1a: /* FABD */
		return u && high;
	case 0x1f: /* FRECPS, FRSQRTS */
		return !u;
	default:
		return false;
	}
}

/* Advanced SIMD scalar three same, on the elements at the bottom. */
static Arm64Exec scalar_same(Arm64Cpu *c, bool u, unsigned size,
                             unsigned opcode, unsigned m, unsigned n,
                             unsigned d)
{
	unsigned key = KEY(u, opcode), esize = 1u << size;
	Arm64Exec result;

	switch (opcode) {
	case 0x01: /* SQADD, UQADD */
	case 0x05: /* SQSUB, UQSUB */
	case 0x09: /* SQSHL, UQSHL (register) */
	case 0x0b: /* SQRSHL, UQRSHL */
		break;
	case 0x06: /* CMGT, CMHI */
	case 0x07: /* CMGE, CMHS */
	case 0x08: /* SSHL, USHL */
	case 0x0a: /* SRSHL, URSHL */
	case 0x10: /* ADD, SUB */
	case 0x11: /* CMTST, CMEQ */
		if (size != 3)
			return ARM64_EXEC_UNDEFINED;
		break;
	case 0x16: /* SQDMULH, SQRDMULH: halfwords and words */
		if (size == 0 || size == 3)
			return ARM64_EXEC_UNDEFINED;
		break;
	default:
		return ARM64_EXEC_UNDEFINED;
	}
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	set_scalar(c, d, esize,
	           element_op(c, key, get_elem(c, n, 0, esize),
	                      get_elem(c, m, 0, esize), get_elem(c, d, 0, esize),
	                      esize));
	return ARM64_EXEC_OK;
}

/*
 * The logical instructions, opcode 00011: AND, BIC, ORR and ORN, and EOR,
 * BSL, BIT and BIF, which size picks, after the VBitOps pseudocode.
 */
static uint64_t logical(bool u, unsigned size, uint64_t n, uint64_t m,
                        uint64_t d)
{
	if (!u) {
		switch (size) {
		case 0:
			return n & m;
		case 1:
			return n & ~m;
		case 2:
			return n | m;
		default:
			return n | ~m;
		}
	}
	switch (size) {
	case 0: /* EOR */
		return m ^ n;
	case 1: /* BSL: Vd selects Vn over Vm */
		return m ^ ((m ^ n) & d);
	case 2: /* BIT: Vm selects Vn over Vd */
		return d ^ ((d ^ n) & m);
	default: /* BIF */
		return d ^ ((d ^ n) & ~m);
	}
}

Arm64Exec arm64_simd_three_same(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), u = bit32(insn, 29);
	unsigned size = field32(insn, 23, 22), m = field32(insn, 20, 16);
	unsigned opcode = field32(insn, 15, 11);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	unsigned key = KEY(u, opcode), esize = 1u << size;
	unsigned datasize = q ? 16 : 8, elements = datasize / esize;
	Arm64Exec result;

	if (opcode >= 0x18) { /* floating-point */
		if (scalar ? fp_scalar_same(u, size, opcode) : fp_same(u, size, opcode))
			return arm64_simd_fp_same(c, insn, scalar);
		return ARM64_EXEC_UNDEFINED;
	}
	if (scalar)
		return scalar_same(c, u, size, opcode, m, n, d);

	switch (key) {
	case KEY(0, 0x03): /* the logical ones */
	case KEY(1, 0x03):
		break;
	case KEY(0, 0x01):       /* SQADD */
	case KEY(1, 0x01):       /* UQADD */
	case KEY(0, 0x05):       /* SQSUB */
	case KEY(1, 0x05):       /* UQSUB */
	case KEY(0, 0x06):       /* CMGT */
	case KEY(1, 0x06):       /* CMHI */
	case KEY(0, 0x07):       /* CMGE */
	case KEY(1, 0x07):       /* CMHS */
	case KEY(0, 0x08):       /* SSHL */
	case KEY(1, 0x08):       /* USHL */
	case KEY(0, 0x09):       /* SQSHL */
	case KEY(1, 0x09):       /* UQSHL */
	case KEY(0, 0x0a):       /* SRSHL */
	case KEY(1, 0x0a):       /* URSHL */
	case KEY(0, 0x0b):       /* SQRSHL */
	case KEY(1, 0x0b):       /* UQRSHL */
	case KEY(0, 0x10):       /* ADD */
	case KEY(1, 0x10):       /* SUB */
	case KEY(0, 0x11):       /* CMTST */
	case KEY(1, 0x11):       /* CMEQ */
	case KEY(0, 0x17):       /* ADDP */
		if (size == 3 && !q) /* size:Q = 110 */
			return ARM64_EXEC_UNDEFINED;
		break;
	case KEY(1, 0x13): /* PMUL: bytes only */
		if (size != 0)
			return ARM64_EXEC_UNDEFINED;
		break;
	case KEY(0, 0x16): /* SQDMULH */
	case KEY(1, 0x16): /* SQRDMULH */
		if (size == 0 || size == 3)
			return ARM64_EXEC_UNDEFINED;
		break;
	case KEY(1, 0x17):
		return ARM64_EXEC_UNDEFINED;
	default:
		/* The halving ones, MAX, MIN, ABD, ABA, MLA, MLS, MUL, the
		 * pairwise MAXP and MINP: no doublewords. */
		if (size == 3)
			return ARM64_EXEC_UNDEFINED;
		break;
	}
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	const Arm64Vreg vn = c->v[n], vm = c->v[m], vd = c->v[d];
	Arm64Vreg r = {{0, 0}};

	if (opcode == 0x03) {
		for (unsigned i = 0; i < 2; i++)
			r.d[i] = logical(u, size, vn.d[i], vm.d[i], vd.d[i]);
	} else if (opcode == 0x14 || opcode == 0x15 || opcode == 0x17) {
		/*
		 * The pairwise ones, SMAXP to ADDP: on the pairs of Vm:Vn,
		 * those of Vn first.
		 */
		for (unsigned e = 0; e < elements; e++) {
			const Arm64Vreg *src = e < elements / 2 ? &vn : &vm;
			unsigned i = 2 * (e % (elements / 2));
			uint64_t a = vreg_elem(src, i, esize),
					 b = vreg_elem(src, i + 1, esize);
			int128 x = to_int(a, esize, u), y = to_int(b, esize, u);
			uint64_t value = opcode == 0x17   ? a + b
			                 : opcode == 0x14 ? (uint64_t)(x > y ? x : y)
			                                  : (uint64_t)(x < y ? x : y);

			vreg_set_elem(&r, e, esize, value);
		}
	} else {
		for (unsigned e = 0; e < elements; e++)
			vreg_set_elem(&r, e, esize,
			              element_op(c, key, vreg_elem(&vn, e, esize),
			                         vreg_elem(&vm, e, esize),
			                         vreg_elem(&vd, e, esize), esize));
	}
	set_v(c, d, r.d[0], q ? r.d[1] : 0);
	return ARM64_EXEC_OK;
}
