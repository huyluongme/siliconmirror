/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the floating-point arithmetic (src/cpu/arm64/fp.c).
 *
 * The bulk test runs each operation on inputs from a generator, the
 * same as that of a reference model written from the Arm ARM's
 * pseudocode with exact rational arithmetic, independently of fp.c, and
 * compares, a group at a time, a hash of the results and the exceptions
 * with the model's. The model rounds as the host's FPU does where both
 * apply (to nearest, no NaNs): 125774 cases agree.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cpu/arm64/fp.h"
#include "test.h"
#include "util/macros.h"

#define AHP FPCR_AHP
#define DN FPCR_DN
#define FZ FPCR_FZ

/* SplitMix64, as the model has it. */
static uint64_t rng_state;

static uint64_t rng_next(void)
{
	uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));

	z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
	return z ^ (z >> 31);
}

static uint64_t mask(unsigned bits)
{
	return bits >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1;
}

/*
 * A value of n bits from a mix of classes: zeros, infinities, NaNs,
 * denormals, the smallest and largest normals, values near 1, any bits;
 * or, with near, half the time, one close to near.
 */
static uint64_t operand(unsigned n, const uint64_t *near)
{
	unsigned e = n == 32 ? 8 : n == 64 ? 11 : 5, f = n - e - 1;
	uint64_t r = rng_next(), frac = rng_next() & mask(f), x, exp;
	uint64_t emax = mask(e), sign = (r >> 8) & 1;
	unsigned cls = (unsigned)(r & 15);

	if (near != NULL && (r >> 12 & 1)) {
		x = *near ^ (cls < 8 ? (rng_next() & 0xff) : UINT64_C(1) << (n - 1));
		if (cls >= 8)
			x ^= rng_next() & 0xf;
		return x & mask(n);
	}
	switch (cls) {
	case 0:
		exp = 0;
		frac = 0;
		break;
	case 1:
		exp = emax;
		frac = 0;
		break;
	case 2:
		exp = emax;
		frac |= UINT64_C(1) << (f - 1);
		break;
	case 3:
		exp = emax;
		frac &= mask(f - 1);
		if (frac == 0)
			frac = 1;
		break;
	case 4:
		exp = 0;
		break;
	case 5:
		exp = 1 + (r >> 9) % 2;
		break;
	case 6:
		exp = emax - 1 - (r >> 9) % 2;
		break;
	case 7:
		exp = (emax >> 1) - 2 + (r >> 9) % 5;
		break;
	default:
		return rng_next() & mask(n);
	}
	return sign << (n - 1) | exp << f | frac;
}

static uint64_t int_operand(unsigned m)
{
	uint64_t r = rng_next(), x = rng_next();

	switch (r & 7) {
	case 0:
		x = 0;
		break;
	case 1:
		x = UINT64_C(1) << (m - 1);
		break;
	case 2:
		x = mask(m - 1);
		break;
	case 3:
		x >>= rng_next() % m;
		break;
	case 4:
		x = (uint64_t)0 - (x >> (rng_next() % m));
		break;
	default:
		break;
	}
	return x & mask(m);
}

/* One case of a group: the result; f gathers the exceptions. */
static uint64_t run_case(const char *name, unsigned n, ArmFp *f)
{
	uint64_t a, b, c, r;
	unsigned m, fbits, mode;

	if (strcmp(name, "muladd") == 0) {
		a = operand(n, NULL);
		b = operand(n, NULL);
		c = operand(n, NULL);
		if (rng_next() & 1) {
			ArmFp quiet = {f->fpcr, 0};
			uint64_t p = fp_mul(&quiet, n, a, b) ^ UINT64_C(1) << (n - 1);

			c = operand(n, &p);
		}
		return fp_muladd(f, n, c, a, b);
	}
	if (strcmp(name, "sqrt") == 0)
		return fp_sqrt(f, n, operand(n, NULL));
	if (strcmp(name, "rint") == 0) {
		mode = (unsigned)(rng_next() % 7);
		a = operand(n, NULL);
		if (mode < 5)
			return fp_round_int(f, n, a, (FpRounding)mode, false);
		return fp_round_int(f, n, a, fp_rounding(f->fpcr), mode == 5);
	}
	if (strcmp(name, "tofix") == 0 || strcmp(name, "fromfix") == 0) {
		r = rng_next();
		m = r & 1 ? 64 : 32;
		mode = (unsigned)((r >> 2) % 5);
		fbits = r & 0x100 ? 0 : (unsigned)((r >> 16) % (m + 1));
		if (name[0] == 't')
			return fp_to_fixed(f, n, operand(n, NULL), fbits, r & 2,
			                   (FpRounding)mode, m);
		return fp_from_fixed(f, n, int_operand(m), m, fbits, r & 2,
		                     fp_rounding(f->fpcr));
	}
	if (strcmp(name, "cvtodd") == 0)
		return fp_convert(f, 32, 64, operand(64, NULL), FP_ODD);
	if (strcmp(name, "urecpe") == 0)
		return fp_unsigned_recip_estimate((uint32_t)rng_next());
	if (strcmp(name, "ursqrte") == 0)
		return fp_unsigned_rsqrt_estimate((uint32_t)rng_next());
	if (strcmp(name, "recpe") == 0)
		return fp_recip_estimate(f, n, operand(n, NULL));
	if (strcmp(name, "rsqrte") == 0)
		return fp_rsqrt_estimate(f, n, operand(n, NULL));
	if (strcmp(name, "recpx") == 0)
		return fp_recpx(f, n, operand(n, NULL));
	if (strncmp(name, "cvt", 3) == 0) {
		unsigned from, to;

		CHECK_EQ(sscanf(name, "cvt%uto%u", &from, &to), 2);
		return fp_convert(f, to, from, operand(from, NULL),
		                  fp_rounding(f->fpcr));
	}
	a = operand(n, NULL);
	b = operand(n, &a);
	if (strcmp(name, "add") == 0)
		return fp_add(f, n, a, b);
	if (strcmp(name, "sub") == 0)
		return fp_sub(f, n, a, b);
	if (strcmp(name, "mul") == 0)
		return fp_mul(f, n, a, b);
	if (strcmp(name, "div") == 0)
		return fp_div(f, n, a, b);
	if (strcmp(name, "max") == 0)
		return fp_max(f, n, a, b);
	if (strcmp(name, "min") == 0)
		return fp_min(f, n, a, b);
	if (strcmp(name, "maxnum") == 0)
		return fp_maxnum(f, n, a, b);
	if (strcmp(name, "minnum") == 0)
		return fp_minnum(f, n, a, b);
	if (strcmp(name, "mulx") == 0)
		return fp_mulx(f, n, a, b);
	if (strcmp(name, "recps") == 0)
		return fp_recip_step(f, n, a, b);
	if (strcmp(name, "rsqrts") == 0)
		return fp_rsqrt_step(f, n, a, b);
	if (strcmp(name, "cmeq") == 0)
		return fp_compare_eq(f, n, a, b);
	if (strcmp(name, "cmge") == 0)
		return fp_compare_ge(f, n, a, b);
	if (strcmp(name, "cmgt") == 0)
		return fp_compare_gt(f, n, a, b);
	return fp_compare(f, n, a, b, strcmp(name, "cmpe") == 0);
}

static uint64_t fnv(uint64_t h, uint64_t x)
{
	for (unsigned i = 0; i < 8; i++) {
		h ^= (x >> (8 * i)) & 0xff;
		h *= UINT64_C(0x100000001B3);
	}
	return h;
}

static const struct {
	const char *name;
	unsigned n;
	uint32_t fpcr;
	uint64_t hash;
} groups[] = {
	{"add", 32, 0x00000000, UINT64_C(0x0e84fbd109044424)},
	{"sub", 32, 0x00000000, UINT64_C(0x2d720dde064af814)},
	{"mul", 32, 0x00000000, UINT64_C(0x91af1376594d9cff)},
	{"div", 32, 0x00000000, UINT64_C(0x978d86adfd4c6838)},
	{"max", 32, 0x00000000, UINT64_C(0xe715c3b6f4a73496)},
	{"min", 32, 0x00000000, UINT64_C(0xa4b415e8761e06d4)},
	{"maxnum", 32, 0x00000000, UINT64_C(0x3fc1be6ab88563db)},
	{"minnum", 32, 0x00000000, UINT64_C(0xce89a933f17a1b56)},
	{"cmp", 32, 0x00000000, UINT64_C(0x066a5756dcc93804)},
	{"cmpe", 32, 0x00000000, UINT64_C(0x1d485e711823b825)},
	{"muladd", 32, 0x00000000, UINT64_C(0x10058206279b1cb7)},
	{"sqrt", 32, 0x00000000, UINT64_C(0x534b32ad81eda34f)},
	{"rint", 32, 0x00000000, UINT64_C(0x6c5ac37098bde31b)},
	{"tofix", 32, 0x00000000, UINT64_C(0x67fb1d54ea6a4569)},
	{"fromfix", 32, 0x00000000, UINT64_C(0x4e5b3300721848d8)},
	{"add", 32, 0x00400000, UINT64_C(0x57be6d515e9f490d)},
	{"sub", 32, 0x00400000, UINT64_C(0x8c5f4b9cf9171ea9)},
	{"mul", 32, 0x00400000, UINT64_C(0x2f79916483f9a4fc)},
	{"div", 32, 0x00400000, UINT64_C(0x789a0a86fbfd3999)},
	{"max", 32, 0x00400000, UINT64_C(0x15ae24b6d50f22a5)},
	{"min", 32, 0x00400000, UINT64_C(0x5efec3984f2a3163)},
	{"maxnum", 32, 0x00400000, UINT64_C(0x2ae1808f59c2ef98)},
	{"minnum", 32, 0x00400000, UINT64_C(0x6857be4fa12d5d6b)},
	{"cmp", 32, 0x00400000, UINT64_C(0xe9d5fde2d6c1ff00)},
	{"cmpe", 32, 0x00400000, UINT64_C(0xdd50c0b4e819456f)},
	{"muladd", 32, 0x00400000, UINT64_C(0xc84dbad0e6544a75)},
	{"sqrt", 32, 0x00400000, UINT64_C(0x79fb321023b3c1bc)},
	{"rint", 32, 0x00400000, UINT64_C(0x764fc8a8787d7446)},
	{"tofix", 32, 0x00400000, UINT64_C(0xcf8912d099f97830)},
	{"fromfix", 32, 0x00400000, UINT64_C(0xf14bc16bdcc4a1ff)},
	{"add", 32, 0x00800000, UINT64_C(0x60fe71d9598d202e)},
	{"sub", 32, 0x00800000, UINT64_C(0x36cc7046905e55f1)},
	{"mul", 32, 0x00800000, UINT64_C(0x83e8ad8f2234b8f4)},
	{"div", 32, 0x00800000, UINT64_C(0x7226723bee3ce20e)},
	{"max", 32, 0x00800000, UINT64_C(0x0faa77eb9474ae51)},
	{"min", 32, 0x00800000, UINT64_C(0xde84453436534c10)},
	{"maxnum", 32, 0x00800000, UINT64_C(0x66aeb250afc87256)},
	{"minnum", 32, 0x00800000, UINT64_C(0x735c782b5d8f29a1)},
	{"cmp", 32, 0x00800000, UINT64_C(0x5bdb5f38563e04a4)},
	{"cmpe", 32, 0x00800000, UINT64_C(0x9256955e0c295805)},
	{"muladd", 32, 0x00800000, UINT64_C(0x8044038c213e0e53)},
	{"sqrt", 32, 0x00800000, UINT64_C(0x1093d0fca13ceb9e)},
	{"rint", 32, 0x00800000, UINT64_C(0x500224b35bcf99ef)},
	{"tofix", 32, 0x00800000, UINT64_C(0xbf73a76113ebdab7)},
	{"fromfix", 32, 0x00800000, UINT64_C(0x0601ab0dc8518822)},
	{"add", 32, 0x00c00000, UINT64_C(0xaab294326d507614)},
	{"sub", 32, 0x00c00000, UINT64_C(0xaaad6bdc6a063444)},
	{"mul", 32, 0x00c00000, UINT64_C(0xb0da9960f41eb10f)},
	{"div", 32, 0x00c00000, UINT64_C(0xe0431ddf0566aefc)},
	{"max", 32, 0x00c00000, UINT64_C(0xf761d10693a6ae71)},
	{"min", 32, 0x00c00000, UINT64_C(0x61210fb72eac65bd)},
	{"maxnum", 32, 0x00c00000, UINT64_C(0x0068d70bcc97fa83)},
	{"minnum", 32, 0x00c00000, UINT64_C(0x1513b822e4d11d75)},
	{"cmp", 32, 0x00c00000, UINT64_C(0xa1a721870073194b)},
	{"cmpe", 32, 0x00c00000, UINT64_C(0x1355844b91b80d85)},
	{"muladd", 32, 0x00c00000, UINT64_C(0x93734cd31037899b)},
	{"sqrt", 32, 0x00c00000, UINT64_C(0x54996aeac6007239)},
	{"rint", 32, 0x00c00000, UINT64_C(0xdf133ec1c56e2b65)},
	{"tofix", 32, 0x00c00000, UINT64_C(0x1a894dde39207e49)},
	{"fromfix", 32, 0x00c00000, UINT64_C(0x0d3dfd865c20d540)},
	{"add", 32, 0x01000000, UINT64_C(0x921f0268fcbb4330)},
	{"sub", 32, 0x01000000, UINT64_C(0xec72b57239077b9b)},
	{"mul", 32, 0x01000000, UINT64_C(0x5b715bbdc42b52dc)},
	{"div", 32, 0x01000000, UINT64_C(0xe748b53a23bd366b)},
	{"max", 32, 0x01000000, UINT64_C(0x2671f239c87da175)},
	{"min", 32, 0x01000000, UINT64_C(0x0973f39422058c3a)},
	{"maxnum", 32, 0x01000000, UINT64_C(0xc4b06cccb5f4f814)},
	{"minnum", 32, 0x01000000, UINT64_C(0x5d1dd0a06e68d649)},
	{"cmp", 32, 0x01000000, UINT64_C(0x6a4de2c78a723c4a)},
	{"cmpe", 32, 0x01000000, UINT64_C(0xdfb92c1662a5534b)},
	{"muladd", 32, 0x01000000, UINT64_C(0x1b7dc5c173505834)},
	{"sqrt", 32, 0x01000000, UINT64_C(0x7d204c93879db497)},
	{"rint", 32, 0x01000000, UINT64_C(0xe074ce4e22526d0b)},
	{"tofix", 32, 0x01000000, UINT64_C(0x27ad0b7eac4e50bd)},
	{"fromfix", 32, 0x01000000, UINT64_C(0x716ba5554000c1f6)},
	{"add", 32, 0x02000000, UINT64_C(0xe657621c4470c3ce)},
	{"sub", 32, 0x02000000, UINT64_C(0x4c578e0cd055e812)},
	{"mul", 32, 0x02000000, UINT64_C(0x1f03edd37e8a615a)},
	{"div", 32, 0x02000000, UINT64_C(0xc75c78505c4ea787)},
	{"max", 32, 0x02000000, UINT64_C(0x8e0c834e3124e3cd)},
	{"min", 32, 0x02000000, UINT64_C(0xcfc9a355b6461de5)},
	{"maxnum", 32, 0x02000000, UINT64_C(0x89f3faa128ea8690)},
	{"minnum", 32, 0x02000000, UINT64_C(0x4aeee8ef3f3e53f6)},
	{"cmp", 32, 0x02000000, UINT64_C(0xadb6adcab08007cf)},
	{"cmpe", 32, 0x02000000, UINT64_C(0xf0cc8a011450880b)},
	{"muladd", 32, 0x02000000, UINT64_C(0xf0f15fe2b243f022)},
	{"sqrt", 32, 0x02000000, UINT64_C(0xc2f1e9b20c242e3c)},
	{"rint", 32, 0x02000000, UINT64_C(0x40b491b806e9d9a0)},
	{"tofix", 32, 0x02000000, UINT64_C(0x76ec3e47a77aa5ba)},
	{"fromfix", 32, 0x02000000, UINT64_C(0x3dcd156bbb40b054)},
	{"add", 32, 0x03400000, UINT64_C(0xc833be9df7e52c67)},
	{"sub", 32, 0x03400000, UINT64_C(0x594bebb25eb286c2)},
	{"mul", 32, 0x03400000, UINT64_C(0xd083a5be3e281c38)},
	{"div", 32, 0x03400000, UINT64_C(0xabaa85cdd6835010)},
	{"max", 32, 0x03400000, UINT64_C(0x472fb74410842d72)},
	{"min", 32, 0x03400000, UINT64_C(0x6e64b58c43d6d45a)},
	{"maxnum", 32, 0x03400000, UINT64_C(0x8d7da114649f8f7b)},
	{"minnum", 32, 0x03400000, UINT64_C(0xf324fd708217ce6b)},
	{"cmp", 32, 0x03400000, UINT64_C(0xf9c586970b189ace)},
	{"cmpe", 32, 0x03400000, UINT64_C(0x1ce41bb20a1788ef)},
	{"muladd", 32, 0x03400000, UINT64_C(0xdf0fd98c3251ccdf)},
	{"sqrt", 32, 0x03400000, UINT64_C(0x14ba7abc51b9917c)},
	{"rint", 32, 0x03400000, UINT64_C(0x36ef45adbb4b9705)},
	{"tofix", 32, 0x03400000, UINT64_C(0xb10ba93870c77172)},
	{"fromfix", 32, 0x03400000, UINT64_C(0x0c0674659a1df9e7)},
	{"add", 64, 0x00000000, UINT64_C(0x48cff2f80782c305)},
	{"sub", 64, 0x00000000, UINT64_C(0x1cba302d533d2f12)},
	{"mul", 64, 0x00000000, UINT64_C(0x26377df97b188be5)},
	{"div", 64, 0x00000000, UINT64_C(0x65c9dd7f6d8cd95d)},
	{"max", 64, 0x00000000, UINT64_C(0xbd073dbf7bc85848)},
	{"min", 64, 0x00000000, UINT64_C(0xa13678a02e7efe1a)},
	{"maxnum", 64, 0x00000000, UINT64_C(0xcf30b16cf03d660a)},
	{"minnum", 64, 0x00000000, UINT64_C(0xb4df4d2414b93053)},
	{"cmp", 64, 0x00000000, UINT64_C(0x212b841b62a6b4a4)},
	{"cmpe", 64, 0x00000000, UINT64_C(0x5df5b1267b66b621)},
	{"muladd", 64, 0x00000000, UINT64_C(0x6dc4e9c9d5f3fd4c)},
	{"sqrt", 64, 0x00000000, UINT64_C(0x21bccab9b2e93f0f)},
	{"rint", 64, 0x00000000, UINT64_C(0x161472bff35e9029)},
	{"tofix", 64, 0x00000000, UINT64_C(0x708bb41b4eeddb02)},
	{"fromfix", 64, 0x00000000, UINT64_C(0xf554a3d7ee26b35f)},
	{"add", 64, 0x00400000, UINT64_C(0xc26df4341800a689)},
	{"sub", 64, 0x00400000, UINT64_C(0x1029e405f257e840)},
	{"mul", 64, 0x00400000, UINT64_C(0xd7c26239117aea32)},
	{"div", 64, 0x00400000, UINT64_C(0xa78dd7b019242390)},
	{"max", 64, 0x00400000, UINT64_C(0x6ef72a102852415c)},
	{"min", 64, 0x00400000, UINT64_C(0x13e4e2f17c379b89)},
	{"maxnum", 64, 0x00400000, UINT64_C(0x0ffc2fd7015306e0)},
	{"minnum", 64, 0x00400000, UINT64_C(0x58f770d51d20339c)},
	{"cmp", 64, 0x00400000, UINT64_C(0x618694a550daa78f)},
	{"cmpe", 64, 0x00400000, UINT64_C(0x1e6447d67eb96b41)},
	{"muladd", 64, 0x00400000, UINT64_C(0x2cd74130cfa57edd)},
	{"sqrt", 64, 0x00400000, UINT64_C(0xaf113ecfd7eb8d88)},
	{"rint", 64, 0x00400000, UINT64_C(0xfd5bbd8d191dcdd7)},
	{"tofix", 64, 0x00400000, UINT64_C(0xcec2b4ea3e8f64c1)},
	{"fromfix", 64, 0x00400000, UINT64_C(0x57209e81a5bb0fa0)},
	{"add", 64, 0x00800000, UINT64_C(0xab129436d2fd5259)},
	{"sub", 64, 0x00800000, UINT64_C(0x024f4c5d565074d0)},
	{"mul", 64, 0x00800000, UINT64_C(0x56e5747d5f1be384)},
	{"div", 64, 0x00800000, UINT64_C(0x0e32cef8a769f01e)},
	{"max", 64, 0x00800000, UINT64_C(0xeec7c735c1af4d96)},
	{"min", 64, 0x00800000, UINT64_C(0x42b7a36208d0732d)},
	{"maxnum", 64, 0x00800000, UINT64_C(0x6bf6ec55f3f7c1a0)},
	{"minnum", 64, 0x00800000, UINT64_C(0x4a42959da784af28)},
	{"cmp", 64, 0x00800000, UINT64_C(0x74b4d413b691de01)},
	{"cmpe", 64, 0x00800000, UINT64_C(0x34a49ec44a15964f)},
	{"muladd", 64, 0x00800000, UINT64_C(0x903ee04f5639f1de)},
	{"sqrt", 64, 0x00800000, UINT64_C(0xec00d0e1937f0085)},
	{"rint", 64, 0x00800000, UINT64_C(0x0102d02982a6eb9a)},
	{"tofix", 64, 0x00800000, UINT64_C(0xc390ff23860ca7be)},
	{"fromfix", 64, 0x00800000, UINT64_C(0xa80342a1077b8e54)},
	{"add", 64, 0x00c00000, UINT64_C(0xf1634d46791a070b)},
	{"sub", 64, 0x00c00000, UINT64_C(0xd0dab8dee27b3a09)},
	{"mul", 64, 0x00c00000, UINT64_C(0x5ea5d4e47e662dc7)},
	{"div", 64, 0x00c00000, UINT64_C(0x278fe0481e183380)},
	{"max", 64, 0x00c00000, UINT64_C(0x501c1eacb1c145f5)},
	{"min", 64, 0x00c00000, UINT64_C(0xbcb9296c8dff955d)},
	{"maxnum", 64, 0x00c00000, UINT64_C(0x98b7f939db1f0cdd)},
	{"minnum", 64, 0x00c00000, UINT64_C(0x5671fcb0664c2007)},
	{"cmp", 64, 0x00c00000, UINT64_C(0x484f8b868f851c2b)},
	{"cmpe", 64, 0x00c00000, UINT64_C(0x6523fe5a7f217c05)},
	{"muladd", 64, 0x00c00000, UINT64_C(0x0ab1aac7d32f2e80)},
	{"sqrt", 64, 0x00c00000, UINT64_C(0xdcd87d80bbb20551)},
	{"rint", 64, 0x00c00000, UINT64_C(0x9e2c4c0b452cb206)},
	{"tofix", 64, 0x00c00000, UINT64_C(0x05a118f87e4b81f8)},
	{"fromfix", 64, 0x00c00000, UINT64_C(0x9f49a21981333520)},
	{"add", 64, 0x01000000, UINT64_C(0xf6a8d41c58b0c4ce)},
	{"sub", 64, 0x01000000, UINT64_C(0x0184c80719f2524c)},
	{"mul", 64, 0x01000000, UINT64_C(0xd47132e24d693056)},
	{"div", 64, 0x01000000, UINT64_C(0x1261f8d8b091d741)},
	{"max", 64, 0x01000000, UINT64_C(0xf52a3f3f6e6f78d4)},
	{"min", 64, 0x01000000, UINT64_C(0xeaa367bc0a89a1a0)},
	{"maxnum", 64, 0x01000000, UINT64_C(0xc28567f5e7fcb966)},
	{"minnum", 64, 0x01000000, UINT64_C(0x6225fa8fe0e643aa)},
	{"cmp", 64, 0x01000000, UINT64_C(0x5f4e47dc697a9224)},
	{"cmpe", 64, 0x01000000, UINT64_C(0xd3f5249c4eeb7d8f)},
	{"muladd", 64, 0x01000000, UINT64_C(0x9eece9338e7e6588)},
	{"sqrt", 64, 0x01000000, UINT64_C(0xa0704a9a2e69db7d)},
	{"rint", 64, 0x01000000, UINT64_C(0x30ff35f54c6e73b9)},
	{"tofix", 64, 0x01000000, UINT64_C(0x99817e3eb5c70f8d)},
	{"fromfix", 64, 0x01000000, UINT64_C(0xceb0b83258d3a09e)},
	{"add", 64, 0x02000000, UINT64_C(0xce9f92a2c6ef4ddb)},
	{"sub", 64, 0x02000000, UINT64_C(0x8300b13a2ac3a8dd)},
	{"mul", 64, 0x02000000, UINT64_C(0x18c41c708085025c)},
	{"div", 64, 0x02000000, UINT64_C(0x08278e9dc784c7d0)},
	{"max", 64, 0x02000000, UINT64_C(0x936b40923d440124)},
	{"min", 64, 0x02000000, UINT64_C(0xef5afa3b317438ea)},
	{"maxnum", 64, 0x02000000, UINT64_C(0x057c8ce240958f71)},
	{"minnum", 64, 0x02000000, UINT64_C(0x7e0a97275d6fc409)},
	{"cmp", 64, 0x02000000, UINT64_C(0x228f3230ab0959ee)},
	{"cmpe", 64, 0x02000000, UINT64_C(0x6ec99dabec05386b)},
	{"muladd", 64, 0x02000000, UINT64_C(0x4b09387e0e46b923)},
	{"sqrt", 64, 0x02000000, UINT64_C(0x089dd3ee22ed2cbc)},
	{"rint", 64, 0x02000000, UINT64_C(0x1a67d1945ce4f330)},
	{"tofix", 64, 0x02000000, UINT64_C(0xda79f640369c1011)},
	{"fromfix", 64, 0x02000000, UINT64_C(0x366bc8f303c06818)},
	{"add", 64, 0x03400000, UINT64_C(0x077ce5752e7d1858)},
	{"sub", 64, 0x03400000, UINT64_C(0xfaf26abd006d84f2)},
	{"mul", 64, 0x03400000, UINT64_C(0xaeacf284832c6db4)},
	{"div", 64, 0x03400000, UINT64_C(0x81763bcedacdae61)},
	{"max", 64, 0x03400000, UINT64_C(0x49d09089ac59a349)},
	{"min", 64, 0x03400000, UINT64_C(0x041c9926946a9b82)},
	{"maxnum", 64, 0x03400000, UINT64_C(0x0f0e1a9557efdb8d)},
	{"minnum", 64, 0x03400000, UINT64_C(0x27b8c262c5c1dadd)},
	{"cmp", 64, 0x03400000, UINT64_C(0x8d8e19c2a2fe9400)},
	{"cmpe", 64, 0x03400000, UINT64_C(0x562d0d6a4b8a0e21)},
	{"muladd", 64, 0x03400000, UINT64_C(0x7660d77c737da63a)},
	{"sqrt", 64, 0x03400000, UINT64_C(0xf6081b25660533ec)},
	{"rint", 64, 0x03400000, UINT64_C(0x29ebd9acd273376d)},
	{"tofix", 64, 0x03400000, UINT64_C(0xddd0522491670fae)},
	{"fromfix", 64, 0x03400000, UINT64_C(0xf8da680431494e6c)},
	{"cvt64to32", 0, 0x00000000, UINT64_C(0x463cd651820ab8d2)},
	{"cvt32to64", 0, 0x00000000, UINT64_C(0x2b3e715ea453faad)},
	{"cvt32to16", 0, 0x00000000, UINT64_C(0x295fbc67dac0e8a3)},
	{"cvt64to16", 0, 0x00000000, UINT64_C(0x14931a96d94c7428)},
	{"cvt16to32", 0, 0x00000000, UINT64_C(0x0385af2cb53a4fa1)},
	{"cvt16to64", 0, 0x00000000, UINT64_C(0x27d369d756492ce8)},
	{"cvt64to32", 0, 0x00400000, UINT64_C(0x347843ab203d4ab3)},
	{"cvt32to64", 0, 0x00400000, UINT64_C(0xa82663e35bffc690)},
	{"cvt32to16", 0, 0x00400000, UINT64_C(0x3fdddce0c7e5a06c)},
	{"cvt64to16", 0, 0x00400000, UINT64_C(0xfa3c30b17270e02d)},
	{"cvt16to32", 0, 0x00400000, UINT64_C(0x8ea3ff213585be58)},
	{"cvt16to64", 0, 0x00400000, UINT64_C(0x365fa69df1315445)},
	{"cvt64to32", 0, 0x00800000, UINT64_C(0xbbea809036adf7a8)},
	{"cvt32to64", 0, 0x00800000, UINT64_C(0x97375cef4aa89767)},
	{"cvt32to16", 0, 0x00800000, UINT64_C(0x58b0f9cabbc2671c)},
	{"cvt64to16", 0, 0x00800000, UINT64_C(0x17683e355894a4d9)},
	{"cvt16to32", 0, 0x00800000, UINT64_C(0xb42a4d271140d708)},
	{"cvt16to64", 0, 0x00800000, UINT64_C(0x6e7042a28159c0c8)},
	{"cvt64to32", 0, 0x00c00000, UINT64_C(0x317023c91bb24441)},
	{"cvt32to64", 0, 0x00c00000, UINT64_C(0xe6dcba0168064616)},
	{"cvt32to16", 0, 0x00c00000, UINT64_C(0x2b101bd847f4b8cb)},
	{"cvt64to16", 0, 0x00c00000, UINT64_C(0x77f8cca0d62b7768)},
	{"cvt16to32", 0, 0x00c00000, UINT64_C(0x15e87fae666bbe97)},
	{"cvt16to64", 0, 0x00c00000, UINT64_C(0x641095159569793f)},
	{"cvt64to32", 0, 0x01000000, UINT64_C(0x65971ade95e2ec9b)},
	{"cvt32to64", 0, 0x01000000, UINT64_C(0x46d86e81add6e0b3)},
	{"cvt32to16", 0, 0x01000000, UINT64_C(0xac8483004f5dd24a)},
	{"cvt64to16", 0, 0x01000000, UINT64_C(0x2db33d4be5b2eb55)},
	{"cvt16to32", 0, 0x01000000, UINT64_C(0x56f7ffe484d47ceb)},
	{"cvt16to64", 0, 0x01000000, UINT64_C(0xccca4c8feb51d07e)},
	{"cvt64to32", 0, 0x02000000, UINT64_C(0xff180bb195b5cdad)},
	{"cvt32to64", 0, 0x02000000, UINT64_C(0xb3c38cd885e51406)},
	{"cvt32to16", 0, 0x02000000, UINT64_C(0x8e26846472d79453)},
	{"cvt64to16", 0, 0x02000000, UINT64_C(0x3f4de3df85db1f17)},
	{"cvt16to32", 0, 0x02000000, UINT64_C(0x8c2dea5a713c0f67)},
	{"cvt16to64", 0, 0x02000000, UINT64_C(0x194c12bbae3a2efa)},
	{"cvt64to32", 0, 0x03400000, UINT64_C(0x3492942634e6a672)},
	{"cvt32to64", 0, 0x03400000, UINT64_C(0xc3e1a8393678600d)},
	{"cvt32to16", 0, 0x03400000, UINT64_C(0x70ab8c223d42ce77)},
	{"cvt64to16", 0, 0x03400000, UINT64_C(0x8a0d12404402ce7c)},
	{"cvt16to32", 0, 0x03400000, UINT64_C(0xfb6c5c2ba83fd9b6)},
	{"cvt16to64", 0, 0x03400000, UINT64_C(0x6e7c139dabfcf19d)},
	{"cvt64to32", 0, 0x04000000, UINT64_C(0x9119c21012e355e0)},
	{"cvt32to64", 0, 0x04000000, UINT64_C(0xb605687f3a34d2a7)},
	{"cvt32to16", 0, 0x04000000, UINT64_C(0x9d7193e769e966a9)},
	{"cvt64to16", 0, 0x04000000, UINT64_C(0xfb8c5501e664aece)},
	{"cvt16to32", 0, 0x04000000, UINT64_C(0xcd7172c6269e91f3)},
	{"cvt16to64", 0, 0x04000000, UINT64_C(0x968e2a19ef265fa9)},
	{"mulx", 32, 0x00000000, UINT64_C(0x9b6fe6cbe7dfb3cb)},
	{"recps", 32, 0x00000000, UINT64_C(0xfd477fb2f023edd5)},
	{"rsqrts", 32, 0x00000000, UINT64_C(0xc256f8e72ffa6e14)},
	{"cmeq", 32, 0x00000000, UINT64_C(0x4e979cd219469c84)},
	{"cmge", 32, 0x00000000, UINT64_C(0x2bab99d199fdaae5)},
	{"cmgt", 32, 0x00000000, UINT64_C(0x324d7a2477817745)},
	{"recpe", 32, 0x00000000, UINT64_C(0xd9bd289609f72cf4)},
	{"rsqrte", 32, 0x00000000, UINT64_C(0x39956a6c1167c8c4)},
	{"recpx", 32, 0x00000000, UINT64_C(0x63ff1258ebde0f3e)},
	{"mulx", 32, 0x00400000, UINT64_C(0x98949444bbb48399)},
	{"recps", 32, 0x00400000, UINT64_C(0x75db40ed670cdf4d)},
	{"rsqrts", 32, 0x00400000, UINT64_C(0xc224b2ef961be2fb)},
	{"cmeq", 32, 0x00400000, UINT64_C(0x377fa463f693fa85)},
	{"cmge", 32, 0x00400000, UINT64_C(0x30662a5f789fab84)},
	{"cmgt", 32, 0x00400000, UINT64_C(0x219a57b81a1f45e4)},
	{"recpe", 32, 0x00400000, UINT64_C(0x3138f5dd68a9d9d5)},
	{"rsqrte", 32, 0x00400000, UINT64_C(0xa8a222549f4548f8)},
	{"recpx", 32, 0x00400000, UINT64_C(0xd4ae494a97ae10b3)},
	{"mulx", 32, 0x00800000, UINT64_C(0x666d0b8521b19363)},
	{"recps", 32, 0x00800000, UINT64_C(0x2b9480531427bda7)},
	{"rsqrts", 32, 0x00800000, UINT64_C(0xe234505b86cf6e22)},
	{"cmeq", 32, 0x00800000, UINT64_C(0x1fba2c62bf543305)},
	{"cmge", 32, 0x00800000, UINT64_C(0x400e8f31894ffdc5)},
	{"cmgt", 32, 0x00800000, UINT64_C(0xf4ece42b6178d145)},
	{"recpe", 32, 0x00800000, UINT64_C(0x441f8155af1435f2)},
	{"rsqrte", 32, 0x00800000, UINT64_C(0xe837949f28dcffd8)},
	{"recpx", 32, 0x00800000, UINT64_C(0x28274eb0b39bba8b)},
	{"mulx", 32, 0x00c00000, UINT64_C(0x72cd8c50665e7499)},
	{"recps", 32, 0x00c00000, UINT64_C(0x5d37154e7d03e237)},
	{"rsqrts", 32, 0x00c00000, UINT64_C(0x0f5cf2edff0a8975)},
	{"cmeq", 32, 0x00c00000, UINT64_C(0x6bdafa2ee4775a25)},
	{"cmge", 32, 0x00c00000, UINT64_C(0xee8645667f1dc185)},
	{"cmgt", 32, 0x00c00000, UINT64_C(0xbac5e39cc1d96764)},
	{"recpe", 32, 0x00c00000, UINT64_C(0x23141ee94c14de9c)},
	{"rsqrte", 32, 0x00c00000, UINT64_C(0xb802cef954776522)},
	{"recpx", 32, 0x00c00000, UINT64_C(0xc882a802187c272a)},
	{"mulx", 32, 0x01000000, UINT64_C(0x80c3d8504d2f4f1b)},
	{"recps", 32, 0x01000000, UINT64_C(0x212ea41a7d10a3d0)},
	{"rsqrts", 32, 0x01000000, UINT64_C(0x3908b534f9e5de9f)},
	{"cmeq", 32, 0x01000000, UINT64_C(0x812f6df7efaacc25)},
	{"cmge", 32, 0x01000000, UINT64_C(0x547ee77ab5a17e64)},
	{"cmgt", 32, 0x01000000, UINT64_C(0x6e549cce0130e705)},
	{"recpe", 32, 0x01000000, UINT64_C(0x94fe4a15b7393238)},
	{"rsqrte", 32, 0x01000000, UINT64_C(0x7b3f5a415e8c9317)},
	{"recpx", 32, 0x01000000, UINT64_C(0xf84f248d0907c064)},
	{"mulx", 32, 0x02000000, UINT64_C(0x39c9fd07be572dd1)},
	{"recps", 32, 0x02000000, UINT64_C(0xcfb02556b299e502)},
	{"rsqrts", 32, 0x02000000, UINT64_C(0x39f4e6573019598c)},
	{"cmeq", 32, 0x02000000, UINT64_C(0xffd46f2f225b3245)},
	{"cmge", 32, 0x02000000, UINT64_C(0xa96dca093f47a9c5)},
	{"cmgt", 32, 0x02000000, UINT64_C(0xb211cbb91d630405)},
	{"recpe", 32, 0x02000000, UINT64_C(0x2f6c56c1d4768781)},
	{"rsqrte", 32, 0x02000000, UINT64_C(0x352515df96aa03c1)},
	{"recpx", 32, 0x02000000, UINT64_C(0x19f5421c1c15f84c)},
	{"mulx", 32, 0x03400000, UINT64_C(0xd82fd4ab159669bd)},
	{"recps", 32, 0x03400000, UINT64_C(0x10ccbe85d1e0f35d)},
	{"rsqrts", 32, 0x03400000, UINT64_C(0xc66d26bdec2ea8d1)},
	{"cmeq", 32, 0x03400000, UINT64_C(0x257b1a6c462bd9a5)},
	{"cmge", 32, 0x03400000, UINT64_C(0x7ca4bd31ba414d25)},
	{"cmgt", 32, 0x03400000, UINT64_C(0xc743ab791aed92e5)},
	{"recpe", 32, 0x03400000, UINT64_C(0xdf85abf0db0ed18d)},
	{"rsqrte", 32, 0x03400000, UINT64_C(0x41269270c1fecee5)},
	{"recpx", 32, 0x03400000, UINT64_C(0xa4e12712cbe75513)},
	{"mulx", 64, 0x00000000, UINT64_C(0xbc74f2414427e7b1)},
	{"recps", 64, 0x00000000, UINT64_C(0xcce12d17e0bd1676)},
	{"rsqrts", 64, 0x00000000, UINT64_C(0x91f48b006aa6f69c)},
	{"cmeq", 64, 0x00000000, UINT64_C(0xcd67a940b7c0bea4)},
	{"cmge", 64, 0x00000000, UINT64_C(0xef7d07a56545a884)},
	{"cmgt", 64, 0x00000000, UINT64_C(0x3772fce22d067e24)},
	{"recpe", 64, 0x00000000, UINT64_C(0x3645e5b3b2f99cdf)},
	{"rsqrte", 64, 0x00000000, UINT64_C(0x71bbd8bfe1c563e2)},
	{"recpx", 64, 0x00000000, UINT64_C(0xf97fbd8ea46e02a8)},
	{"mulx", 64, 0x00400000, UINT64_C(0x43c148d98a52f7c6)},
	{"recps", 64, 0x00400000, UINT64_C(0x2384dd9524b09387)},
	{"rsqrts", 64, 0x00400000, UINT64_C(0x022e6e4b30b19373)},
	{"cmeq", 64, 0x00400000, UINT64_C(0x53e2161c607b16e5)},
	{"cmge", 64, 0x00400000, UINT64_C(0xc70f7d72f6d3d985)},
	{"cmgt", 64, 0x00400000, UINT64_C(0x95988a89517d8f64)},
	{"recpe", 64, 0x00400000, UINT64_C(0xcc4cf042cf264727)},
	{"rsqrte", 64, 0x00400000, UINT64_C(0xbaabdcde4263e70f)},
	{"recpx", 64, 0x00400000, UINT64_C(0xb770d1bc7eb116fb)},
	{"mulx", 64, 0x00800000, UINT64_C(0xffd909c054596f16)},
	{"recps", 64, 0x00800000, UINT64_C(0x9241a0980575300f)},
	{"rsqrts", 64, 0x00800000, UINT64_C(0xca8a40b9889d8c6f)},
	{"cmeq", 64, 0x00800000, UINT64_C(0x5e6ceaafc839d3a5)},
	{"cmge", 64, 0x00800000, UINT64_C(0xc11344aa093604c4)},
	{"cmgt", 64, 0x00800000, UINT64_C(0x64ce2bedddc5bc44)},
	{"recpe", 64, 0x00800000, UINT64_C(0x582613e4667ccfbf)},
	{"rsqrte", 64, 0x00800000, UINT64_C(0xac6ddbd6abac726f)},
	{"recpx", 64, 0x00800000, UINT64_C(0xb8de00a62ae31417)},
	{"mulx", 64, 0x00c00000, UINT64_C(0x0169132d4d633be0)},
	{"recps", 64, 0x00c00000, UINT64_C(0xd3aa8823f12e9eff)},
	{"rsqrts", 64, 0x00c00000, UINT64_C(0x737774e24b445566)},
	{"cmeq", 64, 0x00c00000, UINT64_C(0x0e9d5c8478497e65)},
	{"cmge", 64, 0x00c00000, UINT64_C(0x613940f7b429aca5)},
	{"cmgt", 64, 0x00c00000, UINT64_C(0x420ffd01f2890804)},
	{"recpe", 64, 0x00c00000, UINT64_C(0x45a45df55508900a)},
	{"rsqrte", 64, 0x00c00000, UINT64_C(0x137f3f831539ce0d)},
	{"recpx", 64, 0x00c00000, UINT64_C(0x8ca448db8e0084fc)},
	{"mulx", 64, 0x01000000, UINT64_C(0x7aa966d2d515eeb6)},
	{"recps", 64, 0x01000000, UINT64_C(0x85503124bfc5e243)},
	{"rsqrts", 64, 0x01000000, UINT64_C(0x939fc6fa02e75b78)},
	{"cmeq", 64, 0x01000000, UINT64_C(0x99410382405abd84)},
	{"cmge", 64, 0x01000000, UINT64_C(0x1873da91096ad3e5)},
	{"cmgt", 64, 0x01000000, UINT64_C(0x061a5961f3358905)},
	{"recpe", 64, 0x01000000, UINT64_C(0xdd28746d7f7ff19d)},
	{"rsqrte", 64, 0x01000000, UINT64_C(0xe1a805e47de65817)},
	{"recpx", 64, 0x01000000, UINT64_C(0x5cb40a8d4963d69b)},
	{"mulx", 64, 0x02000000, UINT64_C(0x656279d648b0a62d)},
	{"recps", 64, 0x02000000, UINT64_C(0x3579571de1dd4e0c)},
	{"rsqrts", 64, 0x02000000, UINT64_C(0x806f65dbbe923c3d)},
	{"cmeq", 64, 0x02000000, UINT64_C(0x6146f56f75d0a084)},
	{"cmge", 64, 0x02000000, UINT64_C(0x2534e370890afba5)},
	{"cmgt", 64, 0x02000000, UINT64_C(0xc937f6e524224625)},
	{"recpe", 64, 0x02000000, UINT64_C(0x8247561b34505578)},
	{"rsqrte", 64, 0x02000000, UINT64_C(0xbcaeb38f770f629a)},
	{"recpx", 64, 0x02000000, UINT64_C(0xf79b6ba5954407e9)},
	{"mulx", 64, 0x03400000, UINT64_C(0x4fa7a850577ac96c)},
	{"recps", 64, 0x03400000, UINT64_C(0xbc542126d66886e0)},
	{"rsqrts", 64, 0x03400000, UINT64_C(0xe4d049b39f65848d)},
	{"cmeq", 64, 0x03400000, UINT64_C(0x304430b011b7af85)},
	{"cmge", 64, 0x03400000, UINT64_C(0xf92d7454e13c36a5)},
	{"cmgt", 64, 0x03400000, UINT64_C(0x5e954f630111c4e4)},
	{"recpe", 64, 0x03400000, UINT64_C(0x0c7c777bd0e86a17)},
	{"rsqrte", 64, 0x03400000, UINT64_C(0x1bf3df790aad13e6)},
	{"recpx", 64, 0x03400000, UINT64_C(0xb195604fcf6924dd)},
	{"cvtodd", 0, 0x00000000, UINT64_C(0xf48c7dfad0275076)},
	{"cvtodd", 0, 0x00400000, UINT64_C(0x4a168667f0f0601b)},
	{"cvtodd", 0, 0x00800000, UINT64_C(0x1614be0ea4372cb8)},
	{"cvtodd", 0, 0x00c00000, UINT64_C(0x79ce2d83025e6a62)},
	{"cvtodd", 0, 0x01000000, UINT64_C(0x40965156770d3423)},
	{"cvtodd", 0, 0x02000000, UINT64_C(0x6ff99d72fbce7cdf)},
	{"cvtodd", 0, 0x03400000, UINT64_C(0x137200b48c8f45d2)},
	{"cvtodd", 0, 0x04000000, UINT64_C(0x3ba3fb20093736d2)},
	{"urecpe", 32, 0x00000000, UINT64_C(0xdbc11e0743b2ba49)},
	{"ursqrte", 32, 0x00000000, UINT64_C(0x2c837e9451fd54a5)},
};

#define CASES 1000

/* Each group's results and exceptions, against the model's. */
static void test_bulk(void)
{
	for (unsigned i = 0; i < ARRAY_SIZE(groups); i++) {
		uint64_t h = UINT64_C(0xCBF29CE484222325);

		rng_state = 0x534d4952 + i;
		for (unsigned k = 0; k < CASES; k++) {
			ArmFp f = {groups[i].fpcr, 0};
			uint64_t r = run_case(groups[i].name, groups[i].n, &f);

			h = fnv(fnv(h, r), f.flags);
		}
		if (h != groups[i].hash) {
			fprintf(stderr, "group %s/%u/0x%08" PRIx32 " differs\n",
			        groups[i].name, groups[i].n, groups[i].fpcr);
			test_failures++;
		}
	}
}

int main(void)
{
	RUN_TEST(test_bulk);
	return test_summary();
}
