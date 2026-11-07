/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the Data Processing -- Immediate instructions of the
 * AArch64 model (src/cpu/arm64/dp_imm.c). Every encoding was produced by
 * the GNU assembler, unless its comment says otherwise. Section numbers
 * refer to the Arm Architecture Reference Manual, ARM DDI 0487 M.d.
 */

#include <stdint.h>
#include <string.h>

#include "arm64_test.h"
#include "cpu/arm64/internal.h"
#include "util/macros.h"

#define N ARM64_NZCV_N
#define Z ARM64_NZCV_Z
#define C ARM64_NZCV_C
#define V ARM64_NZCV_V

/* Run one instruction that must execute. */
static void exec(Machine *m, Arm64Cpu *c, uint32_t insn)
{
	CHECK_EQ(arm64_test_step(m, c, insn), MACHINE_STOP_LIMIT);
}

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
	exec(m, c, 0xd2e24680); /* MOVZ X0, #0x1234, LSL #48 */
	CHECK_EQ(c->x[0], 0x1234000000000000);

	c->x[1] = UINT64_MAX;
	exec(m, c, 0x52b579a1); /* MOVZ W1, #0xabcd, LSL #16 */
	CHECK_EQ(c->x[1], 0xabcd0000);

	/* Rd 31 is the zero register: SP does not change. */
	c->sp_el[3] = 0x8000;
	exec(m, c, 0xd280003f); /* MOVZ XZR, #1 */
	CHECK_EQ(c->sp_el[3], 0x8000);
	machine_destroy(m);
}

static void test_movn(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	exec(m, c, 0x92800002); /* MOVN X2, #0 */
	CHECK_EQ(c->x[2], UINT64_MAX);

	c->x[3] = UINT64_MAX;
	exec(m, c, 0x12800003); /* MOVN W3, #0 */
	CHECK_EQ(c->x[3], 0xffffffff);

	exec(m, c, 0x92a24684); /* MOVN X4, #0x1234, LSL #16 */
	CHECK_EQ(c->x[4], 0xffffffffedcbffff);

	exec(m, c, 0x92800027); /* MOV X7, #-2, the alias of MOVN X7, #1 */
	CHECK_EQ(c->x[7], 0xfffffffffffffffe);
	machine_destroy(m);
}

static void test_movk(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[5] = 0x1111222233334444;
	exec(m, c, 0xf2d7dde5); /* MOVK X5, #0xbeef, LSL #32 */
	CHECK_EQ(c->x[5], 0x1111beef33334444);

	/* A 32-bit MOVK keeps bits 31:16 and clears bits 63:32. */
	c->x[6] = 0xffffffff12345678;
	exec(m, c, 0x7297dde6); /* MOVK W6, #0xbeef */
	CHECK_EQ(c->x[6], 0x1234beef);
	machine_destroy(m);
}

static void test_move_wide_unallocated(void)
{
	static const uint32_t insns[] = {
		0x52c00000, /* MOVZ W0 with hw = 10: no bit 32 in a W register */
		0x32800000, /* opc = 01 */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(insns); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		CHECK_EQ(arm64_test_step(m, c, insns[i]), MACHINE_STOP_ERROR);
		CHECK(strstr(m->stop_message, "undefined instruction") != NULL);
		machine_destroy(m);
	}
}

static void test_add_sub_imm(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* ADD and SUB leave the flags alone. */
	c->nzcv = N | C;
	c->x[1] = 0x1000;
	exec(m, c, 0x91048c20); /* ADD X0, X1, #0x123 */
	CHECK_EQ(c->x[0], 0x1123);
	exec(m, c, 0x91400422); /* ADD X2, X1, #1, LSL #12 */
	CHECK_EQ(c->x[2], 0x2000);

	/* A 32-bit result wraps around and clears bits 63:32. */
	c->x[1] = 0xffffffff;
	c->x[3] = UINT64_MAX;
	exec(m, c, 0x11000423); /* ADD W3, W1, #1 */
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
	exec(m, c, 0x9100003f); /* MOV SP, X1 */
	CHECK_EQ(c->sp_el[3], 0x8000);

	exec(m, c, 0xd10043ff); /* SUB SP, SP, #0x10 */
	CHECK_EQ(c->sp_el[3], 0x7ff0);

	exec(m, c, 0x910003e7); /* MOV X7, SP */
	CHECK_EQ(c->x[7], 0x7ff0);

	/* C6.2.5: a 32-bit result written to SP is zero-extended. */
	c->x[1] = 0xffffffff00001234;
	exec(m, c, 0x1100003f); /* MOV WSP, W1 */
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
		exec(m, c, cases[i].insn);
		CHECK_EQ(c->nzcv, cases[i].nzcv);
		machine_destroy(m);
	}

	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = UINT64_MAX;
	exec(m, c, 0xb1000420); /* ADDS X0, X1, #1 */
	CHECK_EQ(c->x[0], 0);

	c->x[0] = UINT64_MAX;
	c->x[1] = 0x80000000;
	exec(m, c, 0x71000420); /* SUBS W0, W1, #1 */
	CHECK_EQ(c->x[0], 0x7fffffff);

	/* Rn 31 is SP, but Rd 31 of a flag-setting form is the zero register. */
	c->sp_el[3] = 0x8000;
	exec(m, c, 0xf10003ff); /* CMP SP, #0 */
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
	exec(m, c, 0x10000800); /* ADR X0, . + 0x100 */
	CHECK_EQ(c->x[0], pc + 0x100);

	pc = c->pc;
	exec(m, c, 0x10ffffe1); /* ADR X1, . - 4 */
	CHECK_EQ(c->x[1], pc - 4);

	/* The limits of the 21-bit signed offset. */
	pc = c->pc;
	exec(m, c, 0x10800002); /* ADR X2, . - 0x100000 */
	CHECK_EQ(c->x[2], pc - 0x100000);

	pc = c->pc;
	exec(m, c, 0x707fffe3); /* ADR X3, . + 0xfffff */
	CHECK_EQ(c->x[3], pc + 0xfffff);
	machine_destroy(m);
}

static void test_adrp(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* Start near the end of a page so that the page offset matters. */
	c->pc = 0x80ff4;
	exec(m, c, 0xb0000000); /* ADRP X0, . + 1 page */
	CHECK_EQ(c->x[0], 0x81000);

	exec(m, c, 0xf0ffffe1); /* ADRP X1, . - 1 page, at 0x80ff8 */
	CHECK_EQ(c->x[1], 0x7f000);

	exec(m, c, 0x90800002); /* ADRP X2, . - 4GB, at 0x80ffc */
	CHECK_EQ(c->x[2], 0xffffffff00080000);
	machine_destroy(m);
}

static void test_classes_newer_than_armv8_0_are_undefined(void)
{
	static const uint32_t insns[] = {
		0x91800000, /* ADDG X0, X0, #0, #0 (FEAT_MTE) */
		0x91c00000, /* SMAX X0, X0, #0 (FEAT_CSSC) */
		/*
	     * AUTIASPPC (FEAT_PAuth_LR), built from the table in C4.1:
	     * binutils 2.46 does not know it.
	     */
		0xf380001f,
	};

	for (unsigned i = 0; i < ARRAY_SIZE(insns); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		CHECK_EQ(arm64_test_step(m, c, insns[i]), MACHINE_STOP_ERROR);
		CHECK(strstr(m->stop_message, "undefined instruction") != NULL);
		machine_destroy(m);
	}
}

static void test_classes_not_implemented_yet(void)
{
	static const uint32_t insns[] = {
		0x92400000, /* AND X0, X0, #1: Logical (immediate) */
		0x93400000, /* SBFX X0, X0, #0, #1: Bitfield */
		0x13800000, /* ROR W0, W0, #0, the alias of EXTR: Extract */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(insns); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		CHECK_EQ(arm64_test_step(m, c, insns[i]), MACHINE_STOP_ERROR);
		CHECK(strstr(m->stop_message, "is not implemented yet (Data "
		                              "Processing -- Immediate)") != NULL);
		machine_destroy(m);
	}
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
	RUN_TEST(test_classes_newer_than_armv8_0_are_undefined);
	RUN_TEST(test_classes_not_implemented_yet);
	return test_summary();
}
