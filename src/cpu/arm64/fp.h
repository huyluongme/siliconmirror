/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The floating-point arithmetic of Armv8.0-A, as the Arm ARM's shared
 * pseudocode defines it (J1.4.3: FPUnpack, FPRound, FPProcessNaNs and the
 * operations on them), done on integers: the host's floating point,
 * whose NaNs and flags differ, plays no part.
 *
 * Values are bit patterns of n = 16, 32 or 64 bits, half, single or
 * double precision. What the Cortex-A53 has: FPCR.AHP, DN, FZ and RMode;
 * no FEAT_FP16 (half precision only in conversions, so FZ16 is 0), no
 * FEAT_AFP (FPCR.AH is 0), and no trapped exceptions (the enable bits
 * read 0). The exceptions an operation raises are FPSR's cumulative
 * bits, gathered in ArmFp.flags.
 */

#ifndef CPU_ARM64_FP_H
#define CPU_ARM64_FP_H

#include <stdbool.h>
#include <stdint.h>

/* The FPCR bits that the arithmetic reads. */
#define FPCR_AHP (UINT32_C(1) << 26)
#define FPCR_DN (UINT32_C(1) << 25)
#define FPCR_FZ (UINT32_C(1) << 24)
#define FPCR_RMODE_SHIFT 22

/* FPSR's cumulative exception bits. */
#define FPSR_IOC (UINT32_C(1) << 0) /* invalid operation */
#define FPSR_DZC (UINT32_C(1) << 1) /* division by zero */
#define FPSR_OFC (UINT32_C(1) << 2) /* overflow */
#define FPSR_UFC (UINT32_C(1) << 3) /* underflow */
#define FPSR_IXC (UINT32_C(1) << 4) /* inexact */
#define FPSR_IDC (UINT32_C(1) << 7) /* input denormal */

/* FPRounding: the first four in FPCR.RMode's order. */
typedef enum FpRounding {
	FP_TIEEVEN,
	FP_POSINF,
	FP_NEGINF,
	FP_ZERO,
	FP_TIEAWAY,
	FP_ODD,
} FpRounding;

typedef struct ArmFp {
	uint32_t fpcr;
	uint32_t flags; /* the FPSR bits that the operations raised */
} ArmFp;

/* FPRoundingMode(): FPCR.RMode. */
FpRounding fp_rounding(uint32_t fpcr);

/* FPDefaultNaN(), and the sign-only operations FPAbs() and FPNeg(). */
uint64_t fp_default_nan(unsigned n);
uint64_t fp_abs(unsigned n, uint64_t a);
uint64_t fp_neg(unsigned n, uint64_t a);

/* FPAdd(), FPSub(), FPMul(), FPDiv(), FPSqrt(), rounding as FPCR says. */
uint64_t fp_add(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
uint64_t fp_sub(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
uint64_t fp_mul(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
uint64_t fp_div(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
uint64_t fp_sqrt(ArmFp *f, unsigned n, uint64_t a);

/* FPMulAdd(): addend + a * b, rounded once. */
uint64_t fp_muladd(ArmFp *f, unsigned n, uint64_t addend, uint64_t a,
                   uint64_t b);

/* FPMax(), FPMin(), FPMaxNum() and FPMinNum(). */
uint64_t fp_max(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
uint64_t fp_min(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
uint64_t fp_maxnum(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
uint64_t fp_minnum(ArmFp *f, unsigned n, uint64_t a, uint64_t b);

/*
 * FPCompare(): NZCV in bits 3:0, 0011 unordered, 0110 equal, 1000 less,
 * 0010 greater. A NaN raises Invalid Operation if it is signalling, or
 * with signal_nans if it is any.
 */
unsigned fp_compare(ArmFp *f, unsigned n, uint64_t a, uint64_t b,
                    bool signal_nans);

/*
 * FPCompareEQ(), FPCompareGE() and FPCompareGT(): a NaN makes them false,
 * raising Invalid Operation if it is signalling, or, for GE and GT, if it
 * is any.
 */
bool fp_compare_eq(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
bool fp_compare_ge(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
bool fp_compare_gt(ArmFp *f, unsigned n, uint64_t a, uint64_t b);

/* FPMulX(): FPMul(), but infinity times zero is 2.0. */
uint64_t fp_mulx(ArmFp *f, unsigned n, uint64_t a, uint64_t b);

/*
 * FPRecipStepFused() and FPRSqrtStepFused(): 2 - a * b and (3 - a * b) / 2,
 * rounded once, for FRECPS and FRSQRTS.
 */
uint64_t fp_recip_step(ArmFp *f, unsigned n, uint64_t a, uint64_t b);
uint64_t fp_rsqrt_step(ArmFp *f, unsigned n, uint64_t a, uint64_t b);

/* FPRecipEstimate(), FPRSqrtEstimate() and FPRecpX(), of 8-bit precision. */
uint64_t fp_recip_estimate(ArmFp *f, unsigned n, uint64_t a);
uint64_t fp_rsqrt_estimate(ArmFp *f, unsigned n, uint64_t a);
uint64_t fp_recpx(ArmFp *f, unsigned n, uint64_t a);

/* UnsignedRecipEstimate() and UnsignedRSqrtEstimate(), of a word. */
uint32_t fp_unsigned_recip_estimate(uint32_t a);
uint32_t fp_unsigned_rsqrt_estimate(uint32_t a);

/* FPRoundInt(): a to an integral value; with exact, Inexact if it moved. */
uint64_t fp_round_int(ArmFp *f, unsigned n, uint64_t a, FpRounding rounding,
                      bool exact);

/*
 * FPToFixed(): a * 2^fbits rounded to an m-bit integer, saturated, signed
 * or not; returned in the low m bits.
 */
uint64_t fp_to_fixed(ArmFp *f, unsigned n, uint64_t a, unsigned fbits,
                     bool is_unsigned, FpRounding rounding, unsigned m);

/* FixedToFP(): the m-bit integer a / 2^fbits, signed or not, to n bits. */
uint64_t fp_from_fixed(ArmFp *f, unsigned n, uint64_t a, unsigned m,
                       unsigned fbits, bool is_unsigned, FpRounding rounding);

/* FPConvert(): a, of from bits, to to bits. */
uint64_t fp_convert(ArmFp *f, unsigned to, unsigned from, uint64_t a,
                    FpRounding rounding);

#endif /* CPU_ARM64_FP_H */
