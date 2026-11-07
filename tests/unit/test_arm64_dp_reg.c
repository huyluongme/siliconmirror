/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the Data Processing -- Register instructions of the
 * AArch64 model (src/cpu/arm64/dp_reg.c). Every encoding was produced
 * by the GNU assembler, unless a comment says otherwise. Section numbers
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

static const uint64_t patterns[] = {
	0xfedcba9876543210,
	0x0123456789abcdef,
	0x8000000000000001,
};

/* Bit n of x, 0 or 1. */
static uint64_t bit(uint64_t x, unsigned n)
{
	return (x >> n) & 1;
}

/*
 * What LSL, LSR, ASR and ROR (type 0 to 3) do, one bit at a time: the
 * source bit that each result bit comes from.
 */
static uint64_t shift_reference(uint64_t value, unsigned type, unsigned amount,
                                unsigned datasize)
{
	uint64_t result = 0;

	for (unsigned i = 0; i < datasize; i++) {
		unsigned from = i + amount;
		uint64_t b;

		switch (type) {
		case 0: /* LSL: zeros come in at the bottom */
			b = i >= amount ? bit(value, i - amount) : 0;
			break;
		case 1: /* LSR: zeros come in at the top */
			b = from < datasize ? bit(value, from) : 0;
			break;
		case 2: /* ASR: copies of the sign bit come in at the top */
			b = bit(value, from < datasize ? from : datasize - 1);
			break;
		default: /* ROR: bits leaving the bottom come in at the top */
			b = bit(value, from % datasize);
			break;
		}
		result |= b << i;
	}
	return result;
}

static void test_shifts_all_amounts(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);
	unsigned mismatches = 0;

	for (uint32_t sf = 0; sf < 2; sf++) {
		unsigned datasize = 32 << sf;

		for (uint32_t type = 0; type < 4; type++) {
			for (uint32_t amount = 0; amount < datasize; amount++) {
				/* ORR X0, XZR, X2, <type> #amount, and ADD for no ROR */
				uint32_t orr = sf << 31 | 0x2a000000 | type << 22 | 2 << 16 |
				               amount << 10 | 31 << 5;
				uint32_t add = sf << 31 | 0x0b000000 | type << 22 | 2 << 16 |
				               amount << 10 | 31 << 5;

				for (unsigned i = 0; i < ARRAY_SIZE(patterns); i++) {
					uint64_t expected =
						shift_reference(patterns[i] & datasize_mask(datasize),
					                    type, amount, datasize);

					c->x[2] = patterns[i];
					if (arm64_execute(c, orr) != ARM64_EXEC_OK ||
					    c->x[0] != expected)
						mismatches++;
					if (type == 3)
						continue;
					c->x[0] = 0;
					if (arm64_execute(c, add) != ARM64_EXEC_OK ||
					    c->x[0] != expected)
						mismatches++;
				}
			}
		}
	}
	CHECK_EQ(mismatches, 0);
	machine_destroy(m);
}

/*
 * What UXTB, UXTH, UXTW, UXTX, SXTB, SXTH, SXTW and SXTX (option 0 to 7)
 * followed by LSL #shift do, one bit at a time.
 */
static uint64_t extend_reference(uint64_t value, unsigned option,
                                 unsigned shift, unsigned datasize)
{
	unsigned len = 8u << (option & 3);
	bool is_signed = option & 4;
	uint64_t result = 0;

	for (unsigned i = 0; i + shift < datasize; i++) {
		uint64_t b;

		if (i < len)
			b = bit(value, i);
		else
			b = is_signed ? bit(value, len - 1) : 0;
		result |= b << (i + shift);
	}
	return result;
}

static void test_extends_all_options(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);
	unsigned mismatches = 0;

	for (uint32_t sf = 0; sf < 2; sf++) {
		unsigned datasize = 32 << sf;

		for (uint32_t option = 0; option < 8; option++) {
			for (uint32_t shift = 0; shift <= 4; shift++) {
				/* ADD X0, X1, X2, <option> #shift with X1 = 0 */
				uint32_t insn = sf << 31 | 0x0b200000 | 2 << 16 | option << 13 |
				                shift << 10 | 1 << 5;

				for (unsigned i = 0; i < ARRAY_SIZE(patterns); i++) {
					c->x[1] = 0;
					c->x[2] = patterns[i];
					if (arm64_execute(c, insn) != ARM64_EXEC_OK ||
					    c->x[0] != extend_reference(patterns[i], option, shift,
					                                datasize))
						mismatches++;
				}
			}
		}
	}
	CHECK_EQ(mismatches, 0);
	machine_destroy(m);
}

static void test_add_sub_shifted(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 0x1000;
	c->x[2] = 0x10;
	arm64_test_exec(m, c, 0x8b020020); /* ADD X0, X1, X2 */
	CHECK_EQ(c->x[0], 0x1010);
	arm64_test_exec(m, c, 0x8b021023); /* ADD X3, X1, X2, LSL #4 */
	CHECK_EQ(c->x[3], 0x1100);

	c->x[2] = 0xffffffff80000000;
	c->x[4] = UINT64_MAX;
	arm64_test_exec(m, c, 0x0b427c24); /* ADD W4, W1, W2, LSR #31 */
	CHECK_EQ(c->x[4], 0x1001);
	arm64_test_exec(m, c, 0xcb82fc25); /* SUB X5, X1, X2, ASR #63 */
	CHECK_EQ(c->x[5], 0x1001);
	arm64_test_exec(m, c, 0xcb0203e6); /* NEG X6, X2 */
	CHECK_EQ(c->x[6], 0x80000000);

	/* Rd 31 is the zero register: SP does not change. */
	c->sp_el[3] = 0x8000;
	arm64_test_exec(m, c, 0xeb02003f); /* CMP X1, X2 */
	CHECK_EQ(c->sp_el[3], 0x8000);
	machine_destroy(m);
}

static void test_logical_shifted(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 0xff00ff00ff00ff00;
	c->x[2] = 0x0ff00ff00ff00ff0;
	arm64_test_exec(m, c, 0xaa0103e7); /* MOV X7, X1 */
	CHECK_EQ(c->x[7], 0xff00ff00ff00ff00);
	c->x[8] = UINT64_MAX;
	arm64_test_exec(m, c, 0x2a0103e8); /* MOV W8, W1 */
	CHECK_EQ(c->x[8], 0xff00ff00);
	arm64_test_exec(m, c, 0xaa2103e9); /* MVN X9, X1 */
	CHECK_EQ(c->x[9], 0x00ff00ff00ff00ff);
	arm64_test_exec(m, c, 0x8a02002a); /* AND X10, X1, X2 */
	CHECK_EQ(c->x[10], 0x0f000f000f000f00);
	arm64_test_exec(m, c, 0x8a22002b); /* BIC X11, X1, X2 */
	CHECK_EQ(c->x[11], 0xf000f000f000f000);
	arm64_test_exec(m, c, 0xaac2202c); /* ORR X12, X1, X2, ROR #8 */
	CHECK_EQ(c->x[12], 0xff0fff0fff0fff0f);
	arm64_test_exec(m, c, 0x2a22002d); /* ORN W13, W1, W2 */
	CHECK_EQ(c->x[13], 0xff0fff0f);
	arm64_test_exec(m, c, 0xca02042e); /* EOR X14, X1, X2, LSL #1 */
	CHECK_EQ(c->x[14], 0xe0e0e0e0e0e0e0e0);
	arm64_test_exec(m, c, 0xca22002f); /* EON X15, X1, X2 */
	CHECK_EQ(c->x[15], 0x0f0f0f0f0f0f0f0f);
	machine_destroy(m);
}

static void test_flag_setting(void)
{
	static const struct {
		uint32_t insn;
		uint64_t x1, x2;
		uint32_t nzcv;
	} cases[] = {
		{0xeb02003f, 5, 5, Z | C},               /* CMP X1, X2 */
		{0xeb02003f, 5, 6, N},                   /* CMP X1, X2 */
		{0x2b02003f, 0x7fffffff, 1, N | V},      /* CMN W1, W2 */
		{0xab020020, UINT64_MAX, 1, Z | C},      /* ADDS X0, X1, X2 */
		{0x6b020020, 0x80000000, 1, C | V},      /* SUBS W0, W1, W2 */
		{0xeb0203e0, 0, 0, Z | C},               /* NEGS X0, X2 */
		{0xea02003f, 0xf0, 0x0f, Z},             /* TST X1, X2 */
		{0x6a020020, 0x80000000, 0xffffffff, N}, /* ANDS W0, W1, W2 */
		{0xea220020, 0x8000000000000000, 0, N},  /* BICS X0, X1, X2 */
		{0xeb2263ff, 0, 0, Z | C},               /* CMP SP, X2 */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		c->x[1] = cases[i].x1;
		c->x[2] = cases[i].x2;
		c->nzcv = N | Z | C | V;
		arm64_test_exec(m, c, cases[i].insn);
		CHECK_EQ(c->nzcv, cases[i].nzcv);
		machine_destroy(m);
	}
}

static void test_add_sub_extended(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 0x1000;
	c->x[2] = 0x1ff;
	arm64_test_exec(m, c, 0x8b220020); /* ADD X0, X1, W2, UXTB */
	CHECK_EQ(c->x[0], 0x10ff);
	c->x[2] = 0x80;
	arm64_test_exec(m, c, 0x8b228820); /* ADD X0, X1, W2, SXTB #2 */
	CHECK_EQ(c->x[0], 0x1000 - 0x200);
	c->x[2] = 0xffff8000;
	arm64_test_exec(m, c, 0x8b22a020); /* ADD X0, X1, W2, SXTH */
	CHECK_EQ(c->x[0], 0x1000 - 0x8000);
	c->x[2] = 0x1234567800000001;
	arm64_test_exec(m, c, 0x8b224c20); /* ADD X0, X1, W2, UXTW #3 */
	CHECK_EQ(c->x[0], 0x1008);
	c->x[2] = 0xffffffff;
	arm64_test_exec(m, c, 0x8b22c020); /* ADD X0, X1, W2, SXTW */
	CHECK_EQ(c->x[0], 0xfff);
	c->x[2] = 1;
	arm64_test_exec(m, c, 0x8b22f020); /* ADD X0, X1, X2, SXTX #4 */
	CHECK_EQ(c->x[0], 0x1010);
	c->x[2] = 0x12345;
	arm64_test_exec(m, c, 0x0b222020); /* ADD W0, W1, W2, UXTH */
	CHECK_EQ(c->x[0], 0x3345);
	machine_destroy(m);
}

static void test_extended_with_stack_pointer(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* Here Rn 31 is SP, and Rd 31 is SP for ADD and SUB. */
	c->sp_el[3] = 0x8000;
	c->x[2] = 0x100;
	arm64_test_exec(m, c, 0x8b2263ff); /* ADD SP, SP, X2 */
	CHECK_EQ(c->sp_el[3], 0x8100);
	arm64_test_exec(m, c, 0xcb2273ff); /* SUB SP, SP, X2, LSL #4 */
	CHECK_EQ(c->sp_el[3], 0x7100);
	arm64_test_exec(m, c, 0x8b2263e0); /* ADD X0, SP, X2 */
	CHECK_EQ(c->x[0], 0x7200);

	/* SUBS writes the zero register, not SP. */
	c->x[2] = 0xffffffff00000100;
	arm64_test_exec(m, c, 0xeb2243e0); /* SUBS X0, SP, W2, UXTW */
	CHECK_EQ(c->x[0], 0x7000);
	CHECK_EQ(c->nzcv, C);
	arm64_test_exec(m, c, 0xeb2263ff); /* CMP SP, X2 */
	CHECK_EQ(c->sp_el[3], 0x7100);
	machine_destroy(m);
}

static void test_conditional_compare(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* The condition holds: the flags of the comparison. */
	c->x[1] = 5;
	c->x[2] = 5;
	c->nzcv = Z;
	arm64_test_exec(m, c, 0xfa420020); /* CCMP X1, X2, #0, EQ */
	CHECK_EQ(c->nzcv, Z | C);
	/* It does not: the nzcv field. */
	c->nzcv = 0;
	arm64_test_exec(m, c, 0xfa420020); /* CCMP X1, X2, #0, EQ */
	CHECK_EQ(c->nzcv, 0);

	c->nzcv = 0;
	arm64_test_exec(m, c, 0xfa451828); /* CCMP X1, #5, #8, NE */
	CHECK_EQ(c->nzcv, Z | C);
	c->nzcv = Z;
	arm64_test_exec(m, c, 0xfa451828); /* CCMP X1, #5, #8, NE */
	CHECK_EQ(c->nzcv, N);

	c->x[1] = 0x7fffffff;
	c->x[2] = 1;
	c->nzcv = N;
	arm64_test_exec(m, c, 0x3a42b024); /* CCMN W1, W2, #4, LT */
	CHECK_EQ(c->nzcv, N | V);
	c->nzcv = 0;
	arm64_test_exec(m, c, 0x3a42b024); /* CCMN W1, W2, #4, LT */
	CHECK_EQ(c->nzcv, Z);

	c->x[1] = (uint64_t)-31;
	arm64_test_exec(m, c, 0xba5fe820); /* CCMN X1, #31, #0, AL */
	CHECK_EQ(c->nzcv, Z | C);
	machine_destroy(m);
}

static void test_conditional_select(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 10;
	c->x[2] = 20;
	c->nzcv = Z;
	arm64_test_exec(m, c, 0x9a820020); /* CSEL X0, X1, X2, EQ */
	CHECK_EQ(c->x[0], 10);
	arm64_test_exec(m, c, 0x9a821420); /* CSINC X0, X1, X2, NE */
	CHECK_EQ(c->x[0], 21);
	arm64_test_exec(m, c, 0xda82b420); /* CSNEG X0, X1, X2, LT */
	CHECK_EQ(c->x[0], (uint64_t)-20);
	c->nzcv = 0;
	arm64_test_exec(m, c, 0x9a820020); /* CSEL X0, X1, X2, EQ */
	CHECK_EQ(c->x[0], 20);
	arm64_test_exec(m, c, 0x5a82a020); /* CSINV W0, W1, W2, GE */
	CHECK_EQ(c->x[0], 10);
	c->nzcv = N;
	arm64_test_exec(m, c, 0x5a82a020); /* CSINV W0, W1, W2, GE */
	CHECK_EQ(c->x[0], 0xffffffeb);

	/* The aliases. */
	c->nzcv = Z;
	arm64_test_exec(m, c, 0x9a9f17e0); /* CSET X0, EQ */
	CHECK_EQ(c->x[0], 1);
	c->x[0] = UINT64_MAX;
	arm64_test_exec(m, c, 0x5a9f03e0); /* CSETM W0, NE */
	CHECK_EQ(c->x[0], 0);
	c->nzcv = 0;
	arm64_test_exec(m, c, 0x5a9f03e0); /* CSETM W0, NE */
	CHECK_EQ(c->x[0], 0xffffffff);
	c->nzcv = C;
	arm64_test_exec(m, c, 0x9a819420); /* CINC X0, X1, HI */
	CHECK_EQ(c->x[0], 11);
	c->nzcv = N;
	arm64_test_exec(m, c, 0xda815420); /* CNEG X0, X1, MI */
	CHECK_EQ(c->x[0], (uint64_t)-10);
	machine_destroy(m);
}

static void test_undefined(void)
{
	/*
	 * UNALLOCATED encodings, built from the tables in C4.1 (binutils
	 * agrees they are invalid), then instructions newer than Armv8.0.
	 */
	static const uint32_t insns[] = {
		0x8bc20020, /* ADD (shifted register) with ROR */
		0x0b02fc20, /* 32-bit ADD with a shift of 63 */
		0x0a02fc20, /* 32-bit AND with a shift of 63 */
		0x8b221420, /* ADD (extended register) with a shift of 5 */
		0x8b621020, /* ADD (extended register) with opt = 01 */
		0x9a420000, /* CCMN with S = 0 */
		0xfa400401, /* CCMP with o2 = 1 */
		0xfa400810, /* CCMP with o3 = 1 */
		0x9a820c20, /* CSEL with op2 = 11 */
		0xba820020, /* CSEL with S = 1 */
		0x9a200000, /* op1 = 1, op2 = 0001: UNALLOCATED */
		0xba018402, /* RMIF X0, #3, #2 (FEAT_FlagM) */
		0x3a00080d, /* SETF8 W0 (FEAT_FlagM) */
		0x9a022020, /* ADDPT X0, X1, X2 (FEAT_CPA) */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

static void test_not_implemented_yet(void)
{
	static const uint32_t insns[] = {
		0x9a020020, /* ADC X0, X1, X2 */
		0x9ac20820, /* UDIV X0, X1, X2 */
		0xdac00020, /* RBIT X0, X1 */
		0x9b020c20, /* MADD X0, X1, X2, X3 */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(insns); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		CHECK_EQ(arm64_test_step(m, c, insns[i]), MACHINE_STOP_ERROR);
		CHECK(strstr(m->stop_message, "is not implemented yet (Data "
		                              "Processing -- Register)") != NULL);
		machine_destroy(m);
	}
}

int main(void)
{
	RUN_TEST(test_shifts_all_amounts);
	RUN_TEST(test_extends_all_options);
	RUN_TEST(test_add_sub_shifted);
	RUN_TEST(test_logical_shifted);
	RUN_TEST(test_flag_setting);
	RUN_TEST(test_add_sub_extended);
	RUN_TEST(test_extended_with_stack_pointer);
	RUN_TEST(test_conditional_compare);
	RUN_TEST(test_conditional_select);
	RUN_TEST(test_undefined);
	RUN_TEST(test_not_implemented_yet);
	return test_summary();
}
