/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The integer instructions of Advanced SIMD three different and x indexed
 * element, vector and scalar: four classes of C4.1 that simd.c gives to
 * this file, most of whose instructions make results twice as wide as
 * their operands, or half. Each instruction follows its description in
 * C7.2. The floating-point instructions by element stop the machine as
 * not implemented yet; those newer than Armv8.0 (FEAT_FP16, FEAT_RDM,
 * FEAT_DotProd, FEAT_FCMA and others) and PMULL of doublewords, which
 * needs the Cryptographic Extension, are UNDEFINED.
 *
 * A saturating instruction sets FPSR.QC when a result saturates.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

__extension__ typedef __int128 int128;

#define FPSR_QC (UINT32_C(1) << 27)

#define KEY(u, opcode) ((u) << 4 | (opcode))

static int128 to_int(uint64_t value, unsigned size, bool is_unsigned)
{
	value &= bytes_mask(size);
	return is_unsigned ? (int128)value
	                   : (int128)(int64_t)sign_extend64(value, 8 * size);
}

/* SatQ() of J1.3, setting FPSR.QC when it saturates. */
static int128 saturate(Arm64Cpu *c, int128 value, unsigned size)
{
	int128 high = ((int128)1 << (8 * size - 1)) - 1;

	if (value > high || value < -high - 1) {
		value = value > high ? high : -high - 1;
		c->fpsr |= FPSR_QC;
	}
	return value;
}

/* PolynomialMult() of J1.3 for bytes: the 16 bits of the product. */
static uint64_t poly_mul(uint64_t a, uint64_t b)
{
	uint64_t result = 0;

	for (unsigned i = 0; i < 8; i++)
		if ((b >> i) & 1)
			result ^= (a & 0xff) << i;
	return result;
}

/*
 * The operations that make an element of 2 * size bytes from a and b of
 * size bytes and acc, the element of Vd: the long forms of three
 * different (opcodes 0101, 0111 to 1101) and by element (0010, 0011, 0110,
 * 0111, 1010, 1011), by their three-different opcode.
 */
static uint64_t long_op(Arm64Cpu *c, unsigned opcode, bool u, uint64_t a,
                        uint64_t b, uint64_t acc, unsigned size)
{
	int128 x = to_int(a, size, u), y = to_int(b, size, u);
	int128 product = x * y, value;

	switch (opcode) {
	case 0x5: /* SABAL, UABAL */
		value = acc + (x > y ? x - y : y - x);
		break;
	case 0x7: /* SABDL, UABDL */
		value = x > y ? x - y : y - x;
		break;
	case 0x8: /* SMLAL, UMLAL */
		value = acc + product;
		break;
	case 0xa: /* SMLSL, UMLSL */
		value = acc - product;
		break;
	case 0xc: /* SMULL, UMULL */
		value = product;
		break;
	case 0xd: /* SQDMULL */
		value = saturate(c, 2 * product, 2 * size);
		break;
	case 0xe: /* PMULL */
		value = poly_mul(a, b);
		break;
	default: { /* 0x9 SQDMLAL, 0xb SQDMLSL: two saturations */
		int128 doubled = saturate(c, 2 * product, 2 * size);
		int128 accum = to_int(acc, 2 * size, false);

		value = saturate(c, opcode == 0x9 ? accum + doubled : accum - doubled,
		                 2 * size);
		break;
	}
	}
	return (uint64_t)value & bytes_mask(2 * size);
}

/* The 64 bits of Vn that a long form reads: Vpart(n, part). */
static Arm64Vreg half(const Arm64Cpu *c, unsigned n, bool upper)
{
	Arm64Vreg v = {{c->v[n].d[upper ? 1 : 0], 0}};

	return v;
}

/*
 * Advanced SIMD three different, 0 Q U 01110 size 1 Rm opcode 00 Rn Rd,
 * and its scalar form, 01 U 11110 size 1 Rm opcode 00 Rn Rd (SQDMLAL,
 * SQDMLSL and SQDMULL). The forms named 2 use the upper halves, Q = 1.
 */
Arm64Exec arm64_simd_three_different(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), u = bit32(insn, 29);
	unsigned size = field32(insn, 23, 22), m = field32(insn, 20, 16);
	unsigned opcode = field32(insn, 15, 12);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	unsigned esize = 1u << size, elements = 8 / esize;
	bool doubling = !u && (opcode == 0x9 || opcode == 0xb || opcode == 0xd);
	Arm64Exec result;

	if (scalar ? !doubling
	           : (opcode == 0xf ||
	              (u && opcode >= 0x8 && (opcode & 1 || opcode == 0xe))))
		return ARM64_EXEC_UNDEFINED;
	if (opcode == 0xe) /* PMULL: of bytes; of doublewords is FEAT_PMULL */
		size = size == 0 ? 0 : 3;
	else if (doubling && size == 0)
		size = 3;
	if (size == 3)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	if (scalar) {
		set_scalar(c, d, 2 * esize,
		           long_op(c, opcode, false, get_elem(c, n, 0, esize),
		                   get_elem(c, m, 0, esize),
		                   get_elem(c, d, 0, 2 * esize), esize));
		return ARM64_EXEC_OK;
	}

	const Arm64Vreg vn = c->v[n], vm = c->v[m], vd = c->v[d];
	const Arm64Vreg hn = half(c, n, q), hm = half(c, m, q);
	Arm64Vreg r = {{0, 0}};

	if (opcode == 0x4 || opcode == 0x6) {
		/* ADDHN, SUBHN and the rounding RADDHN, RSUBHN: the high halves. */
		for (unsigned e = 0; e < elements; e++) {
			uint64_t a = vreg_elem(&vn, e, 2 * esize);
			uint64_t b = vreg_elem(&vm, e, 2 * esize);
			int128 value = (int128)a + (opcode == 0x4 ? (int128)b : -(int128)b);

			if (u)
				value += (int128)1 << (8 * esize - 1);
			vreg_set_elem(&r, e, esize, (uint64_t)(value >> (8 * esize)));
		}
		if (q)
			c->v[d].d[1] = r.d[0];
		else
			set_v(c, d, r.d[0], 0);
		return ARM64_EXEC_OK;
	}
	for (unsigned e = 0; e < elements; e++) {
		uint64_t b = vreg_elem(&hm, e, esize),
				 acc = vreg_elem(&vd, e, 2 * esize);
		uint64_t value;

		if (opcode < 0x4) {
			/* SADDL, SSUBL; the wide SADDW, SSUBW take all of Vn. */
			int128 x = opcode & 1
			               ? to_int(vreg_elem(&vn, e, 2 * esize), 2 * esize, u)
			               : to_int(vreg_elem(&hn, e, esize), esize, u);
			int128 y = to_int(b, esize, u);

			value = (uint64_t)(opcode & 2 ? x - y : x + y);
		} else {
			value =
				long_op(c, opcode, u, vreg_elem(&hn, e, esize), b, acc, esize);
		}
		vreg_set_elem(&r, e, 2 * esize, value);
	}
	set_v(c, d, r.d[0], r.d[1]);
	return ARM64_EXEC_OK;
}

/*
 * The integer opcodes by element, vector: what three-different opcode
 * each long form has, or 0 for one that is not long.
 */
static unsigned long_opcode(bool u, unsigned opcode)
{
	switch (opcode) {
	case 0x2: /* SMLAL, UMLAL */
		return 0x8;
	case 0x3: /* SQDMLAL */
		return u ? 0 : 0x9;
	case 0x6: /* SMLSL, UMLSL */
		return 0xa;
	case 0x7: /* SQDMLSL */
		return u ? 0 : 0xb;
	case 0xa: /* SMULL, UMULL */
		return 0xc;
	case 0xb: /* SQDMULL */
		return u ? 0 : 0xd;
	default:
		return 0;
	}
}

/*
 * Advanced SIMD x indexed element, 0 Q U 01111 size L M Rm opcode H 0 Rn
 * Rd, and its scalar form, 01 U 11111 size L M Rm opcode H 0 Rn Rd. The
 * element of Vm is H:L:M of V0 to V15 for halfwords, H:L of V0 to V31
 * for words.
 */
Arm64Exec arm64_simd_by_element(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), u = bit32(insn, 29);
	unsigned size = field32(insn, 23, 22), opcode = field32(insn, 15, 12);
	unsigned h = bit32(insn, 11), l = bit32(insn, 21), mbit = bit32(insn, 20);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	unsigned key = KEY(u, opcode), lop = long_opcode(u, opcode);
	unsigned esize = 1u << size;
	Arm64Exec result;
	bool fp;

	/* The floating-point ones of Armv8.0: FMLA, FMLS, FMUL, FMULX. */
	fp = (size & 2) && (opcode == 0x1 || opcode == 0x5 || opcode == 0x9) &&
	     (!u || opcode == 0x9);
	if (fp)
		return arm64_simd_not_yet(c, scalar ? "Advanced SIMD scalar x indexed "
		                                      "element"
		                                    : "Advanced SIMD vector x indexed "
		                                      "element");
	if (scalar) {
		/* SQDMLAL, SQDMLSL, SQDMULL, SQDMULH and SQRDMULH. */
		if (u || !(lop == 0x9 || lop == 0xb || lop == 0xd || opcode == 0xc ||
		           opcode == 0xd))
			return ARM64_EXEC_UNDEFINED;
	} else if (!(lop != 0 || key == KEY(1, 0x0) || key == KEY(1, 0x4) ||
	             key == KEY(0, 0x8) || key == KEY(0, 0xc) ||
	             key == KEY(0, 0xd))) {
		/* Others: UNALLOCATED, or FEAT_RDM, FEAT_DotProd and newer. */
		return ARM64_EXEC_UNDEFINED;
	}
	if (size == 0 || size == 3) /* halfwords and words only */
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	unsigned m = field32(insn, 19, 16) | (size == 2 ? mbit << 4 : 0);
	unsigned index = size == 1 ? h << 2 | l << 1 | mbit : h << 1 | l;
	uint64_t element = get_elem(c, m, index, esize);
	const Arm64Vreg vn = c->v[n], vd = c->v[d];
	Arm64Vreg r = {{0, 0}};

	if (scalar) {
		if (lop != 0)
			set_scalar(c, d, 2 * esize,
			           long_op(c, lop, false, vreg_elem(&vn, 0, esize), element,
			                   vreg_elem(&vd, 0, 2 * esize), esize));
		else
			set_scalar(c, d, esize,
			           arm64_simd_doubling_mul_high(c, vreg_elem(&vn, 0, esize),
			                                        element, esize,
			                                        opcode & 1));
		return ARM64_EXEC_OK;
	}
	if (lop != 0) {
		/* The long forms: the lower half of Vn, or the upper for the 2. */
		const Arm64Vreg hn = half(c, n, q);

		for (unsigned e = 0; e < 8 / esize; e++)
			vreg_set_elem(&r, e, 2 * esize,
			              long_op(c, lop, u, vreg_elem(&hn, e, esize), element,
			                      vreg_elem(&vd, e, 2 * esize), esize));
		set_v(c, d, r.d[0], r.d[1]);
		return ARM64_EXEC_OK;
	}
	for (unsigned e = 0; e < (q ? 16 : 8) / esize; e++) {
		uint64_t a = vreg_elem(&vn, e, esize), acc = vreg_elem(&vd, e, esize);
		uint64_t value;

		switch (opcode) {
		case 0x0: /* MLA */
			value = acc + a * element;
			break;
		case 0x4: /* MLS */
			value = acc - a * element;
			break;
		case 0x8: /* MUL */
			value = a * element;
			break;
		default: /* SQDMULH, SQRDMULH */
			value =
				arm64_simd_doubling_mul_high(c, a, element, esize, opcode & 1);
			break;
		}
		vreg_set_elem(&r, e, esize, value);
	}
	set_v(c, d, r.d[0], q ? r.d[1] : 0);
	return ARM64_EXEC_OK;
}
