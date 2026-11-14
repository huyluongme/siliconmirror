/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Data Processing -- Scalar Floating-Point and Advanced SIMD: the group
 * of C4.1 with bits 27:25 = 111. arm64_exec_simd_fp() decodes its table
 * down to the instruction classes, each instruction after its description
 * in C7.2.
 *
 * Implemented so far: the instructions that move data without computing,
 * DUP, INS, SMOV, UMOV (Advanced SIMD copy and scalar copy), MOVI, MVNI,
 * ORR and BIC (vector, immediate) and FMOV (vector, immediate) (Advanced
 * SIMD modified immediate), FMOV (general), FMOV (register) and FMOV
 * (scalar, immediate). The other classes of Armv8.0 stop the machine as
 * not implemented yet, naming the class.
 *
 * The Cortex-A53 of the BCM2837 has no Cryptographic Extension: the AES
 * and SHA classes are UNDEFINED, as are those that only hold instructions
 * newer than Armv8.0 (FEAT_FP16, FEAT_RDM, FEAT_DotProd and others).
 */

#include <stdio.h>

#include "cpu/arm64/internal.h"
#include "util/bits.h"

/* Stop the machine on an instruction of a class not implemented yet. */
static Arm64Exec not_yet(Arm64Cpu *c, const char *class_name)
{
	snprintf(c->detail, sizeof(c->detail), "%s", class_name);
	return ARM64_EXEC_UNIMPLEMENTED;
}

/* Replicate the low esize bytes of value to fill 64 bits. */
static uint64_t replicate64(uint64_t value, unsigned esize)
{
	value &= bytes_mask(esize);
	for (unsigned width = 8 * esize; width < 64; width *= 2)
		value |= value << width;
	return value;
}

/*
 * LowestSetBitNZ() of the low `bits` bits of imm5, the log2 of the size
 * of an element in bytes; `bits` when they are all clear.
 */
static unsigned lowest_set_bit(unsigned imm5, unsigned bits)
{
	unsigned i = 0;

	while (i < bits && !(imm5 & (1u << i)))
		i++;
	return i;
}

/*
 * Advanced SIMD copy, 0 Q op 01110000 imm5 0 imm4 1 Rn Rd, and Advanced
 * SIMD scalar copy, 01 op 11110000 imm5 0 imm4 1 Rn Rd: DUP (element),
 * C7.2.40, DUP (general), C7.2.41, INS (element), C7.2.202, INS
 * (general), C7.2.203, SMOV, C7.2.323, and UMOV, C7.2.430.
 */
static Arm64Exec copy(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), op = bit32(insn, 29);
	unsigned imm5 = field32(insn, 20, 16), imm4 = field32(insn, 14, 11);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	unsigned log2 = lowest_set_bit(imm5, 4);
	unsigned size = 1u << log2;          /* bytes per element */
	unsigned index = imm5 >> (log2 + 1); /* imm5[4:size+1] */
	Arm64Exec result;

	if (scalar) {
		/* DUP (element), scalar: Vd = one element of Vn. */
		if (op || imm4 != 0 || log2 == 4)
			return ARM64_EXEC_UNDEFINED;
		if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
			return result;
		set_scalar(c, d, size, get_elem(c, n, index, size));
		return ARM64_EXEC_OK;
	}
	if (op) { /* INS (element) */
		if (!q || log2 == 4)
			return ARM64_EXEC_UNDEFINED;
		if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
			return result;
		/* src_index = imm4[3:size]; imm4[size-1:0] is IGNORED. */
		set_elem(c, d, index, size, get_elem(c, n, imm4 >> log2, size));
		return ARM64_EXEC_OK;
	}

	switch (imm4) {
	case 0x0:   /* DUP (element) */
	case 0x1: { /* DUP (general) */
		uint64_t element;

		if (log2 == 4 || (log2 == 3 && !q))
			return ARM64_EXEC_UNDEFINED;
		if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
			return result;
		element = imm4 == 0 ? get_elem(c, n, index, size)
		                    : get_x(c, n); /* imm5[4:size+1] is IGNORED */
		element = replicate64(element, size);
		set_v(c, d, element, q ? element : 0);
		return ARM64_EXEC_OK;
	}
	case 0x3: /* INS (general) */
		if (!q || log2 == 4)
			return ARM64_EXEC_UNDEFINED;
		if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
			return result;
		set_elem(c, d, index, size, get_x(c, n));
		return ARM64_EXEC_OK;
	case 0x5: { /* SMOV: of a byte, halfword or word (not imm5 = xx000) */
		unsigned regsize = q ? 64 : 32;

		if (log2 >= 3 || regsize <= 8 * size)
			return ARM64_EXEC_UNDEFINED;
		if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
			return result;
		set_x(c, d,
		      sign_extend64(get_elem(c, n, index, size), 8 * size) &
		          datasize_mask(regsize));
		return ARM64_EXEC_OK;
	}
	case 0x7: /* UMOV: X from a doubleword, W from a smaller element */
		if (log2 == 4 || (q ? log2 != 3 : log2 == 3))
			return ARM64_EXEC_UNDEFINED;
		if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
			return result;
		set_x(c, d, get_elem(c, n, index, size));
		return ARM64_EXEC_OK;
	default:
		return ARM64_EXEC_UNDEFINED;
	}
}

/*
 * AdvSIMDExpandImm(), J1.3: the 64-bit immediate of the Advanced SIMD
 * modified immediate class.
 */
static uint64_t expand_imm(bool op, unsigned cmode, uint64_t imm8)
{
	switch (cmode >> 1) {
	case 0:
		return replicate64(imm8, 4);
	case 1:
		return replicate64(imm8 << 8, 4);
	case 2:
		return replicate64(imm8 << 16, 4);
	case 3:
		return replicate64(imm8 << 24, 4);
	case 4:
		return replicate64(imm8, 2);
	case 5:
		return replicate64(imm8 << 8, 2);
	case 6:
		return replicate64((cmode & 1) ? imm8 << 16 | 0xffff : imm8 << 8 | 0xff,
		                   4);
	default:
		break;
	}
	if (!(cmode & 1) && !op)
		return replicate64(imm8, 1);
	if (!(cmode & 1)) { /* each bit of imm8 makes a byte */
		uint64_t imm64 = 0;

		for (unsigned i = 0; i < 8; i++)
			if (imm8 & (1u << i))
				imm64 |= UINT64_C(0xff) << (8 * i);
		return imm64;
	}
	if (!op) { /* a single-precision value, VFPExpandImm() */
		uint64_t b6 = (imm8 >> 6) & 1;
		uint64_t imm32 = (imm8 >> 7) << 31 | (b6 ^ 1) << 30 |
		                 (b6 ? UINT64_C(0x1f) : 0) << 25 | (imm8 & 0x3f) << 19;

		return replicate64(imm32, 4);
	}
	/* A double-precision value. */
	uint64_t b6 = (imm8 >> 6) & 1;

	return (imm8 >> 7) << 63 | (b6 ^ 1) << 62 |
	       (b6 ? UINT64_C(0xff) : 0) << 54 | (imm8 & 0x3f) << 48;
}

/*
 * Advanced SIMD modified immediate, 0 Q op 0111100000 a b c cmode o2 1
 * d e f g h Rd: MOVI, C7.2.247, MVNI, C7.2.251, ORR (vector, immediate),
 * C7.2.255, BIC (vector, immediate) and FMOV (vector, immediate),
 * C7.2.155.
 */
static Arm64Exec modified_immediate(Arm64Cpu *c, uint32_t insn)
{
	bool q = bit32(insn, 30), op = bit32(insn, 29);
	unsigned cmode = field32(insn, 15, 12);
	unsigned d = field32(insn, 4, 0);
	uint64_t imm8 = field32(insn, 18, 16) << 5 | field32(insn, 9, 5);
	bool shifted = cmode < 12 && (cmode & 1); /* ORR and BIC */
	Arm64Exec result;

	/* o2: FMOV (vector, immediate) of half precision, FEAT_FP16. */
	if (bit32(insn, 11))
		return ARM64_EXEC_UNDEFINED;
	/* 64-bit MOVI as a scalar has no FMOV of double precision. */
	if (op && cmode == 15 && !q)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	uint64_t imm = expand_imm(op, cmode, imm8);
	uint64_t lo = c->v[d].d[0], hi = c->v[d].d[1];

	if (shifted && !op) { /* ORR */
		lo |= imm;
		hi |= imm;
	} else if (shifted) { /* BIC */
		lo &= ~imm;
		hi &= ~imm;
	} else if (op && cmode < 14) { /* MVNI */
		lo = hi = ~imm;
	} else { /* MOVI, FMOV */
		lo = hi = imm;
	}
	/* V{datasize}(d) = result: a 64-bit result clears the upper half. */
	if (!q)
		hi = 0;
	set_v(c, d, lo, hi);
	return ARM64_EXEC_OK;
}

/*
 * The size in bytes of a scalar floating-point type ftype: 4 for single
 * precision, 8 for double precision, or 0 for half precision (FEAT_FP16)
 * and the reserved value, which are UNDEFINED here.
 */
static unsigned ftype_size(uint32_t insn)
{
	switch (field32(insn, 23, 22)) {
	case 0:
		return 4;
	case 1:
		return 8;
	default:
		return 0;
	}
}

/*
 * Conversion between floating-point and integer, sf 0 S 11110 ftype 1
 * rmode opcode 000000 Rn Rd. Only FMOV (general), C7.2.157, so far.
 */
static Arm64Exec fp_int_conversion(Arm64Cpu *c, uint32_t insn)
{
	bool sf = bit32(insn, 31);
	unsigned ftype = field32(insn, 23, 22), rmode = field32(insn, 20, 19);
	unsigned opcode = field32(insn, 18, 16);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	Arm64Exec result;

	if (bit32(insn, 29))
		return ARM64_EXEC_UNDEFINED;
	if (opcode >> 1 != 3) {
		/* ftype 11 needs FEAT_FP16; 10 is UNALLOCATED. */
		if (ftype >= 2)
			return ARM64_EXEC_UNDEFINED;
		return not_yet(c, "Conversion between floating-point and integer");
	}
	/*
	 * FMOV: rmode 00 between a register and S (sf = 0, ftype = 00) or D
	 * (sf = 1, ftype = 01); rmode 01 between X and V.D[1] (ftype = 10).
	 * Half precision needs FEAT_FP16; rmode 1x holds newer instructions.
	 */
	if (!(rmode == 0 && ftype == (sf ? 1u : 0u)) &&
	    !(rmode == 1 && ftype == 2 && sf))
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	if (opcode & 1) { /* general to SIMD&FP */
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
 * Floating-point data-processing (1 source), M 0 S 11110 ftype 1 opcode
 * 10000 Rn Rd, with M and S clear. Only FMOV (register), C7.2.156, so
 * far. Opcodes 1xxxxx are UNALLOCATED. Half precision is FEAT_FP16,
 * except FCVT from it (opcode 00010x).
 */
static Arm64Exec fp_one_source(Arm64Cpu *c, uint32_t insn)
{
	unsigned size = ftype_size(insn);
	unsigned opcode = field32(insn, 20, 15);
	Arm64Exec result;

	if (opcode & 0x20)
		return ARM64_EXEC_UNDEFINED;
	if (opcode != 0) {
		if (size == 0 && !(field32(insn, 23, 22) == 3 && opcode >> 1 == 2))
			return ARM64_EXEC_UNDEFINED;
		return not_yet(c, "Floating-point data-processing (1 source)");
	}
	if (size == 0)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	set_scalar(c, field32(insn, 4, 0), size, get_d(c, field32(insn, 9, 5)));
	return ARM64_EXEC_OK;
}

/*
 * Floating-point immediate, M 0 S 11110 ftype 1 imm8 100 imm5 Rd, with
 * M and S clear: FMOV (scalar, immediate), C7.2.158, with VFPExpandImm().
 */
static Arm64Exec fp_immediate(Arm64Cpu *c, uint32_t insn)
{
	unsigned size = ftype_size(insn);
	uint64_t imm8 = field32(insn, 20, 13);
	Arm64Exec result;

	if (field32(insn, 9, 5) != 0 || size == 0)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	/* The same values as FMOV (vector, immediate), cmode = 1111. */
	set_scalar(c, field32(insn, 4, 0), size, expand_imm(size == 8, 15, imm8));
	return ARM64_EXEC_OK;
}

/*
 * The scalar floating-point classes, op0 = x0x1: M[31] 0 S[29] 1 1111
 * op1[24:23] op2[22:19] op3[18:10], ftype in bits 23:22. Bit 31 is sf in
 * the two conversion classes, and M in the others, where M and S must be
 * clear. Outside the conversions and FCVT, ftype 11, half precision, is
 * FEAT_FP16, and ftype 10 UNALLOCATED.
 */
static Arm64Exec fp_scalar(Arm64Cpu *c, uint32_t insn)
{
	unsigned op3 = field32(insn, 18, 10);
	bool wide = field32(insn, 23, 22) >= 2;

	if (!bit32(insn, 24) && !bit32(insn, 21)) {
		if (bit32(insn, 29) || wide)
			return ARM64_EXEC_UNDEFINED;
		return not_yet(c, "Conversion between floating-point and "
		                  "fixed-point");
	}
	if (!bit32(insn, 24) && (op3 & 0x3f) == 0x00)
		return fp_int_conversion(c, insn);
	if (bit32(insn, 31) || bit32(insn, 29) || (op3 & 0x3f) == 0x20)
		return ARM64_EXEC_UNDEFINED;
	if (!bit32(insn, 24) && (op3 & 0x1f) == 0x10)
		return fp_one_source(c, insn);
	if (!bit32(insn, 24) && (op3 & 0x7) == 0x4)
		return fp_immediate(c, insn);
	if (wide)
		return ARM64_EXEC_UNDEFINED;
	if (bit32(insn, 24))
		return not_yet(c, "Floating-point data-processing (3 source)");
	if ((op3 & 0xf) == 0x8)
		return not_yet(c, "Floating-point compare");
	switch (op3 & 3) {
	case 1:
		return not_yet(c, "Floating-point conditional compare");
	case 2:
		return not_yet(c, "Floating-point data-processing (2 source)");
	default:
		return not_yet(c, "Floating-point conditional select");
	}
}

/*
 * The Advanced SIMD scalar classes and the SHA classes, op0 = 01x1:
 * 01 U 11110 op1[24:23] op2[22:19] op3[18:10].
 */
static Arm64Exec simd_scalar(Arm64Cpu *c, uint32_t insn)
{
	unsigned op1 = field32(insn, 24, 23), op2 = field32(insn, 22, 19);
	unsigned op3 = field32(insn, 18, 10);

	if (op1 & 2) {
		if (!(op3 & 1))
			return not_yet(c, "Advanced SIMD scalar x indexed element");
		if (op1 == 3)
			return ARM64_EXEC_UNDEFINED;
		return not_yet(c, "Advanced SIMD scalar shift by immediate");
	}
	if (op3 & 1) {
		/* op2 = 10xx: FEAT_FP16; op3[5]: FEAT_RDM. */
		if ((op2 & 0xc) == 0 && !(op3 & 0x20))
			return op1 == 0 ? copy(c, insn, true) : ARM64_EXEC_UNDEFINED;
		if (!(op2 & 4))
			return ARM64_EXEC_UNDEFINED;
		return not_yet(c, "Advanced SIMD scalar three same");
	}
	/* op2 = x0xx: the SHA classes and UNALLOCATED. */
	if (!(op2 & 4))
		return ARM64_EXEC_UNDEFINED;
	if (!(op3 & 2))
		return not_yet(c, "Advanced SIMD scalar three different");
	if (op3 >> 7 != 0)
		return ARM64_EXEC_UNDEFINED;
	if ((op2 & 7) == 4)
		return not_yet(c, "Advanced SIMD scalar two-register miscellaneous");
	if ((op2 & 7) == 6)
		return not_yet(c, "Advanced SIMD scalar pairwise");
	return ARM64_EXEC_UNDEFINED; /* SHA, FEAT_FP16, UNALLOCATED */
}

/*
 * The Advanced SIMD vector classes and the AES class, op0 = 0xx0:
 * 0 Q U 01110 op1[24:23] op2[22:19] op3[18:10].
 */
static Arm64Exec simd_vector(Arm64Cpu *c, uint32_t insn)
{
	unsigned op1 = field32(insn, 24, 23), op2 = field32(insn, 22, 19);
	unsigned op3 = field32(insn, 18, 10);

	if (op1 & 2) {
		if (!(op3 & 1))
			return not_yet(c, "Advanced SIMD vector x indexed element");
		if (op1 == 3)
			return ARM64_EXEC_UNDEFINED;
		if (op2 == 0)
			return modified_immediate(c, insn);
		return not_yet(c, "Advanced SIMD shift by immediate");
	}
	if (op3 & 1) {
		/* op2 = 10xx: FEAT_FP16; op3[5]: three-register extension. */
		if ((op2 & 0xc) == 0 && !(op3 & 0x20))
			return op1 == 0 ? copy(c, insn, false) : ARM64_EXEC_UNDEFINED;
		if (!(op2 & 4))
			return ARM64_EXEC_UNDEFINED;
		return not_yet(c, "Advanced SIMD three same");
	}
	if (!(op2 & 4)) {
		if (op3 & 0x20)
			return ARM64_EXEC_UNDEFINED;
		if (bit32(insn, 29))
			return not_yet(c, "Advanced SIMD extract");
		if (op3 & 2)
			return not_yet(c, "Advanced SIMD permute");
		return not_yet(c, "Advanced SIMD table lookup");
	}
	if (!(op3 & 2))
		return not_yet(c, "Advanced SIMD three different");
	if (op3 >> 7 != 0)
		return ARM64_EXEC_UNDEFINED;
	if ((op2 & 7) == 4)
		return not_yet(c, "Advanced SIMD two-register miscellaneous");
	if ((op2 & 7) == 6)
		return not_yet(c, "Advanced SIMD across lanes");
	return ARM64_EXEC_UNDEFINED; /* AES, FEAT_FP16, UNALLOCATED */
}

Arm64Exec arm64_exec_simd_fp(Arm64Cpu *c, uint32_t insn)
{
	if (bit32(insn, 28)) {
		if (!bit32(insn, 30))
			return fp_scalar(c, insn); /* x0x1 */
		if (bit32(insn, 31))
			return ARM64_EXEC_UNDEFINED; /* 11x1 */
		return simd_scalar(c, insn);     /* 01x1 */
	}
	/* 1xx0: UNALLOCATED, or cryptographic classes newer than Armv8.0. */
	if (bit32(insn, 31))
		return ARM64_EXEC_UNDEFINED;
	return simd_vector(c, insn); /* 0xx0 */
}
