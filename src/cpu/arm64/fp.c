/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The floating-point arithmetic of Armv8.0-A on integers; see fp.h.
 *
 * A finite value is mant * 2^exp. The operations compute it exactly, or,
 * for a quotient or a square root, keep enough bits and a sticky bit:
 * bit 0 is then set, the exact value lying strictly between mant - 1 and
 * mant + 1, and the other bits are exact. Rounding looks 70 bits or more
 * above bit 0, where such a value rounds as the exact one does.
 */

#include "cpu/arm64/fp.h"

__extension__ typedef unsigned __int128 U128;

/* The format of n bits: exponent and fraction bits, the minimum exponent. */
typedef struct Format {
	unsigned e, f;
	int min_exp;
} Format;

static Format format(unsigned n)
{
	switch (n) {
	case 16:
		return (Format){5, 10, -14};
	case 32:
		return (Format){8, 23, -126};
	default:
		return (Format){11, 52, -1022};
	}
}

static uint64_t ones(unsigned bits)
{
	return bits >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1;
}

/* The FPType of FPUnpack(): a denormal is a Nonzero here. */
typedef enum FpType { T_ZERO, T_NONZERO, T_INF, T_QNAN, T_SNAN } FpType;

typedef struct Unpacked {
	FpType type;
	bool sign;
	int exp; /* a Nonzero's value: mant * 2^exp */
	uint64_t mant;
} Unpacked;

static bool is_nan(const Unpacked *u)
{
	return u->type == T_QNAN || u->type == T_SNAN;
}

FpRounding fp_rounding(uint32_t fpcr)
{
	return (FpRounding)(fpcr >> FPCR_RMODE_SHIFT & 3);
}

/*
 * FPUnpack(). FZ flushes a single or double precision denormal to zero,
 * with Input Denormal; AHP makes a half precision exponent of all ones a
 * number's.
 */
static Unpacked unpack(ArmFp *f, unsigned n, uint64_t x)
{
	Format fm = format(n);
	uint64_t frac = x & ones(fm.f);
	unsigned exp = (unsigned)(x >> fm.f & ones(fm.e));
	Unpacked u = {T_NONZERO, (x >> (n - 1) & 1) != 0, 0, 0};

	if (exp == 0) {
		if (frac == 0 || (n != 16 && (f->fpcr & FPCR_FZ))) {
			u.type = T_ZERO;
			if (frac != 0)
				f->flags |= FPSR_IDC;
		} else {
			u.exp = fm.min_exp - (int)fm.f;
			u.mant = frac;
		}
	} else if (exp == ones(fm.e) && !(n == 16 && (f->fpcr & FPCR_AHP))) {
		if (frac == 0)
			u.type = T_INF;
		else
			u.type = frac >> (fm.f - 1) ? T_QNAN : T_SNAN;
	} else {
		u.exp = (int)exp + fm.min_exp - 1 - (int)fm.f;
		u.mant = frac | UINT64_C(1) << fm.f;
	}
	return u;
}

static uint64_t zero(unsigned n, bool sign)
{
	return (uint64_t)sign << (n - 1);
}

static uint64_t infinity(unsigned n, bool sign)
{
	Format fm = format(n);

	return zero(n, sign) | ones(fm.e) << fm.f;
}

uint64_t fp_default_nan(unsigned n)
{
	Format fm = format(n);

	return ones(fm.e) << fm.f | UINT64_C(1) << (fm.f - 1);
}

uint64_t fp_abs(unsigned n, uint64_t a)
{
	return a & ones(n - 1);
}

uint64_t fp_neg(unsigned n, uint64_t a)
{
	return a ^ UINT64_C(1) << (n - 1);
}

static int msb128(U128 x)
{
	int i = 0;

	if (x >> 64) {
		x >>= 64;
		i = 64;
	}
	while (x >>= 1)
		i++;
	return i;
}

#define TOP 125 /* where rounding puts the most significant bit */

/*
 * FPRoundBase() with FPCR.AH 0: sign, mant * 2^exp (mant != 0, see the
 * top of the file), to n bits. Underflow is found before rounding, and FZ
 * flushes a result below the minimum normal to zero, before rounding too.
 */
static uint64_t round_value(ArmFp *f, unsigned n, bool sign, int exp, U128 mant,
                            FpRounding rounding)
{
	Format fm = format(n);
	int shift = TOP - msb128(mant), exponent, biased, drop;
	U128 int_mant;
	bool half, sticky, inexact, up = false, to_inf = false;
	uint64_t result;

	if (shift >= 0) {
		mant <<= shift;
	} else {
		bool jam = (mant & (((U128)1 << -shift) - 1)) != 0;

		mant = mant >> -shift | (U128)jam;
	}
	exp -= shift;
	exponent = exp + TOP; /* the value is 1.x * 2^exponent */
	if (n != 16 && (f->fpcr & FPCR_FZ) && exponent < fm.min_exp) {
		f->flags |= FPSR_UFC;
		return zero(n, sign);
	}
	biased = exponent - fm.min_exp + 1;
	if (biased < 0)
		biased = 0;
	/* The bits of mant below the unit of the result's last place. */
	drop = (biased > 0 ? exponent : fm.min_exp) - (int)fm.f - exp;
	if (drop > 127) {
		int_mant = 0;
		half = false;
		sticky = true;
	} else {
		int_mant = mant >> drop;
		half = (mant >> (drop - 1) & 1) != 0;
		sticky = (mant & (((U128)1 << (drop - 1)) - 1)) != 0;
	}
	inexact = half || sticky;
	if (biased == 0 && inexact)
		f->flags |= FPSR_UFC;
	switch (rounding) {
	case FP_TIEEVEN:
	default:
		up = half && (sticky || (int_mant & 1));
		to_inf = true;
		break;
	case FP_POSINF:
		up = inexact && !sign;
		to_inf = !sign;
		break;
	case FP_NEGINF:
		up = inexact && sign;
		to_inf = sign;
		break;
	case FP_ZERO:
	case FP_ODD: /* neither rounds up */
		break;
	}
	if (up) {
		int_mant++;
		if (int_mant == (U128)1 << fm.f) /* denormal to normal */
			biased = 1;
		if (int_mant == (U128)1 << (fm.f + 1)) /* its low bits are 0 */
			biased++;
	}
	if (inexact && rounding == FP_ODD && !(int_mant & 1))
		int_mant++;
	if (n != 16 || !(f->fpcr & FPCR_AHP)) {
		if (biased >= (int)ones(fm.e)) {
			result = to_inf ? infinity(n, sign)
			                : infinity(n, sign) - 1; /* the largest normal */
			f->flags |= FPSR_OFC;
			inexact = true;
		} else {
			result = zero(n, sign) | (uint64_t)biased << fm.f |
			         ((uint64_t)int_mant & ones(fm.f));
		}
	} else if (biased > (int)ones(fm.e)) { /* the alternative half precision */
		result = zero(n, sign) | ones(n - 1);
		f->flags |= FPSR_IOC;
		inexact = false;
	} else {
		result = zero(n, sign) | (uint64_t)biased << fm.f |
		         ((uint64_t)int_mant & ones(fm.f));
	}
	if (inexact)
		f->flags |= FPSR_IXC;
	return result;
}

/* FPProcessNaN(): a NaN, quietened, or the default NaN with DN. */
static uint64_t process_nan(ArmFp *f, unsigned n, const Unpacked *u, uint64_t x)
{
	Format fm = format(n);

	if (u->type == T_SNAN) {
		x |= UINT64_C(1) << (fm.f - 1);
		f->flags |= FPSR_IOC;
	}
	return (f->fpcr & FPCR_DN) ? fp_default_nan(n) : x;
}

/*
 * FPProcessNaNs() and FPProcessNaNs3(): the first signalling NaN, else the
 * first quiet one. False when there is none.
 */
static bool process_nans(ArmFp *f, unsigned n, const Unpacked *u,
                         const uint64_t *x, unsigned count, uint64_t *result)
{
	for (FpType t = T_SNAN; t >= T_QNAN; t--)
		for (unsigned i = 0; i < count; i++)
			if (u[i].type == t) {
				*result = process_nan(f, n, &u[i], x[i]);
				return true;
			}
	return false;
}

static uint64_t invalid(ArmFp *f, unsigned n)
{
	f->flags |= FPSR_IOC;
	return fp_default_nan(n);
}

/* A signed value mant * 2^exp, exact or with its sticky bit. */
typedef struct Term {
	bool sign;
	int exp;
	U128 mant; /* 0 for zero */
} Term;

static Term term(const Unpacked *u)
{
	return (Term){u->sign, u->exp, u->type == T_ZERO ? 0 : u->mant};
}

/*
 * a + b, for mantissas of up to 106 bits. The bigger goes to bit 125 and
 * the smaller follows it, both a bit up from where they were, so that what
 * falls off the smaller one's end can make the sticky bit 0.
 */
static Term term_add(Term a, Term b)
{
	Term t;
	int shift;

	if (a.mant == 0)
		return b;
	if (b.mant == 0)
		return a;
	if (a.exp + msb128(a.mant) < b.exp + msb128(b.mant)) {
		t = a;
		a = b;
		b = t;
	}
	shift = TOP - 1 - msb128(a.mant);
	a.mant <<= shift;
	a.exp -= shift;
	shift = a.exp - b.exp; /* how far b goes right */
	if (shift <= 0) {
		b.mant = b.mant << -shift << 1;
	} else if (shift >= 127) {
		b.mant = 1; /* the sticky bit alone */
	} else {
		bool jam = (b.mant & (((U128)1 << shift) - 1)) != 0;

		b.mant = b.mant >> shift << 1 | (U128)jam;
	}
	t.exp = a.exp - 1;
	a.mant <<= 1;
	if (a.sign == b.sign) {
		t.sign = a.sign;
		t.mant = a.mant + b.mant;
	} else if (a.mant >= b.mant) {
		t.sign = a.sign;
		t.mant = a.mant - b.mant;
	} else {
		t.sign = b.sign;
		t.mant = b.mant - a.mant;
	}
	return t;
}

/*
 * The end of FPAdd() and FPMulAdd(): a sum of zero is +0, or -0 when
 * rounding to minus infinity; anything else is rounded.
 */
static uint64_t round_sum(ArmFp *f, unsigned n, Term t)
{
	FpRounding rounding = fp_rounding(f->fpcr);

	if (t.mant == 0)
		return zero(n, rounding == FP_NEGINF);
	return round_value(f, n, t.sign, t.exp, t.mant, rounding);
}

static uint64_t add(ArmFp *f, unsigned n, uint64_t a, uint64_t b, bool sub)
{
	uint64_t x[2] = {a, b}, result;
	Unpacked u[2] = {unpack(f, n, a), unpack(f, n, b)};

	if (process_nans(f, n, u, x, 2, &result))
		return result;
	if (sub)
		u[1].sign = !u[1].sign;
	if (u[0].type == T_INF && u[1].type == T_INF && u[0].sign != u[1].sign)
		return invalid(f, n);
	if (u[0].type == T_INF || u[1].type == T_INF)
		return infinity(n, u[u[0].type == T_INF ? 0 : 1].sign);
	if (u[0].type == T_ZERO && u[1].type == T_ZERO && u[0].sign == u[1].sign)
		return zero(n, u[0].sign);
	return round_sum(f, n, term_add(term(&u[0]), term(&u[1])));
}

uint64_t fp_add(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return add(f, n, a, b, false);
}

uint64_t fp_sub(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return add(f, n, a, b, true);
}

uint64_t fp_mul(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	uint64_t x[2] = {a, b}, result;
	Unpacked u[2] = {unpack(f, n, a), unpack(f, n, b)};
	bool sign = u[0].sign != u[1].sign;

	if (process_nans(f, n, u, x, 2, &result))
		return result;
	if ((u[0].type == T_INF && u[1].type == T_ZERO) ||
	    (u[0].type == T_ZERO && u[1].type == T_INF))
		return invalid(f, n);
	if (u[0].type == T_INF || u[1].type == T_INF)
		return infinity(n, sign);
	if (u[0].type == T_ZERO || u[1].type == T_ZERO)
		return zero(n, sign);
	return round_value(f, n, sign, u[0].exp + u[1].exp,
	                   (U128)u[0].mant * u[1].mant, fp_rounding(f->fpcr));
}

/* +-2.0 and 1.5, as FPTwo() and FPOnePointFive() make them. */
static uint64_t two(unsigned n, bool sign)
{
	Format fm = format(n);

	return zero(n, sign) | (uint64_t)(-fm.min_exp + 2) << fm.f;
}

static uint64_t one_point_five(unsigned n)
{
	Format fm = format(n);

	return (uint64_t)(-fm.min_exp + 1) << fm.f | UINT64_C(1) << (fm.f - 1);
}

/* FPMul() does all but infinity times zero, NaNs first. */
uint64_t fp_mulx(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	Unpacked ua = unpack(f, n, a), ub = unpack(f, n, b);

	if ((ua.type == T_INF && ub.type == T_ZERO) ||
	    (ua.type == T_ZERO && ub.type == T_INF))
		return two(n, ua.sign != ub.sign);
	return fp_mul(f, n, a, b);
}

/*
 * FPRecipStepFused() (with constant 2) and FPRSqrtStepFused() (with 3,
 * halved): constant - a * b, a negated first, NaNs and all.
 */
static uint64_t step_fused(ArmFp *f, unsigned n, uint64_t a, uint64_t b,
                           bool rsqrt)
{
	uint64_t x[2], result;
	Unpacked u[2];
	Term k, p;

	a = fp_neg(n, a);
	x[0] = a;
	x[1] = b;
	u[0] = unpack(f, n, a);
	u[1] = unpack(f, n, b);
	if (process_nans(f, n, u, x, 2, &result))
		return result;
	if ((u[0].type == T_INF && u[1].type == T_ZERO) ||
	    (u[0].type == T_ZERO && u[1].type == T_INF))
		return rsqrt ? one_point_five(n) : two(n, false);
	if (u[0].type == T_INF || u[1].type == T_INF)
		return infinity(n, u[0].sign != u[1].sign);
	k = (Term){false, 0, rsqrt ? 3 : 2};
	p.sign = u[0].sign != u[1].sign;
	p.exp = u[0].exp + u[1].exp;
	p.mant = (U128)u[0].mant * u[1].mant; /* a zero's mant is 0 */
	k = term_add(k, p);
	if (rsqrt)
		k.exp--;
	return round_sum(f, n, k);
}

uint64_t fp_recip_step(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return step_fused(f, n, a, b, false);
}

uint64_t fp_rsqrt_step(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return step_fused(f, n, a, b, true);
}

uint64_t fp_muladd(ArmFp *f, unsigned n, uint64_t addend, uint64_t a,
                   uint64_t b)
{
	uint64_t x[3] = {addend, a, b}, result;
	Unpacked u[3] = {unpack(f, n, addend), unpack(f, n, a), unpack(f, n, b)};
	bool inf_a = u[0].type == T_INF, zero_a = u[0].type == T_ZERO;
	bool inf1 = u[1].type == T_INF, zero1 = u[1].type == T_ZERO;
	bool inf2 = u[2].type == T_INF, zero2 = u[2].type == T_ZERO;
	bool sign_p = u[1].sign != u[2].sign, inf_p = inf1 || inf2;
	bool zero_p = zero1 || zero2;
	bool nans = process_nans(f, n, u, x, 3, &result);
	Term p;

	/* A quiet NaN addend with infinity times zero is invalid as well. */
	if (u[0].type == T_QNAN && ((inf1 && zero2) || (zero1 && inf2)))
		return invalid(f, n);
	if (nans)
		return result;
	if ((inf1 && zero2) || (zero1 && inf2) ||
	    (inf_a && inf_p && u[0].sign != sign_p))
		return invalid(f, n);
	if ((inf_a && !u[0].sign) || (inf_p && !sign_p))
		return infinity(n, false);
	if ((inf_a && u[0].sign) || (inf_p && sign_p))
		return infinity(n, true);
	if (zero_a && zero_p && u[0].sign == sign_p)
		return zero(n, u[0].sign);
	p.sign = sign_p;
	p.exp = u[1].exp + u[2].exp;
	p.mant = (U128)u[1].mant * u[2].mant; /* a zero's mant is 0 */
	return round_sum(f, n, term_add(term(&u[0]), p));
}

/* A Nonzero's mantissa with its most significant bit at 63. */
static uint64_t normalized(const Unpacked *u, int *exp)
{
	int shift = 63 - msb128(u->mant);

	*exp = u->exp - shift;
	return u->mant << shift;
}

uint64_t fp_div(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	uint64_t x[2] = {a, b}, result, ma, mb;
	Unpacked u[2] = {unpack(f, n, a), unpack(f, n, b)};
	bool sign = u[0].sign != u[1].sign;
	int ea, eb;
	U128 q, r;

	if (process_nans(f, n, u, x, 2, &result))
		return result;
	if ((u[0].type == T_INF && u[1].type == T_INF) ||
	    (u[0].type == T_ZERO && u[1].type == T_ZERO))
		return invalid(f, n);
	if (u[0].type == T_INF || u[1].type == T_ZERO) {
		if (u[0].type != T_INF)
			f->flags |= FPSR_DZC;
		return infinity(n, sign);
	}
	if (u[0].type == T_ZERO || u[1].type == T_INF)
		return zero(n, sign);
	ma = normalized(&u[0], &ea);
	mb = normalized(&u[1], &eb);
	/* 64 or 65 bits of quotient, then the sticky bit. */
	q = ((U128)ma << 64) / mb;
	r = ((U128)ma << 64) % mb;
	return round_value(f, n, sign, ea - eb - 65, q << 1 | (U128)(r != 0),
	                   fp_rounding(f->fpcr));
}

/* The integer square root of x, and whether it is exact. */
static uint64_t isqrt128(U128 x, bool *exact)
{
	U128 root = 0, bit = (U128)1 << 126;

	while (bit != 0) {
		if (x >= root + bit) {
			x -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	*exact = x == 0;
	return (uint64_t)root;
}

uint64_t fp_sqrt(ArmFp *f, unsigned n, uint64_t a)
{
	Unpacked u = unpack(f, n, a);
	int shift, exp;
	U128 m;
	uint64_t root;
	bool exact;

	if (is_nan(&u))
		return process_nan(f, n, &u, a);
	if (u.type == T_ZERO)
		return zero(n, u.sign);
	if (u.type == T_INF && !u.sign)
		return infinity(n, false);
	if (u.sign)
		return invalid(f, n);
	/* 124 or 125 bits, with an even exponent: 62 or 63 bits of root. */
	shift = 124 - msb128(u.mant);
	if ((u.exp - shift) & 1)
		shift++;
	m = (U128)u.mant << shift;
	exp = (u.exp - shift) / 2;
	root = isqrt128(m, &exact);
	return round_value(f, n, false, exp - 1, (U128)root << 1 | (U128)!exact,
	                   fp_rounding(f->fpcr));
}

/* Sign of the value: -1, 0 or 1. */
static int signum(const Unpacked *u)
{
	if (u->type == T_ZERO)
		return 0;
	return u->sign ? -1 : 1;
}

/* Compare two values that are not NaNs: -1, 0 or 1. */
static int compare_values(const Unpacked *a, const Unpacked *b)
{
	int sa = signum(a), sb = signum(b), mag;
	int ea, eb;
	uint64_t ma, mb;

	if (sa != sb)
		return sa < sb ? -1 : 1;
	if (sa == 0)
		return 0;
	if (a->type == T_INF || b->type == T_INF) {
		mag = (a->type == T_INF) - (b->type == T_INF);
	} else {
		ma = normalized(a, &ea);
		mb = normalized(b, &eb);
		if (ea != eb)
			mag = ea < eb ? -1 : 1;
		else
			mag = ma < mb ? -1 : ma > mb;
	}
	return sa > 0 ? mag : -mag;
}

unsigned fp_compare(ArmFp *f, unsigned n, uint64_t a, uint64_t b,
                    bool signal_nans)
{
	Unpacked ua = unpack(f, n, a), ub = unpack(f, n, b);
	int c;

	if (is_nan(&ua) || is_nan(&ub)) {
		if (signal_nans || ua.type == T_SNAN || ub.type == T_SNAN)
			f->flags |= FPSR_IOC;
		return 0x3;
	}
	c = compare_values(&ua, &ub);
	if (c == 0)
		return 0x6;
	return c < 0 ? 0x8 : 0x2;
}

/* FPCompareEQ(), FPCompareGE() and FPCompareGT(): c < 0, 0 or > 0 holds. */
static bool compare_holds(ArmFp *f, unsigned n, uint64_t a, uint64_t b,
                          bool signal_nans, int lowest)
{
	Unpacked ua = unpack(f, n, a), ub = unpack(f, n, b);
	int c;

	if (is_nan(&ua) || is_nan(&ub)) {
		if (signal_nans || ua.type == T_SNAN || ub.type == T_SNAN)
			f->flags |= FPSR_IOC;
		return false;
	}
	c = compare_values(&ua, &ub);
	return lowest == 0 ? c == 0 : c >= lowest - 1;
}

bool fp_compare_eq(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return compare_holds(f, n, a, b, false, 0);
}

bool fp_compare_ge(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return compare_holds(f, n, a, b, true, 1);
}

bool fp_compare_gt(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return compare_holds(f, n, a, b, true, 2);
}

/* FPMax() and FPMin(). */
static uint64_t min_max(ArmFp *f, unsigned n, uint64_t a, uint64_t b, bool max)
{
	uint64_t x[2] = {a, b}, result;
	Unpacked u[2] = {unpack(f, n, a), unpack(f, n, b)};
	const Unpacked *pick;
	int c;

	if (process_nans(f, n, u, x, 2, &result))
		return result;
	c = compare_values(&u[0], &u[1]);
	pick = (max ? c > 0 : c < 0) ? &u[0] : &u[1];
	if (pick->type == T_INF)
		return infinity(n, pick->sign);
	if (pick->type == T_ZERO)
		return zero(n, max ? u[0].sign && u[1].sign : u[0].sign || u[1].sign);
	return round_value(f, n, pick->sign, pick->exp, pick->mant,
	                   fp_rounding(f->fpcr));
}

uint64_t fp_max(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return min_max(f, n, a, b, true);
}

uint64_t fp_min(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return min_max(f, n, a, b, false);
}

/*
 * FPMaxNum() and FPMinNum(): a quiet NaN against anything but a quiet NaN
 * stands for the infinity that loses.
 */
static uint64_t min_max_num(ArmFp *f, unsigned n, uint64_t a, uint64_t b,
                            bool max)
{
	ArmFp quiet = {f->fpcr, 0}; /* only the types: no flags yet */
	FpType ta = unpack(&quiet, n, a).type, tb = unpack(&quiet, n, b).type;

	if (ta == T_QNAN && tb != T_QNAN)
		a = infinity(n, max);
	else if (ta != T_QNAN && tb == T_QNAN)
		b = infinity(n, max);
	return min_max(f, n, a, b, max);
}

uint64_t fp_maxnum(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return min_max_num(f, n, a, b, true);
}

uint64_t fp_minnum(ArmFp *f, unsigned n, uint64_t a, uint64_t b)
{
	return min_max_num(f, n, a, b, false);
}

/*
 * A Nonzero value times 2^scale, split: its magnitude's integer part,
 * and whether the fraction is at least a half (half) and more (sticky).
 * False when the integer part does not fit in 64 bits.
 */
static bool split(const Unpacked *u, int scale, uint64_t *integer, bool *half,
                  bool *sticky)
{
	int exp = u->exp + scale, k;

	*half = *sticky = false;
	if (exp >= 0) {
		if (exp + msb128(u->mant) >= 64)
			return false;
		*integer = u->mant << exp;
		return true;
	}
	k = -exp;
	if (k >= 64) {
		*integer = 0;
		*sticky = true; /* below a quarter: mant < 2^53 */
		return true;
	}
	*integer = u->mant >> k;
	*half = (u->mant >> (k - 1) & 1) != 0;
	*sticky = (u->mant & ones((unsigned)k - 1)) != 0;
	return true;
}

/*
 * The rounding of FPToFixed() and FPRoundInt(), on the magnitude: whether
 * a value of that sign, whose magnitude's integer part is integer, with
 * that fraction, rounds to a bigger magnitude.
 */
static bool rounds_away(bool sign, uint64_t integer, bool half, bool sticky,
                        FpRounding rounding)
{
	bool inexact = half || sticky;

	switch (rounding) {
	case FP_TIEEVEN:
	default:
		return half && (sticky || (integer & 1));
	case FP_POSINF:
		return inexact && !sign;
	case FP_NEGINF:
		return inexact && sign;
	case FP_ZERO:
		return false;
	case FP_TIEAWAY:
		return half;
	}
}

uint64_t fp_round_int(ArmFp *f, unsigned n, uint64_t a, FpRounding rounding,
                      bool exact)
{
	Unpacked u = unpack(f, n, a);
	uint64_t integer;
	bool half, sticky;

	if (is_nan(&u))
		return process_nan(f, n, &u, a);
	if (u.type == T_INF)
		return infinity(n, u.sign);
	/* A zero, whose mant is 0, comes out as itself. */
	if (!split(&u, 0, &integer, &half, &sticky))
		return a; /* 2^64 or more: an integer already */
	if (rounds_away(u.sign, integer, half, sticky, rounding))
		integer++;
	if ((half || sticky) && exact)
		f->flags |= FPSR_IXC;
	if (integer == 0)
		return zero(n, u.sign);
	return round_value(f, n, u.sign, 0, integer, FP_ZERO);
}

uint64_t fp_to_fixed(ArmFp *f, unsigned n, uint64_t a, unsigned fbits,
                     bool is_unsigned, FpRounding rounding, unsigned m)
{
	Unpacked u = unpack(f, n, a);
	uint64_t integer = 0, max;
	bool half = false, sticky = false, fits = true;

	if (is_nan(&u)) {
		f->flags |= FPSR_IOC;
		return 0;
	}
	if (u.type == T_INF)
		fits = false;
	else if (u.type == T_NONZERO)
		fits = split(&u, (int)fbits, &integer, &half, &sticky);
	if (fits && rounds_away(u.sign, integer, half, sticky, rounding))
		fits = ++integer != 0;
	/* The largest magnitude of the sign: 2^(m-1) for a negative one. */
	if (is_unsigned)
		max = u.sign ? 0 : ones(m);
	else
		max = u.sign ? UINT64_C(1) << (m - 1) : ones(m - 1);
	if (!fits || integer > max) {
		f->flags |= FPSR_IOC;
		integer = max;
	} else if (half || sticky) {
		f->flags |= FPSR_IXC;
	}
	return (u.sign ? (uint64_t)0 - integer : integer) & ones(m);
}

uint64_t fp_from_fixed(ArmFp *f, unsigned n, uint64_t a, unsigned m,
                       unsigned fbits, bool is_unsigned, FpRounding rounding)
{
	bool sign = !is_unsigned && (a >> (m - 1) & 1);
	uint64_t mag = a & ones(m);

	if (sign)
		mag = ((uint64_t)0 - mag) & ones(m);
	if (mag == 0)
		return zero(n, false);
	return round_value(f, n, sign, -(int)fbits, mag, rounding);
}

/* FPConvertNaN(): the sign and the payload's top bits, quiet. */
static uint64_t convert_nan(unsigned to, unsigned from, uint64_t a)
{
	Format ff = format(from), ft = format(to);
	uint64_t payload = a & ones(ff.f - 1); /* without the quiet bit */
	uint64_t result = zero(to, (a >> (from - 1) & 1) != 0) | ones(ft.e + 1)
	                                                             << (ft.f - 1);

	/* 51 bits: the payload at the top, as the pseudocode lays it out. */
	payload <<= 52 - ff.f;
	return result | payload >> (52 - ft.f);
}

uint64_t fp_convert(ArmFp *f, unsigned to, unsigned from, uint64_t a,
                    FpRounding rounding)
{
	Unpacked u = unpack(f, from, a);
	bool alt_hp = to == 16 && (f->fpcr & FPCR_AHP);

	if (is_nan(&u)) {
		uint64_t result;

		if (alt_hp)
			result = zero(to, u.sign);
		else if (f->fpcr & FPCR_DN)
			result = fp_default_nan(to);
		else
			result = convert_nan(to, from, a);
		if (u.type == T_SNAN || alt_hp)
			f->flags |= FPSR_IOC;
		return result;
	}
	if (u.type == T_INF) {
		if (alt_hp) {
			f->flags |= FPSR_IOC;
			return zero(to, u.sign) | ones(to - 1);
		}
		return infinity(to, u.sign);
	}
	if (u.type == T_ZERO)
		return zero(to, u.sign);
	return round_value(f, to, u.sign, u.exp, u.mant, rounding);
}

/* RecipEstimate(): a in [256, 512), the reciprocal in [256, 512). */
static unsigned recip_estimate(unsigned a)
{
	unsigned b = (1u << 19) / (a * 2 + 1);

	return (b + 1) / 2;
}

/* RecipSqrtEstimate(): a in [128, 512), 1 / sqrt in [256, 512). */
static unsigned rsqrt_estimate(unsigned a)
{
	uint64_t b = 512;

	if (a < 256)
		a = a * 2 + 1;
	else
		a = ((a >> 1 << 1) + 1) * 2;
	while ((uint64_t)a * (b + 1) * (b + 1) < (UINT64_C(1) << 28))
		b++;
	return (unsigned)((b + 1) / 2);
}

/* The fraction of a, as 52 bits, and its biased exponent. */
static uint64_t fraction52(unsigned n, uint64_t a, int *exp)
{
	Format fm = format(n);

	*exp = (int)(a >> fm.f & ones(fm.e));
	return (a & ones(fm.f)) << (52 - fm.f);
}

uint64_t fp_recip_estimate(ArmFp *f, unsigned n, uint64_t a)
{
	Format fm = format(n);
	Unpacked u = unpack(f, n, a);
	int bias = -fm.min_exp + 1, exp, result_exp, top;
	uint64_t fraction;

	if (is_nan(&u))
		return process_nan(f, n, &u, a);
	if (u.type == T_INF)
		return zero(n, u.sign);
	if (u.type == T_ZERO) {
		f->flags |= FPSR_DZC;
		return infinity(n, u.sign);
	}
	/* Below 2^-(bias + 1): the reciprocal overflows. */
	top = u.exp + msb128(u.mant);
	if (top < -bias - 1) {
		FpRounding r = fp_rounding(f->fpcr);
		bool to_inf = r == FP_TIEEVEN || (r == FP_POSINF && !u.sign) ||
		              (r == FP_NEGINF && u.sign);

		f->flags |= FPSR_OFC | FPSR_IXC;
		return to_inf ? infinity(n, u.sign) : infinity(n, u.sign) - 1;
	}
	if (n != 16 && (f->fpcr & FPCR_FZ) && top >= bias - 1) {
		f->flags |= FPSR_UFC;
		return zero(n, u.sign);
	}
	fraction = fraction52(n, a, &exp);
	if (exp == 0) {
		if (!(fraction >> 51 & 1)) {
			exp = -1;
			fraction = fraction << 2 & ones(52);
		} else {
			fraction = fraction << 1 & ones(52);
		}
	}
	result_exp = 2 * bias - 1 - exp;
	fraction =
		(uint64_t)(recip_estimate((unsigned)(256 | fraction >> 44)) & 0xff)
		<< 44;
	if (result_exp == 0) {
		fraction = UINT64_C(1) << 51 | fraction >> 1;
	} else if (result_exp == -1) {
		fraction = UINT64_C(1) << 50 | fraction >> 2;
		result_exp = 0;
	}
	return zero(n, u.sign) | (uint64_t)result_exp << fm.f |
	       fraction >> (52 - fm.f);
}

uint64_t fp_rsqrt_estimate(ArmFp *f, unsigned n, uint64_t a)
{
	Format fm = format(n);
	Unpacked u = unpack(f, n, a);
	int bias = -fm.min_exp + 1, exp, result_exp;
	uint64_t fraction;
	unsigned scaled;

	if (is_nan(&u))
		return process_nan(f, n, &u, a);
	if (u.type == T_ZERO) {
		f->flags |= FPSR_DZC;
		return infinity(n, u.sign);
	}
	if (u.sign)
		return invalid(f, n);
	if (u.type == T_INF)
		return zero(n, false);
	fraction = fraction52(n, a, &exp);
	if (exp == 0) {
		while (!(fraction >> 51 & 1)) {
			fraction <<= 1;
			exp--;
		}
		fraction = fraction << 1 & ones(52);
	}
	if (exp & 1)
		scaled = (unsigned)(fraction >> 45) | 128;
	else
		scaled = (unsigned)(fraction >> 44) | 256;
	/* (3 * bias - 1 - exp) / 2, rounded down: exp may be negative. */
	result_exp = (3 * bias - 1 - exp) >> 1;
	return (uint64_t)result_exp << fm.f |
	       (uint64_t)(rsqrt_estimate(scaled) & 0xff) << (fm.f - 8);
}

uint64_t fp_recpx(ArmFp *f, unsigned n, uint64_t a)
{
	Format fm = format(n);
	Unpacked u = unpack(f, n, a);
	uint64_t exp = a >> fm.f & ones(fm.e);

	if (is_nan(&u))
		return process_nan(f, n, &u, a);
	if (exp == 0) /* zeros and denormals */
		return zero(n, u.sign) | (ones(fm.e) - 1) << fm.f;
	return zero(n, u.sign) | (~exp & ones(fm.e)) << fm.f;
}

uint32_t fp_unsigned_recip_estimate(uint32_t a)
{
	if (!(a >> 31))
		return 0xffffffff;
	return (uint32_t)recip_estimate(a >> 23) << 23;
}

uint32_t fp_unsigned_rsqrt_estimate(uint32_t a)
{
	if (!(a >> 30))
		return 0xffffffff;
	return (uint32_t)rsqrt_estimate(a >> 23) << 23;
}
