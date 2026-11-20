/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The floating-point instructions of Advanced SIMD, vector and scalar,
 * on single and double precision (half precision is FEAT_FP16, but in
 * FCVTN and FCVTL): the three same, two-register miscellaneous, across
 * lanes, pairwise, shift by immediate (fixed point) and x indexed element
 * classes. The callers, in simd_same.c, simd_int.c, simd_shift.c and
 * simd_long.c, have checked that the encoding is one of them; size<1>,
 * "a", picks between two instructions, and size<0>, "sz", the precision.
 * A vector of doublewords needs Q = 1. The arithmetic is fp.c's.
 */

#include "cpu/arm64/fp.h"
#include "cpu/arm64/internal.h"

static ArmFp fp_begin(const Arm64Cpu *c)
{
	return (ArmFp){c->fpcr, 0};
}

static void fp_end(Arm64Cpu *c, const ArmFp *f)
{
	c->fpsr |= f->flags;
}

/* All ones in an element of bits bits when holds, else 0. */
static uint64_t truth(bool holds, unsigned bits)
{
	return holds ? datasize_mask(bits) : 0;
}

/*
 * The three same operations, by U:a:opcode<2:0>; the pairwise ones (U = 1,
 * opcodes 11000, 11010, 11110) take their two elements from a pair.
 */
enum {
	FMAXNM = 0x00,
	FMLA = 0x01,
	FADD = 0x02,
	FMULX = 0x03,
	FCMEQ = 0x04,
	FMAX = 0x06,
	FRECPS = 0x07,
	FMINNM = 0x08,
	FMLS = 0x09,
	FSUB = 0x0a,
	FMIN = 0x0e,
	FRSQRTS = 0x0f,
	FMAXNMP = 0x10,
	FADDP = 0x12,
	FMUL = 0x13,
	FCMGE = 0x14,
	FACGE = 0x15,
	FMAXP = 0x16,
	FDIV = 0x17,
	FMINNMP = 0x18,
	FCMGT = 0x1c, /* and 0x1a, FABD, the default */
	FACGT = 0x1d,
	FMINP = 0x1e,
};

static uint64_t same_op(ArmFp *f, unsigned key, unsigned bits, uint64_t a,
                        uint64_t b, uint64_t acc)
{
	switch (key) {
	case FMAXNM:
	case FMAXNMP:
		return fp_maxnum(f, bits, a, b);
	case FMINNM:
	case FMINNMP:
		return fp_minnum(f, bits, a, b);
	case FMLA:
		return fp_muladd(f, bits, acc, a, b);
	case FMLS:
		return fp_muladd(f, bits, acc, fp_neg(bits, a), b);
	case FADD:
	case FADDP:
		return fp_add(f, bits, a, b);
	case FSUB:
		return fp_sub(f, bits, a, b);
	case FMULX:
		return fp_mulx(f, bits, a, b);
	case FCMEQ:
		return truth(fp_compare_eq(f, bits, a, b), bits);
	case FMAX:
	case FMAXP:
		return fp_max(f, bits, a, b);
	case FMIN:
	case FMINP:
		return fp_min(f, bits, a, b);
	case FRECPS:
		return fp_recip_step(f, bits, a, b);
	case FRSQRTS:
		return fp_rsqrt_step(f, bits, a, b);
	case FMUL:
		return fp_mul(f, bits, a, b);
	case FCMGE:
		return truth(fp_compare_ge(f, bits, a, b), bits);
	case FCMGT:
		return truth(fp_compare_gt(f, bits, a, b), bits);
	case FACGE:
		return truth(fp_compare_ge(f, bits, fp_abs(bits, a), fp_abs(bits, b)),
		             bits);
	case FACGT:
		return truth(fp_compare_gt(f, bits, fp_abs(bits, a), fp_abs(bits, b)),
		             bits);
	case FDIV:
		return fp_div(f, bits, a, b);
	default: /* FABD */
		return fp_abs(bits, fp_sub(f, bits, a, b));
	}
}

static bool pairwise(unsigned key)
{
	return key == FMAXNMP || key == FMINNMP || key == FADDP || key == FMAXP ||
	       key == FMINP;
}

/*
 * Advanced SIMD three same and scalar three same, opcodes 11xxx: 0 Q U
 * 01110 a sz 1 Rm opcode 1 Rn Rd.
 */
Arm64Exec arm64_simd_fp_same(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), sz = bit32(insn, 22);
	unsigned key =
		bit32(insn, 29) << 4 | bit32(insn, 23) << 3 | field32(insn, 13, 11);
	unsigned bits = sz ? 64 : 32, elements = scalar ? 1 : (q ? 128 : 64) / bits;
	unsigned d = field32(insn, 4, 0);
	Arm64Exec result;
	Arm64Vreg r = {{0, 0}};
	ArmFp f;

	if (!scalar && sz && !q)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	const Arm64Vreg vn = c->v[field32(insn, 9, 5)];
	const Arm64Vreg vm = c->v[field32(insn, 20, 16)], vd = c->v[d];

	f = fp_begin(c);
	for (unsigned e = 0; e < elements; e++) {
		uint64_t a, b;

		if (pairwise(key)) { /* of Vm:Vn */
			const Arm64Vreg *v = e < elements / 2 ? &vn : &vm;
			unsigned i = 2 * (e % (elements / 2));

			a = vreg_elem(v, i, bits / 8);
			b = vreg_elem(v, i + 1, bits / 8);
		} else {
			a = vreg_elem(&vn, e, bits / 8);
			b = vreg_elem(&vm, e, bits / 8);
		}
		vreg_set_elem(
			&r, e, bits / 8,
			same_op(&f, key, bits, a, b, vreg_elem(&vd, e, bits / 8)));
	}
	if (scalar)
		set_scalar(c, d, bits / 8, r.d[0]);
	else
		c->v[d] = r; /* a 64-bit result leaves the upper half 0 */
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * FCVTN, FCVTN2 and FCVTXN, FCVTXN2: the elements of Vn, narrowed into
 * the lower half of Vd, or the upper one, keeping the lower.
 */
static void narrow(Arm64Cpu *c, uint32_t insn, unsigned bits, FpRounding r,
                   ArmFp *f)
{
	bool q = bit32(insn, 30);
	unsigned d = field32(insn, 4, 0);
	const Arm64Vreg vn = c->v[field32(insn, 9, 5)];
	Arm64Vreg half = {{0, 0}};

	for (unsigned e = 0; e < 64 / bits; e++)
		vreg_set_elem(
			&half, e, bits / 8,
			fp_convert(f, bits, 2 * bits, vreg_elem(&vn, e, bits / 4), r));
	if (q)
		c->v[d].d[1] = half.d[0];
	else
		set_v(c, d, half.d[0], 0);
}

/* FCVTL, FCVTL2: the lower half of Vn, or the upper one, widened. */
static void widen(Arm64Cpu *c, uint32_t insn, unsigned bits, ArmFp *f)
{
	Arm64Vreg half = {{c->v[field32(insn, 9, 5)].d[bit32(insn, 30)], 0}};
	Arm64Vreg r = {{0, 0}};

	for (unsigned e = 0; e < 64 / bits; e++)
		vreg_set_elem(&r, e, bits / 4,
		              fp_convert(f, 2 * bits, bits,
		                         vreg_elem(&half, e, bits / 8),
		                         fp_rounding(f->fpcr)));
	c->v[field32(insn, 4, 0)] = r;
}

/* The rounding of FRINT<N, P, M, Z, A> and FCVT<N, P, M, Z><S, U>. */
static FpRounding misc_rounding(unsigned opcode, bool a, bool u)
{
	switch (opcode) {
	case 0x18: /* FRINTN, FRINTP; FRINTA */
	case 0x1a: /* FCVTN, FCVTP */
		if (opcode == 0x18 && u)
			return FP_TIEAWAY;
		return a ? FP_POSINF : FP_TIEEVEN;
	default: /* 0x19: FRINTM, FRINTZ; 0x1b: FCVTM, FCVTZ */
		return a ? FP_ZERO : FP_NEGINF;
	}
}

/* A two-register miscellaneous operation on an element of bits bits. */
static uint64_t misc_op(ArmFp *f, unsigned opcode, bool u, bool a,
                        unsigned bits, uint64_t x)
{
	switch (opcode) {
	case 0x0c: /* FCMGT, FCMGE (zero) */
		return truth(u ? fp_compare_ge(f, bits, x, 0)
		               : fp_compare_gt(f, bits, x, 0),
		             bits);
	case 0x0d: /* FCMEQ, FCMLE (zero) */
		return truth(u ? fp_compare_ge(f, bits, 0, x)
		               : fp_compare_eq(f, bits, x, 0),
		             bits);
	case 0x0e: /* FCMLT (zero) */
		return truth(fp_compare_gt(f, bits, 0, x), bits);
	case 0x0f: /* FABS, FNEG */
		return u ? fp_neg(bits, x) : fp_abs(bits, x);
	case 0x18:
	case 0x19: /* FRINTX and FRINTI round as FPCR says */
		if (opcode == 0x19 && u)
			return fp_round_int(f, bits, x, fp_rounding(f->fpcr), !a);
		return fp_round_int(f, bits, x, misc_rounding(opcode, a, u), false);
	case 0x1c:
		if (a) /* URECPE, URSQRTE */
			return u ? fp_unsigned_rsqrt_estimate((uint32_t)x)
			         : fp_unsigned_recip_estimate((uint32_t)x);
		return fp_to_fixed(f, bits, x, 0, u, FP_TIEAWAY, bits);
	case 0x1a:
	case 0x1b:
		return fp_to_fixed(f, bits, x, 0, u, misc_rounding(opcode, a, u), bits);
	case 0x1d:
		if (a) /* FRECPE, FRSQRTE */
			return u ? fp_rsqrt_estimate(f, bits, x)
			         : fp_recip_estimate(f, bits, x);
		return fp_from_fixed(f, bits, x, bits, 0, u, fp_rounding(f->fpcr));
	default: /* 0x1f: FSQRT; FRECPX in the scalar class */
		return u ? fp_sqrt(f, bits, x) : fp_recpx(f, bits, x);
	}
}

/*
 * Advanced SIMD two-register miscellaneous and its scalar form, the
 * floating-point opcodes: 0 Q U 01110 a sz 10000 opcode 10 Rn Rd.
 */
Arm64Exec arm64_simd_fp_misc(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), u = bit32(insn, 29), a = bit32(insn, 23);
	bool sz = bit32(insn, 22);
	unsigned opcode = field32(insn, 16, 12), d = field32(insn, 4, 0);
	unsigned bits = sz ? 64 : 32, elements = scalar ? 1 : (q ? 128 : 64) / bits;
	Arm64Exec result;
	Arm64Vreg r = {{0, 0}};
	ArmFp f;

	if (!scalar && sz && !q && opcode != 0x16 && opcode != 0x17)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	f = fp_begin(c);
	if (opcode == 0x16 && scalar) { /* FCVTXN */
		set_scalar(
			c, d, 4,
			fp_convert(&f, 32, 64, get_d(c, field32(insn, 9, 5)), FP_ODD));
	} else if (opcode == 0x16) { /* FCVTN, FCVTXN */
		narrow(c, insn, sz ? 32 : 16, u ? FP_ODD : fp_rounding(c->fpcr), &f);
	} else if (opcode == 0x17) { /* FCVTL */
		widen(c, insn, sz ? 32 : 16, &f);
	} else {
		const Arm64Vreg vn = c->v[field32(insn, 9, 5)];

		for (unsigned e = 0; e < elements; e++)
			vreg_set_elem(
				&r, e, bits / 8,
				misc_op(&f, opcode, u, a, bits, vreg_elem(&vn, e, bits / 8)));
		if (scalar)
			set_scalar(c, d, bits / 8, r.d[0]);
		else
			c->v[d] = r; /* a 64-bit result leaves the upper half 0 */
	}
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/* FMAXNM, FMINNM, FMAX or FMIN, by opcode<0> (11100 or 11111) and a. */
static uint64_t min_max_op(ArmFp *f, unsigned opcode, bool a, unsigned bits,
                           uint64_t x, uint64_t y)
{
	if (opcode == 0x0c)
		return a ? fp_minnum(f, bits, x, y) : fp_maxnum(f, bits, x, y);
	return a ? fp_min(f, bits, x, y) : fp_max(f, bits, x, y);
}

/*
 * Advanced SIMD across lanes, FMAXNMV, FMINNMV, FMAXV and FMINV: 0 1 1
 * 01110 a 0 11000 opcode 10 Rn Rd, of four words, two pairs and the
 * pair of their results (Reduce()).
 */
Arm64Exec arm64_simd_fp_across(Arm64Cpu *c, uint32_t insn)
{
	unsigned opcode = field32(insn, 16, 12);
	bool a = bit32(insn, 23);
	const Arm64Vreg *v;
	uint64_t lo, hi;
	Arm64Exec result;
	ArmFp f;

	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	v = &c->v[field32(insn, 9, 5)];
	f = fp_begin(c);
	lo = min_max_op(&f, opcode, a, 32, vreg_elem(v, 0, 4), vreg_elem(v, 1, 4));
	hi = min_max_op(&f, opcode, a, 32, vreg_elem(v, 2, 4), vreg_elem(v, 3, 4));
	set_scalar(c, field32(insn, 4, 0), 4,
	           min_max_op(&f, opcode, a, 32, lo, hi));
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * Advanced SIMD scalar pairwise, FMAXNMP, FMINNMP, FADDP, FMAXP and FMINP:
 * 01 1 11110 a sz 11000 opcode 10 Rn Rd, of the two elements of Vn.
 */
Arm64Exec arm64_simd_fp_pairwise(Arm64Cpu *c, uint32_t insn)
{
	unsigned opcode = field32(insn, 16, 12), bits = bit32(insn, 22) ? 64 : 32;
	const Arm64Vreg *v;
	uint64_t x, y, r;
	Arm64Exec result;
	ArmFp f;

	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	v = &c->v[field32(insn, 9, 5)];
	x = vreg_elem(v, 0, bits / 8);
	y = vreg_elem(v, 1, bits / 8);
	f = fp_begin(c);
	if (opcode == 0x0d)
		r = fp_add(&f, bits, x, y);
	else
		r = min_max_op(&f, opcode, bit32(insn, 23), bits, x, y);
	set_scalar(c, field32(insn, 4, 0), bits / 8, r);
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * Advanced SIMD shift by immediate and its scalar form, SCVTF, UCVTF
 * (opcode 11100) and FCVTZS, FCVTZU (11111) with fraction bits: 0 Q U
 * 011110 immh immb opcode 1 Rn Rd. immh 1xxx is double precision, with
 * 128 - immh:immb fraction bits, 01xx single, with 64 - immh:immb.
 */
Arm64Exec arm64_simd_fp_fixed(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), u = bit32(insn, 29);
	unsigned immh = field32(insn, 22, 19), bits = immh & 8 ? 64 : 32;
	unsigned fbits = 2 * bits - field32(insn, 22, 16);
	unsigned elements = scalar ? 1 : (q ? 128 : 64) / bits;
	unsigned d = field32(insn, 4, 0);
	bool to_fp = field32(insn, 15, 11) == 0x1c;
	Arm64Exec result;
	Arm64Vreg r = {{0, 0}};
	ArmFp f;

	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	const Arm64Vreg vn = c->v[field32(insn, 9, 5)];

	f = fp_begin(c);
	for (unsigned e = 0; e < elements; e++) {
		uint64_t x = vreg_elem(&vn, e, bits / 8);

		vreg_set_elem(&r, e, bits / 8,
		              to_fp
		                  ? fp_from_fixed(&f, bits, x, bits, fbits, u,
		                                  fp_rounding(c->fpcr))
		                  : fp_to_fixed(&f, bits, x, fbits, u, FP_ZERO, bits));
	}
	if (scalar)
		set_scalar(c, d, bits / 8, r.d[0]);
	else
		c->v[d] = r; /* a 64-bit result leaves the upper half 0 */
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * Advanced SIMD x indexed element and its scalar form, FMLA (opcode
 * 0001), FMLS (0101), FMUL and FMULX (1001, U): 0 Q U 01111 1 sz L M Rm
 * opcode H 0 Rn Rd. The element of Vm (M:Rm) is H:L of words, H of
 * doublewords, where L must be 0.
 */
Arm64Exec arm64_simd_fp_by_element(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), u = bit32(insn, 29), sz = bit32(insn, 22);
	bool l = bit32(insn, 21), h = bit32(insn, 11);
	unsigned opcode = field32(insn, 15, 12), d = field32(insn, 4, 0);
	unsigned bits = sz ? 64 : 32, elements = scalar ? 1 : (q ? 128 : 64) / bits;
	unsigned index = sz ? h : (unsigned)(h << 1 | l);
	uint64_t b;
	Arm64Exec result;
	Arm64Vreg r = {{0, 0}};
	ArmFp f;

	if ((sz && l) || (!scalar && sz && !q))
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	const Arm64Vreg vn = c->v[field32(insn, 9, 5)], vd = c->v[d];

	b = get_elem(c, field32(insn, 20, 16), index, bits / 8);
	f = fp_begin(c);
	for (unsigned e = 0; e < elements; e++) {
		uint64_t a = vreg_elem(&vn, e, bits / 8),
				 acc = vreg_elem(&vd, e, bits / 8);
		uint64_t v;

		if (opcode == 0x1)
			v = fp_muladd(&f, bits, acc, a, b);
		else if (opcode == 0x5)
			v = fp_muladd(&f, bits, acc, fp_neg(bits, a), b);
		else if (u)
			v = fp_mulx(&f, bits, a, b);
		else
			v = fp_mul(&f, bits, a, b);
		vreg_set_elem(&r, e, bits / 8, v);
	}
	if (scalar)
		set_scalar(c, d, bits / 8, r.d[0]);
	else
		c->v[d] = r; /* a 64-bit result leaves the upper half 0 */
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}
