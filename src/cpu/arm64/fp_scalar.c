/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The scalar floating-point instructions (C4.1.96, "Data Processing --
 * Scalar Floating-Point and Advanced SIMD"): conversions to and from
 * integers and fixed point, data-processing with 1, 2 and 3 sources,
 * compares and conditional selects. The arithmetic is fp.c's. ftype 11,
 * half precision, is FEAT_FP16 but in FCVT, and ftype 10 is unallocated.
 */

#include "cpu/arm64/fp.h"
#include "cpu/arm64/internal.h"

/* ftype as a precision in bits: 0 for half precision and ftype 10. */
static unsigned ftype_bits(uint32_t insn)
{
	switch (field32(insn, 23, 22)) {
	case 0:
		return 32;
	case 1:
		return 64;
	default:
		return 0;
	}
}

static uint64_t get_fp(const Arm64Cpu *c, unsigned n, unsigned bits)
{
	return get_d(c, n) & datasize_mask(bits);
}

static void set_fp(Arm64Cpu *c, unsigned n, unsigned bits, uint64_t value)
{
	set_scalar(c, n, bits / 8, value);
}

/* An operation's environment: FPCR, and the flags it raises. */
static ArmFp fp_begin(const Arm64Cpu *c)
{
	return (ArmFp){c->fpcr, 0};
}

static void fp_end(Arm64Cpu *c, const ArmFp *f)
{
	c->fpsr |= f->flags;
}

/*
 * FMOV (general), C7.2.157: rmode 00 between a register and S (sf = 0,
 * ftype = 00) or D (sf = 1, ftype = 01); rmode 01 between X and V.D[1]
 * (ftype = 10). Half precision needs FEAT_FP16; rmode 1x holds newer
 * instructions.
 */
static Arm64Exec fmov_general(Arm64Cpu *c, uint32_t insn)
{
	bool sf = bit32(insn, 31);
	unsigned ftype = field32(insn, 23, 22), rmode = field32(insn, 20, 19);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	Arm64Exec result;

	if (!(rmode == 0 && ftype == (sf ? 1u : 0u)) &&
	    !(rmode == 1 && ftype == 2 && sf))
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	if (bit32(insn, 16)) { /* general to SIMD&FP */
		if (rmode == 1)
			c->v[d].d[1] = get_x(c, n);
		else
			set_scalar(c, d, sf ? 8 : 4, get_x(c, n));
	} else if (rmode == 1) {
		set_x(c, d, c->v[n].d[1]);
	} else {
		set_x(c, d, get_d(c, n) & datasize_mask(sf ? 64 : 32));
	}
	return ARM64_EXEC_OK;
}

/*
 * Conversion between floating-point and integer, sf 0 S 11110 ftype 1
 * rmode opcode 000000 Rn Rd: FCVT{N,P,M,Z}{S,U} (opcode 00x, rmode the
 * rounding), SCVTF and UCVTF (rmode 00, opcode 01x), FCVTA{S,U} (rmode
 * 00, opcode 10x), and FMOV (opcode 11x).
 */
Arm64Exec arm64_fp_int_conversion(Arm64Cpu *c, uint32_t insn)
{
	unsigned intsize = bit32(insn, 31) ? 64 : 32, bits = ftype_bits(insn);
	unsigned rmode = field32(insn, 20, 19), opcode = field32(insn, 18, 16);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	FpRounding rounding = (FpRounding)rmode;
	Arm64Exec result;
	ArmFp f;

	if (bit32(insn, 29))
		return ARM64_EXEC_UNDEFINED;
	if (opcode >> 1 == 3)
		return fmov_general(c, insn);
	if (bits == 0 || (opcode >= 2 && rmode != 0))
		return ARM64_EXEC_UNDEFINED;
	if (opcode >= 4)
		rounding = FP_TIEAWAY;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	f = fp_begin(c);
	if (opcode >> 1 == 1)
		set_fp(c, d, bits,
		       fp_from_fixed(&f, bits, get_x(c, n), intsize, 0, opcode & 1,
		                     fp_rounding(c->fpcr)));
	else
		set_x(c, d,
		      fp_to_fixed(&f, bits, get_fp(c, n, bits), 0, opcode & 1, rounding,
		                  intsize));
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * Conversion between floating-point and fixed-point, sf 0 S 11110 ftype 0
 * rmode opcode scale Rn Rd: SCVTF and UCVTF (rmode 00, opcode 01x),
 * FCVTZS and FCVTZU (rmode 11, opcode 00x), with 64 - scale fraction
 * bits, which a 32-bit register cannot have more than 32 of. The caller
 * has checked S and ftype, as for the classes below but FCVT.
 */
Arm64Exec arm64_fp_fixed_conversion(Arm64Cpu *c, uint32_t insn)
{
	unsigned intsize = bit32(insn, 31) ? 64 : 32, bits = ftype_bits(insn);
	unsigned rmode = field32(insn, 20, 19), opcode = field32(insn, 18, 16);
	unsigned fbits = 64 - field32(insn, 15, 10);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	bool to_fp = rmode == 0 && opcode >> 1 == 1;
	Arm64Exec result;
	ArmFp f;

	if (fbits > intsize || (!to_fp && !(rmode == 3 && opcode >> 1 == 0)))
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	f = fp_begin(c);
	if (to_fp)
		set_fp(c, d, bits,
		       fp_from_fixed(&f, bits, get_x(c, n), intsize, fbits, opcode & 1,
		                     fp_rounding(c->fpcr)));
	else
		set_x(c, d,
		      fp_to_fixed(&f, bits, get_fp(c, n, bits), fbits, opcode & 1,
		                  FP_ZERO, intsize));
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/* FCVT, C7.2.69: opc the destination's precision, ftype the source's. */
static Arm64Exec fcvt(Arm64Cpu *c, uint32_t insn)
{
	static const unsigned precision[4] = {32, 64, 0, 16}; /* 10: BFCVT */
	unsigned from = precision[field32(insn, 23, 22)];
	unsigned to = precision[field32(insn, 16, 15)];
	Arm64Exec result;
	ArmFp f;

	if (from == 0 || to == 0 || from == to)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	f = fp_begin(c);
	set_fp(c, field32(insn, 4, 0), to,
	       fp_convert(&f, to, from, get_fp(c, field32(insn, 9, 5), from),
	                  fp_rounding(c->fpcr)));
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * Floating-point data-processing (1 source), M 0 S 11110 ftype 1 opcode
 * 10000 Rn Rd: FMOV, FABS, FNEG, FSQRT (opcodes 0 to 3), FCVT (0001xx),
 * and FRINT{N,P,M,Z,A} (8 to 12), FRINTX (14) and FRINTI (15). The
 * others need FEAT_FRINTTS or are unallocated, as is opcode 1xxxxx.
 */
Arm64Exec arm64_fp_one_source(Arm64Cpu *c, uint32_t insn)
{
	static const FpRounding frint[5] = {FP_TIEEVEN, FP_POSINF, FP_NEGINF,
	                                    FP_ZERO, FP_TIEAWAY};
	unsigned bits = ftype_bits(insn), opcode = field32(insn, 20, 15);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	uint64_t a;
	Arm64Exec result;
	ArmFp f;

	if (opcode >> 2 == 1)
		return fcvt(c, insn);
	if (bits == 0 || opcode >= 16 || opcode == 13) /* 4 to 7: FCVT */
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	a = get_fp(c, n, bits);
	f = fp_begin(c);
	switch (opcode) {
	case 0:
		break;
	case 1:
		a = fp_abs(bits, a);
		break;
	case 2:
		a = fp_neg(bits, a);
		break;
	case 3:
		a = fp_sqrt(&f, bits, a);
		break;
	default: /* FRINTX and FRINTI round as FPCR says */
		a = fp_round_int(&f, bits, a,
		                 opcode < 13 ? frint[opcode - 8] : fp_rounding(c->fpcr),
		                 opcode == 14);
		break;
	}
	set_fp(c, d, bits, a);
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * Floating-point data-processing (2 source), M 0 S 11110 ftype 1 Rm
 * opcode 10 Rn Rd: FMUL, FDIV, FADD, FSUB, FMAX, FMIN, FMAXNM, FMINNM and
 * FNMUL (opcodes 0 to 8).
 */
Arm64Exec arm64_fp_two_source(Arm64Cpu *c, uint32_t insn)
{
	unsigned bits = ftype_bits(insn), opcode = field32(insn, 15, 12);
	uint64_t a, b, r;
	Arm64Exec result;
	ArmFp f;

	if (opcode > 8)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	a = get_fp(c, field32(insn, 9, 5), bits);
	b = get_fp(c, field32(insn, 20, 16), bits);
	f = fp_begin(c);
	switch (opcode) {
	case 0:
		r = fp_mul(&f, bits, a, b);
		break;
	case 1:
		r = fp_div(&f, bits, a, b);
		break;
	case 2:
		r = fp_add(&f, bits, a, b);
		break;
	case 3:
		r = fp_sub(&f, bits, a, b);
		break;
	case 4:
		r = fp_max(&f, bits, a, b);
		break;
	case 5:
		r = fp_min(&f, bits, a, b);
		break;
	case 6:
		r = fp_maxnum(&f, bits, a, b);
		break;
	case 7:
		r = fp_minnum(&f, bits, a, b);
		break;
	default: /* FNMUL: the product, negated */
		r = fp_neg(bits, fp_mul(&f, bits, a, b));
		break;
	}
	set_fp(c, field32(insn, 4, 0), bits, r);
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * Floating-point data-processing (3 source), M 0 S 11111 ftype o1 Rm o0
 * Ra Rn Rd: FMADD, FMSUB, FNMADD and FNMSUB. o1 negates the addend, and
 * o0 != o1 the first factor, before the fused multiply-add.
 */
Arm64Exec arm64_fp_three_source(Arm64Cpu *c, uint32_t insn)
{
	unsigned bits = ftype_bits(insn);
	bool o1 = bit32(insn, 21), o0 = bit32(insn, 15);
	uint64_t a, n, m;
	Arm64Exec result;
	ArmFp f;

	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	a = get_fp(c, field32(insn, 14, 10), bits);
	n = get_fp(c, field32(insn, 9, 5), bits);
	m = get_fp(c, field32(insn, 20, 16), bits);
	if (o1)
		a = fp_neg(bits, a);
	if (o0 != o1)
		n = fp_neg(bits, n);
	f = fp_begin(c);
	set_fp(c, field32(insn, 4, 0), bits, fp_muladd(&f, bits, a, n, m));
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * Floating-point compare, M 0 S 11110 ftype 1 Rm op 1000 Rn opcode2: FCMP
 * and FCMPE (opcode2 bit 4), with Rm or with +0.0 (bit 3); op and
 * opcode2's bits 2:0 are 0. Rm is ignored against zero.
 */
Arm64Exec arm64_fp_compare(Arm64Cpu *c, uint32_t insn)
{
	unsigned bits = ftype_bits(insn);
	uint64_t a, b = 0;
	Arm64Exec result;
	ArmFp f;

	if (field32(insn, 15, 14) != 0 || field32(insn, 2, 0) != 0)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	a = get_fp(c, field32(insn, 9, 5), bits);
	if (!bit32(insn, 3))
		b = get_fp(c, field32(insn, 20, 16), bits);
	f = fp_begin(c);
	c->nzcv = (uint32_t)fp_compare(&f, bits, a, b, bit32(insn, 4)) << 28;
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/*
 * Floating-point conditional compare, M 0 S 11110 ftype 1 Rm cond 01 Rn
 * op nzcv: FCCMP and FCCMPE (op); nzcv when cond does not hold.
 */
Arm64Exec arm64_fp_cond_compare(Arm64Cpu *c, uint32_t insn)
{
	unsigned bits = ftype_bits(insn);
	Arm64Exec result;
	ArmFp f;

	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	if (!condition_holds(c, field32(insn, 15, 12))) {
		c->nzcv = field32(insn, 3, 0) << 28;
		return ARM64_EXEC_OK;
	}
	f = fp_begin(c);
	c->nzcv = (uint32_t)fp_compare(
				  &f, bits, get_fp(c, field32(insn, 9, 5), bits),
				  get_fp(c, field32(insn, 20, 16), bits), bit32(insn, 4))
	          << 28;
	fp_end(c, &f);
	return ARM64_EXEC_OK;
}

/* FCSEL, M 0 S 11110 ftype 1 Rm cond 11 Rn Rd: Rn if cond holds, else Rm. */
Arm64Exec arm64_fp_cond_select(Arm64Cpu *c, uint32_t insn)
{
	unsigned bits = ftype_bits(insn);
	Arm64Exec result;

	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	set_fp(c, field32(insn, 4, 0), bits,
	       get_fp(c,
	              condition_holds(c, field32(insn, 15, 12))
	                  ? field32(insn, 9, 5)
	                  : field32(insn, 20, 16),
	              bits));
	return ARM64_EXEC_OK;
}
