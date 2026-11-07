/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the Data Processing -- Immediate instructions of the
 * AArch64 model (src/cpu/arm64/dp_imm.c). Every encoding was produced by
 * the GNU assembler, unless a comment says otherwise. Section numbers
 * refer to the Arm Architecture Reference Manual, ARM DDI 0487 M.d.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm64_test.h"
#include "cpu/arm64/internal.h"
#include "util/macros.h"

#define N ARM64_NZCV_N
#define Z ARM64_NZCV_Z
#define C ARM64_NZCV_C
#define V ARM64_NZCV_V

/* SInt() of the low 32 bits of x. */
static int64_t sint32(uint64_t x)
{
	return (int64_t)(x & UINT32_MAX) - (x & 0x80000000 ? INT64_C(1) << 32 : 0);
}

static void test_add_with_carry_32_bit(void)
{
	/* Compare with the integer arithmetic of J1.4.3.218. */
	static const uint64_t values[] = {
		0,          1,          2,          0x12345678, 0x7ffffffe, 0x7fffffff,
		0x80000000, 0x80000001, 0xedcba987, 0xfffffffe, 0xffffffff,
	};

	for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
		for (unsigned j = 0; j < ARRAY_SIZE(values); j++) {
			for (unsigned carry_in = 0; carry_in < 2; carry_in++) {
				uint64_t x = values[i], y = values[j];
				uint64_t unsigned_sum = x + y + carry_in;
				int64_t signed_sum = sint32(x) + sint32(y) + carry_in;
				uint64_t expected = unsigned_sum & UINT32_MAX;
				uint32_t expected_nzcv =
					(expected & 0x80000000 ? N : 0) | (expected == 0 ? Z : 0) |
					(expected != unsigned_sum ? C : 0) |
					(sint32(expected) != signed_sum ? V : 0);
				uint32_t nzcv;

				CHECK_EQ(add_with_carry(32, x, y, carry_in, &nzcv), expected);
				CHECK_EQ(nzcv, expected_nzcv);
			}
		}
	}

	/* Bits above bit 31 of the operands are ignored. */
	uint32_t nzcv;

	CHECK_EQ(add_with_carry(32, 0xffffffff00000001, 1, 0, &nzcv), 2);
	CHECK_EQ(nzcv, 0);
}

static void test_add_with_carry_64_bit(void)
{
	static const struct {
		uint64_t x, y;
		unsigned carry_in;
		uint64_t result;
		uint32_t nzcv;
	} cases[] = {
		{1, 2, 0, 3, 0},
		{0x7fffffffffffffff, 1, 0, 0x8000000000000000, N | V},
		{UINT64_MAX, 1, 0, 0, Z | C},
		{UINT64_MAX, UINT64_MAX, 1, UINT64_MAX, N | C},
		{0x8000000000000000, 0x8000000000000000, 0, 0, Z | C | V},
		/* x - y is x + NOT(y) + 1. */
		{5, ~(uint64_t)5, 1, 0, Z | C},
		{0, ~(uint64_t)1, 1, UINT64_MAX, N},
		{0x8000000000000000, ~(uint64_t)1, 1, 0x7fffffffffffffff, C | V},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		uint32_t nzcv;

		CHECK_EQ(add_with_carry(64, cases[i].x, cases[i].y, cases[i].carry_in,
		                        &nzcv),
		         cases[i].result);
		CHECK_EQ(nzcv, cases[i].nzcv);
	}
}

static void test_movz(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[0] = 0x5555555555555555;
	arm64_test_exec(m, c, 0xd2e24680); /* MOVZ X0, #0x1234, LSL #48 */
	CHECK_EQ(c->x[0], 0x1234000000000000);

	c->x[1] = UINT64_MAX;
	arm64_test_exec(m, c, 0x52b579a1); /* MOVZ W1, #0xabcd, LSL #16 */
	CHECK_EQ(c->x[1], 0xabcd0000);

	/* Rd 31 is the zero register: SP does not change. */
	c->sp_el[3] = 0x8000;
	arm64_test_exec(m, c, 0xd280003f); /* MOVZ XZR, #1 */
	CHECK_EQ(c->sp_el[3], 0x8000);
	machine_destroy(m);
}

static void test_movn(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	arm64_test_exec(m, c, 0x92800002); /* MOVN X2, #0 */
	CHECK_EQ(c->x[2], UINT64_MAX);

	c->x[3] = UINT64_MAX;
	arm64_test_exec(m, c, 0x12800003); /* MOVN W3, #0 */
	CHECK_EQ(c->x[3], 0xffffffff);

	arm64_test_exec(m, c, 0x92a24684); /* MOVN X4, #0x1234, LSL #16 */
	CHECK_EQ(c->x[4], 0xffffffffedcbffff);

	arm64_test_exec(m, c,
	                0x92800027); /* MOV X7, #-2, the alias of MOVN X7, #1 */
	CHECK_EQ(c->x[7], 0xfffffffffffffffe);
	machine_destroy(m);
}

static void test_movk(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[5] = 0x1111222233334444;
	arm64_test_exec(m, c, 0xf2d7dde5); /* MOVK X5, #0xbeef, LSL #32 */
	CHECK_EQ(c->x[5], 0x1111beef33334444);

	/* A 32-bit MOVK keeps bits 31:16 and clears bits 63:32. */
	c->x[6] = 0xffffffff12345678;
	arm64_test_exec(m, c, 0x7297dde6); /* MOVK W6, #0xbeef */
	CHECK_EQ(c->x[6], 0x1234beef);
	machine_destroy(m);
}

static void test_move_wide_unallocated(void)
{
	static const uint32_t insns[] = {
		0x52c00000, /* MOVZ W0 with hw = 10: no bit 32 in a W register */
		0x32800000, /* opc = 01 */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

static void test_add_sub_imm(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* ADD and SUB leave the flags alone. */
	c->nzcv = N | C;
	c->x[1] = 0x1000;
	arm64_test_exec(m, c, 0x91048c20); /* ADD X0, X1, #0x123 */
	CHECK_EQ(c->x[0], 0x1123);
	arm64_test_exec(m, c, 0x91400422); /* ADD X2, X1, #1, LSL #12 */
	CHECK_EQ(c->x[2], 0x2000);

	/* A 32-bit result wraps around and clears bits 63:32. */
	c->x[1] = 0xffffffff;
	c->x[3] = UINT64_MAX;
	arm64_test_exec(m, c, 0x11000423); /* ADD W3, W1, #1 */
	CHECK_EQ(c->x[3], 0);
	CHECK_EQ(c->nzcv, N | C);
	machine_destroy(m);
}

static void test_stack_pointer_operands(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* At EL3 with PSTATE.SP = 1, register 31 means SP_EL3. */
	c->x[1] = 0x8000;
	arm64_test_exec(m, c, 0x9100003f); /* MOV SP, X1 */
	CHECK_EQ(c->sp_el[3], 0x8000);

	arm64_test_exec(m, c, 0xd10043ff); /* SUB SP, SP, #0x10 */
	CHECK_EQ(c->sp_el[3], 0x7ff0);

	arm64_test_exec(m, c, 0x910003e7); /* MOV X7, SP */
	CHECK_EQ(c->x[7], 0x7ff0);

	/* C6.2.5: a 32-bit result written to SP is zero-extended. */
	c->x[1] = 0xffffffff00001234;
	arm64_test_exec(m, c, 0x1100003f); /* MOV WSP, W1 */
	CHECK_EQ(c->sp_el[3], 0x1234);
	CHECK_EQ(c->sp_el[0], 0);
	machine_destroy(m);
}

static void test_flag_setting(void)
{
	static const struct {
		uint32_t insn;
		uint64_t x1;
		uint32_t nzcv;
	} cases[] = {
		{0xf100043f, 1, Z | C},          /* CMP X1, #1 */
		{0xf100083f, 1, N},              /* CMP X1, #2 */
		{0x3100043f, 0x7fffffff, N | V}, /* CMN W1, #1 */
		{0xb1000420, UINT64_MAX, Z | C}, /* ADDS X0, X1, #1 */
		{0x71000420, 0x80000000, C | V}, /* SUBS W0, W1, #1 */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		c->x[1] = cases[i].x1;
		c->nzcv = N | Z | C | V;
		arm64_test_exec(m, c, cases[i].insn);
		CHECK_EQ(c->nzcv, cases[i].nzcv);
		machine_destroy(m);
	}

	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = UINT64_MAX;
	arm64_test_exec(m, c, 0xb1000420); /* ADDS X0, X1, #1 */
	CHECK_EQ(c->x[0], 0);

	c->x[0] = UINT64_MAX;
	c->x[1] = 0x80000000;
	arm64_test_exec(m, c, 0x71000420); /* SUBS W0, W1, #1 */
	CHECK_EQ(c->x[0], 0x7fffffff);

	/* Rn 31 is SP, but Rd 31 of a flag-setting form is the zero register. */
	c->sp_el[3] = 0x8000;
	arm64_test_exec(m, c, 0xf10003ff); /* CMP SP, #0 */
	CHECK_EQ(c->nzcv, C);
	CHECK_EQ(c->sp_el[3], 0x8000);
	machine_destroy(m);
}

static void test_adr(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);
	uint64_t pc;

	pc = c->pc;
	arm64_test_exec(m, c, 0x10000800); /* ADR X0, . + 0x100 */
	CHECK_EQ(c->x[0], pc + 0x100);

	pc = c->pc;
	arm64_test_exec(m, c, 0x10ffffe1); /* ADR X1, . - 4 */
	CHECK_EQ(c->x[1], pc - 4);

	/* The limits of the 21-bit signed offset. */
	pc = c->pc;
	arm64_test_exec(m, c, 0x10800002); /* ADR X2, . - 0x100000 */
	CHECK_EQ(c->x[2], pc - 0x100000);

	pc = c->pc;
	arm64_test_exec(m, c, 0x707fffe3); /* ADR X3, . + 0xfffff */
	CHECK_EQ(c->x[3], pc + 0xfffff);
	machine_destroy(m);
}

static void test_adrp(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* Start near the end of a page so that the page offset matters. */
	c->pc = 0x80ff4;
	arm64_test_exec(m, c, 0xb0000000); /* ADRP X0, . + 1 page */
	CHECK_EQ(c->x[0], 0x81000);

	arm64_test_exec(m, c, 0xf0ffffe1); /* ADRP X1, . - 1 page, at 0x80ff8 */
	CHECK_EQ(c->x[1], 0x7f000);

	arm64_test_exec(m, c, 0x90800002); /* ADRP X2, . - 4GB, at 0x80ffc */
	CHECK_EQ(c->x[2], 0xffffffff00080000);
	machine_destroy(m);
}

static void test_logical_imm(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 0xfedcba987654321f;
	arm64_test_exec(m, c, 0x92401c20); /* AND X0, X1, #0xff */
	CHECK_EQ(c->x[0], 0x1f);

	c->x[2] = UINT64_MAX;
	arm64_test_exec(m, c, 0x12000422); /* AND W2, W1, #3 */
	CHECK_EQ(c->x[2], 3);

	arm64_test_exec(m, c, 0xb204cc23); /* ORR X3, X1, #0xf0f0f0f0f0f0f0f0 */
	CHECK_EQ(c->x[3], 0xfefcfaf8f6f4f2ff);

	arm64_test_exec(m, c, 0xd200f024); /* EOR X4, X1, #0x5555555555555555 */
	CHECK_EQ(c->x[4], 0xab89efcd2301674a);

	arm64_test_exec(m, c, 0xb200f3e5); /* MOV X5, #0x5555555555555555, an ORR */
	CHECK_EQ(c->x[5], 0x5555555555555555);

	c->x[6] = UINT64_MAX;
	arm64_test_exec(m, c, 0x32009fe6); /* MOV W6, #0x00ff00ff, an ORR */
	CHECK_EQ(c->x[6], 0x00ff00ff);

	/* Rd 31 of AND is SP: aligning the stack pointer. */
	c->x[1] = 0x8007;
	arm64_test_exec(m, c, 0x927cec3f); /* AND SP, X1, #0xfffffffffffffff0 */
	CHECK_EQ(c->sp_el[3], 0x8000);
	machine_destroy(m);
}

static void test_logical_imm_flags(void)
{
	/* ANDS sets N and Z from the result and clears C and V. */
	static const struct {
		uint32_t insn;
		uint64_t x1;
		uint32_t nzcv;
	} cases[] = {
		{0xf2410020, UINT64_MAX, N},         /* ANDS X0, X1, #1 << 63 */
		{0x7200003f, 0x10, Z},               /* TST W1, #1 */
		{0x7200003f, 0xffffffff00000001, 0}, /* TST W1, #1 */
		{0x72181c20, 0xffffffff00000000, Z}, /* ANDS W0, W1, #0xff00 */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		c->x[1] = cases[i].x1;
		c->nzcv = N | Z | C | V;
		c->sp_el[3] = 0x8000;
		arm64_test_exec(m, c, cases[i].insn);
		CHECK_EQ(c->nzcv, cases[i].nzcv);
		CHECK_EQ(c->sp_el[3], 0x8000); /* TST writes the zero register */
		machine_destroy(m);
	}
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static void test_logical_imm_all_encodings(void)
{
	/*
	 * Run ORR <Rd>, ZR, #imm for every value of N:immr:imms. There are
	 * 5334 different 64-bit bitmask immediates and 1302 32-bit ones,
	 * and none of them is all zeros or all ones.
	 */
	static uint64_t values[1 << 13];
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	for (uint32_t sf = 0; sf < 2; sf++) {
		uint64_t all_ones = sf ? UINT64_MAX : UINT32_MAX;
		unsigned count = 0, distinct = 0;

		for (uint32_t fields = 0; fields < ARRAY_SIZE(values); fields++) {
			uint32_t insn = sf << 31 | 0x32000000 | fields << 10 | 31 << 5;

			if (arm64_execute(c, insn) != ARM64_EXEC_OK)
				continue;
			CHECK(c->x[0] != 0 && c->x[0] != all_ones);
			values[count++] = c->x[0];
		}
		qsort(values, count, sizeof(values[0]), compare_u64);
		for (unsigned i = 0; i < count; i++)
			distinct += i == 0 || values[i] != values[i - 1];
		CHECK_EQ(distinct, sf ? 5334 : 1302);
	}
	machine_destroy(m);
}

static void test_logical_imm_undefined(void)
{
	static const uint32_t insns[] = {
		0x12400000, /* 32-bit AND with N = 1 */
		0x9240fc00, /* N:imms = 1:111111, a 64-bit element of all ones */
		0x1200f400, /* N:imms = 0:111101, a 2-bit element of all ones */
		0x1200f800, /* N:NOT(imms) = 0000000 or 0000001: no element size */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

static void test_bitfield_aliases(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 0xfedcba9876543210;
	arm64_test_exec(m, c, 0xd37cec20); /* LSL X0, X1, #4 */
	CHECK_EQ(c->x[0], 0xedcba98765432100);
	arm64_test_exec(m, c, 0xd37cfc22); /* LSR X2, X1, #60 */
	CHECK_EQ(c->x[2], 0xf);
	arm64_test_exec(m, c, 0x9344fc23); /* ASR X3, X1, #4 */
	CHECK_EQ(c->x[3], 0xffedcba987654321);
	arm64_test_exec(m, c, 0xd3483c24); /* UBFX X4, X1, #8, #8 */
	CHECK_EQ(c->x[4], 0x32);
	arm64_test_exec(m, c, 0xd37c1c2c); /* UBFIZ X12, X1, #4, #8 */
	CHECK_EQ(c->x[12], 0x100);

	c->x[6] = 0x1111111111111111;
	arm64_test_exec(m, c, 0xb3701c26); /* BFI X6, X1, #16, #8 */
	CHECK_EQ(c->x[6], 0x1111111111101111);
	c->x[7] = 0x1111111111111111;
	arm64_test_exec(m, c, 0xb3483c27); /* BFXIL X7, X1, #8, #8 */
	CHECK_EQ(c->x[7], 0x1111111111111132);
	c->x[14] = UINT64_MAX;
	arm64_test_exec(m, c, 0xb3783fee); /* BFC X14, #8, #16 */
	CHECK_EQ(c->x[14], 0xffffffffff0000ff);

	c->x[8] = UINT64_MAX;
	arm64_test_exec(m, c, 0x53001c28); /* UXTB W8, W1 */
	CHECK_EQ(c->x[8], 0x10);

	/* The signed forms copy the top bit of the field upwards. */
	c->x[1] = 0x8000;
	arm64_test_exec(m, c, 0x93483c25); /* SBFX X5, X1, #8, #8 */
	CHECK_EQ(c->x[5], 0xffffffffffffff80);
	c->x[1] = 0x80;
	arm64_test_exec(m, c, 0x937c1c2d); /* SBFIZ X13, X1, #4, #8 */
	CHECK_EQ(c->x[13], 0xfffffffffffff800);
	c->x[1] = 0x12348765;
	arm64_test_exec(m, c, 0x93403c29); /* SXTH X9, W1 */
	CHECK_EQ(c->x[9], 0xffffffffffff8765);
	c->x[1] = 0x80000000;
	arm64_test_exec(m, c, 0x93407c2a); /* SXTW X10, W1 */
	CHECK_EQ(c->x[10], 0xffffffff80000000);

	c->x[1] = 3;
	arm64_test_exec(m, c, 0x5301002b); /* LSL W11, W1, #31 */
	CHECK_EQ(c->x[11], 0x80000000);
	machine_destroy(m);
}

/*
 * What the descriptions of the SBFM, BFM and UBFM aliases say, one bit at
 * a time. When imms >= immr, bits imms to immr of src go to the bottom
 * of the result (SBFX, BFXIL, UBFX). Otherwise bits imms to 0 go to bit
 * datasize - immr (SBFIZ, BFI, UBFIZ). SBFM fills the bits above the
 * field with its top bit, BFM keeps the other bits of dst and UBFM
 * clears them.
 */
static uint64_t bitfield_reference(unsigned opc, unsigned datasize,
                                   unsigned immr, unsigned imms, uint64_t src,
                                   uint64_t dst)
{
	unsigned from = imms >= immr ? immr : 0;
	unsigned to = imms >= immr ? 0 : datasize - immr;
	unsigned width = imms >= immr ? imms - immr + 1 : imms + 1;
	uint64_t result = opc == 1 ? dst : 0;

	for (unsigned i = 0; i < width; i++) {
		uint64_t bit = (src >> (from + i)) & 1;

		result &= ~((uint64_t)1 << (to + i));
		result |= bit << (to + i);
	}
	if (opc == 0) {
		uint64_t sign = (src >> (from + width - 1)) & 1;

		for (unsigned i = to + width; i < datasize; i++)
			result |= sign << i;
	}
	return datasize == 64 ? result : result & UINT32_MAX;
}

static void test_bitfield_all_encodings(void)
{
	static const uint64_t sources[] = {0xfedcba9876543210, 0x0123456789abcdef};
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);
	unsigned mismatches = 0;

	for (uint32_t sf = 0; sf < 2; sf++) {
		unsigned datasize = 32 << sf;

		for (uint32_t opc = 0; opc < 3; opc++) {
			for (uint32_t immr = 0; immr < datasize; immr++) {
				for (uint32_t imms = 0; imms < datasize; imms++) {
					/* <op> X0, X1, #immr, #imms, with N = sf */
					uint32_t insn = sf << 31 | opc << 29 | 0x13000000 |
					                sf << 22 | immr << 16 | imms << 10 | 1 << 5;

					for (unsigned i = 0; i < ARRAY_SIZE(sources); i++) {
						uint64_t dst = 0x5aa55aa55aa55aa5;
						uint64_t expected = bitfield_reference(
							opc, datasize, immr, imms, sources[i], dst);

						c->x[0] = dst;
						c->x[1] = sources[i];
						if (arm64_execute(c, insn) != ARM64_EXEC_OK ||
						    c->x[0] != expected) {
							if (mismatches++ == 0)
								fprintf(stderr,
								        "first mismatch: 0x%08" PRIx32 "\n",
								        insn);
						}
					}
				}
			}
		}
	}
	CHECK_EQ(mismatches, 0);
	machine_destroy(m);
}

static void test_bitfield_undefined(void)
{
	static const uint32_t insns[] = {
		0x13400000, /* 32-bit SBFM with N = 1 */
		0x93000000, /* 64-bit SBFM with N = 0 */
		0x13200000, /* 32-bit SBFM with immr = 32 */
		0x13008000, /* 32-bit SBFM with imms = 32 */
		0x73000000, /* opc = 11 */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

static void test_extract(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 0xfedcba9876543210;
	c->x[2] = 0x0123456789abcdef;
	arm64_test_exec(m, c, 0x93c22020); /* EXTR X0, X1, X2, #8 */
	CHECK_EQ(c->x[0], 0x100123456789abcd);
	arm64_test_exec(m, c, 0x93c11023); /* ROR X3, X1, #4 */
	CHECK_EQ(c->x[3], 0x0fedcba987654321);
	arm64_test_exec(m, c, 0x13824024); /* EXTR W4, W1, W2, #16 */
	CHECK_EQ(c->x[4], 0x321089ab);
	arm64_test_exec(m, c, 0x93c20025); /* EXTR X5, X1, X2, #0 */
	CHECK_EQ(c->x[5], 0x0123456789abcdef);
	machine_destroy(m);
}

static void test_extract_undefined(void)
{
	static const uint32_t insns[] = {
		0x13808000, /* 32-bit EXTR with imms = 32 */
		0x93808000, /* 64-bit EXTR with N = 0 */
		0x13a00000, /* o0 = 1 */
		0x33800000, /* op21 = 01 */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

static void test_classes_newer_than_armv8_0_are_undefined(void)
{
	/*
	 * The encoding of AUTIASPPC is built from the table in C4.1:
	 * binutils 2.46 does not know FEAT_PAuth_LR.
	 */
	static const uint32_t insns[] = {
		0x91800000, /* ADDG X0, X0, #0, #0 (FEAT_MTE) */
		0x91c00000, /* SMAX X0, X0, #0 (FEAT_CSSC) */
		0xf380001f, /* AUTIASPPC (FEAT_PAuth_LR) */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

int main(void)
{
	RUN_TEST(test_add_with_carry_32_bit);
	RUN_TEST(test_add_with_carry_64_bit);
	RUN_TEST(test_movz);
	RUN_TEST(test_movn);
	RUN_TEST(test_movk);
	RUN_TEST(test_move_wide_unallocated);
	RUN_TEST(test_add_sub_imm);
	RUN_TEST(test_stack_pointer_operands);
	RUN_TEST(test_flag_setting);
	RUN_TEST(test_adr);
	RUN_TEST(test_adrp);
	RUN_TEST(test_logical_imm);
	RUN_TEST(test_logical_imm_flags);
	RUN_TEST(test_logical_imm_all_encodings);
	RUN_TEST(test_logical_imm_undefined);
	RUN_TEST(test_bitfield_aliases);
	RUN_TEST(test_bitfield_all_encodings);
	RUN_TEST(test_bitfield_undefined);
	RUN_TEST(test_extract);
	RUN_TEST(test_extract_undefined);
	RUN_TEST(test_classes_newer_than_armv8_0_are_undefined);
	return test_summary();
}
