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

static void test_add_sub_with_carry(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 5;
	c->x[2] = 7;
	c->nzcv = C;
	arm64_test_exec(m, c, 0x9a020020); /* ADC X0, X1, X2 */
	CHECK_EQ(c->x[0], 13);
	CHECK_EQ(c->nzcv, C); /* ADC leaves the flags alone */
	c->nzcv = 0;
	arm64_test_exec(m, c, 0x9a020020); /* ADC X0, X1, X2 */
	CHECK_EQ(c->x[0], 12);

	/* SBC subtracts one more when C is clear: C means "no borrow". */
	c->x[1] = 10;
	c->x[2] = 3;
	c->nzcv = C;
	arm64_test_exec(m, c, 0xda020020); /* SBC X0, X1, X2 */
	CHECK_EQ(c->x[0], 7);
	c->nzcv = 0;
	arm64_test_exec(m, c, 0xda020020); /* SBC X0, X1, X2 */
	CHECK_EQ(c->x[0], 6);

	c->x[2] = 5;
	c->nzcv = C;
	arm64_test_exec(m, c, 0xda0203e0); /* NGC X0, X2 */
	CHECK_EQ(c->x[0], (uint64_t)-5);
	c->nzcv = 0;
	arm64_test_exec(m, c, 0xda0203e0); /* NGC X0, X2 */
	CHECK_EQ(c->x[0], (uint64_t)-6);

	/* The flag-setting forms. */
	c->x[1] = 0xffffffff;
	c->x[2] = 0;
	c->nzcv = C;
	arm64_test_exec(m, c, 0x3a020020); /* ADCS W0, W1, W2 */
	CHECK_EQ(c->x[0], 0);
	CHECK_EQ(c->nzcv, Z | C);
	c->x[1] = 0;
	c->nzcv = 0;
	arm64_test_exec(m, c, 0xfa020020); /* SBCS X0, X1, X2 */
	CHECK_EQ(c->x[0], UINT64_MAX);
	CHECK_EQ(c->nzcv, N);
	machine_destroy(m);
}

static void test_divide(void)
{
	static const struct {
		uint32_t insn;
		uint64_t x1, x2, result;
	} cases[] = {
		{0x9ac20820, 100, 7, 14},                    /* UDIV X0, X1, X2 */
		{0x9ac20820, 100, 0, 0},                     /* UDIV X0, X1, X2 */
		{0x1ac20820, 0x1ffffffff, 2, 0x7fffffff},    /* UDIV W0, W1, W2 */
		{0x9ac20c20, (uint64_t)-7, 2, (uint64_t)-3}, /* SDIV X0, X1, X2 */
		{0x9ac20c20, 7, (uint64_t)-2, (uint64_t)-3}, /* SDIV X0, X1, X2 */
		{0x9ac20c20, (uint64_t)-7, (uint64_t)-2, 3}, /* SDIV X0, X1, X2 */
		{0x9ac20c20, 7, 0, 0},                       /* SDIV X0, X1, X2 */
		/* The most negative value divided by -1 overflows to itself. */
		{0x9ac20c20, 0x8000000000000000, UINT64_MAX, 0x8000000000000000},
		{0x1ac20c20, 0x80000000, 0xffffffff, 0x80000000}, /* SDIV W0 */
		{0x1ac20c20, 0xfffffff9, 2, 0xfffffffd},          /* SDIV W0 */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		c->x[1] = cases[i].x1;
		c->x[2] = cases[i].x2;
		arm64_test_exec(m, c, cases[i].insn);
		CHECK_EQ(c->x[0], cases[i].result);
		machine_destroy(m);
	}
}

static void test_shift_by_register(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* The amount is taken modulo the register size. */
	c->x[1] = 0x0123456789abcdef;
	c->x[2] = 68;
	arm64_test_exec(m, c, 0x9ac22020); /* LSL X0, X1, X2 */
	CHECK_EQ(c->x[0], 0x123456789abcdef0);
	c->x[2] = 33;
	arm64_test_exec(m, c, 0x1ac22420); /* LSR W0, W1, W2 */
	CHECK_EQ(c->x[0], 0x44d5e6f7);
	c->x[1] = 0x8000000000000000;
	c->x[2] = 63;
	arm64_test_exec(m, c, 0x9ac22820); /* ASR X0, X1, X2 */
	CHECK_EQ(c->x[0], UINT64_MAX);
	c->x[1] = 0x12345678;
	c->x[2] = 36;
	arm64_test_exec(m, c, 0x1ac22c20); /* ROR W0, W1, W2 */
	CHECK_EQ(c->x[0], 0x81234567);
	machine_destroy(m);
}

/* Run a CRC32 instruction with W0 as the accumulator. */
static void crc(Machine *m, Arm64Cpu *c, uint32_t insn, uint64_t value)
{
	c->x[1] = value;
	arm64_test_exec(m, c, insn);
}

static void test_crc32(void)
{
	/*
	 * The standard check values of CRC-32 and CRC-32C for "123456789":
	 * start from all ones, add each byte, invert at the end.
	 */
	static const char data[] = "123456789";
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[0] = UINT32_MAX;
	for (unsigned i = 0; i < 9; i++)
		crc(m, c, 0x1ac14000, (uint8_t)data[i]); /* CRC32B W0, W0, W1 */
	CHECK_EQ(c->x[0] ^ UINT32_MAX, 0xcbf43926);

	c->x[0] = UINT32_MAX;
	for (unsigned i = 0; i < 9; i++)
		crc(m, c, 0x1ac15000, (uint8_t)data[i]); /* CRC32CB W0, W0, W1 */
	CHECK_EQ(c->x[0] ^ UINT32_MAX, 0xe3069283);

	/* Eight bytes at once, as a little-endian doubleword. */
	c->x[0] = UINT32_MAX;
	crc(m, c, 0x9ac14c00, 0x3837363534333231); /* CRC32X W0, W0, X1 */
	crc(m, c, 0x1ac14000, '9');                /* CRC32B W0, W0, W1 */
	CHECK_EQ(c->x[0] ^ UINT32_MAX, 0xcbf43926);

	c->x[0] = UINT32_MAX;
	crc(m, c, 0x9ac15c00, 0x3837363534333231); /* CRC32CX W0, W0, X1 */
	crc(m, c, 0x1ac15000, '9');                /* CRC32CB W0, W0, W1 */
	CHECK_EQ(c->x[0] ^ UINT32_MAX, 0xe3069283);

	/* Only the low 16 and 32 bits count; values from Python. */
	c->x[0] = UINT32_MAX;
	crc(m, c, 0x1ac14400, 0xffff3231); /* CRC32H W0, W0, W1 */
	CHECK_EQ(c->x[0], 0xb0acbb32);
	c->x[0] = 0xffffffff00000000 | UINT32_MAX;
	crc(m, c, 0x1ac14800, 0xffffffff34333231); /* CRC32W W0, W0, W1 */
	CHECK_EQ(c->x[0], 0x641c1f5c);
	machine_destroy(m);
}

static void test_bit_operations(void)
{
	static const struct {
		uint32_t insn;
		uint64_t x1, result;
	} cases[] = {
		{0xdac00020, 1, 0x8000000000000000},                  /* RBIT X0, X1 */
		{0x5ac00020, 0xffffffff00000001, 0x80000000},         /* RBIT W0, W1 */
		{0xdac00420, 0x0011223344556677, 0x1100332255447766}, /* REV16 X0, X1 */
		{0xdac00820, 0x0011223344556677, 0x3322110077665544}, /* REV32 X0, X1 */
		{0xdac00c20, 0x0011223344556677, 0x7766554433221100}, /* REV X0, X1 */
		{0x5ac00820, 0xffffffff00112233, 0x33221100},         /* REV W0, W1 */
		{0xdac01020, 0, 64},                                  /* CLZ X0, X1 */
		{0xdac01020, 1, 63},                                  /* CLZ X0, X1 */
		{0xdac01020, 0x8000000000000000, 0},                  /* CLZ X0, X1 */
		{0x5ac01020, 0, 32},                                  /* CLZ W0, W1 */
		{0x5ac01020, 0xffffffff00000001, 31},                 /* CLZ W0, W1 */
		{0xdac01420, 0, 63},                                  /* CLS X0, X1 */
		{0xdac01420, UINT64_MAX, 63},                         /* CLS X0, X1 */
		{0xdac01420, 1, 62},                                  /* CLS X0, X1 */
		{0xdac01420, 0x4000000000000000, 0},                  /* CLS X0, X1 */
		{0x5ac01420, 0x80000000, 0},                          /* CLS W0, W1 */
		{0x5ac01420, 0xffffffff, 31},                         /* CLS W0, W1 */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		c->x[0] = 0x5555;
		c->x[1] = cases[i].x1;
		arm64_test_exec(m, c, cases[i].insn);
		CHECK_EQ(c->x[0], cases[i].result);
		machine_destroy(m);
	}
}

static void test_multiply(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 5;
	c->x[2] = 7;
	c->x[3] = 3;
	arm64_test_exec(m, c, 0x9b020c20); /* MADD X0, X1, X2, X3 */
	CHECK_EQ(c->x[0], 38);
	arm64_test_exec(m, c, 0x9b028c20); /* MSUB X0, X1, X2, X3 */
	CHECK_EQ(c->x[0], (uint64_t)-32);
	arm64_test_exec(m, c, 0x9b02fc20); /* MNEG X0, X1, X2 */
	CHECK_EQ(c->x[0], (uint64_t)-35);

	c->x[1] = 0x10000;
	c->x[2] = 0x10001;
	arm64_test_exec(m, c, 0x1b027c20); /* MUL W0, W1, W2 */
	CHECK_EQ(c->x[0], 0x10000);

	/* The long forms multiply the low 32 bits into 64. */
	c->x[1] = 0xfffffffe; /* -2 as a signed word */
	c->x[2] = 3;
	c->x[3] = 100;
	arm64_test_exec(m, c, 0x9b220c20); /* SMADDL X0, W1, W2, X3 */
	CHECK_EQ(c->x[0], 94);
	arm64_test_exec(m, c, 0x9b228c20); /* SMSUBL X0, W1, W2, X3 */
	CHECK_EQ(c->x[0], 106);
	arm64_test_exec(m, c, 0x9ba20c20); /* UMADDL X0, W1, W2, X3 */
	CHECK_EQ(c->x[0], 0x30000005e);
	arm64_test_exec(m, c, 0x9b227c20); /* SMULL X0, W1, W2 */
	CHECK_EQ(c->x[0], (uint64_t)-6);
	arm64_test_exec(m, c, 0x9ba27c20); /* UMULL X0, W1, W2 */
	CHECK_EQ(c->x[0], 0x2fffffffa);
	machine_destroy(m);
}

static void test_multiply_high(void)
{
	/* The high halves of 128-bit products, computed with Python. */
	static const struct {
		uint64_t x, y, umulh, smulh;
	} cases[] = {
		{UINT64_MAX, UINT64_MAX, 0xfffffffffffffffe, 0},
		{0x8000000000000000, 0x8000000000000000, 0x4000000000000000,
	     0x4000000000000000},
		{0x8000000000000000, 1, 0, UINT64_MAX},
		{0x123456789abcdef0, 0xfedcba9876543210, 0x121fa00ad77d7422,
	     0xffeb49923cc09532},
		{UINT64_MAX, 2, 1, UINT64_MAX},
		{0x7fffffffffffffff, 0x7fffffffffffffff, 0x3fffffffffffffff,
	     0x3fffffffffffffff},
	};
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		c->x[1] = cases[i].x;
		c->x[2] = cases[i].y;
		arm64_test_exec(m, c, 0x9bc27c20); /* UMULH X0, X1, X2 */
		CHECK_EQ(c->x[0], cases[i].umulh);
		arm64_test_exec(m, c, 0x9b427c20); /* SMULH X0, X1, X2 */
		CHECK_EQ(c->x[0], cases[i].smulh);
	}

	/* Ra should be 11111; as chosen in dp_reg.c, other values are ignored. */
	arm64_test_exec(m, c, 0x9b420c20); /* SMULH X0, X1, X2 with Ra = 3 */
	CHECK_EQ(c->x[0], 0x3fffffffffffffff);
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
		0xbac20c20, /* SDIV with S = 1 */
		0x1ac24c20, /* CRC32X with sf = 0 */
		0x9ac24020, /* CRC32B with sf = 1 */
		0x5ac00c20, /* 32-bit REV with opc = 11 */
		0x9b42fc20, /* SMULH with o0 = 1 */
		0xbb020c20, /* 3 source with op54 = 01 */
		0x1b220c20, /* 32-bit SMADDL */
		0xba018402, /* RMIF X0, #3, #2 (FEAT_FlagM) */
		0x3a00080d, /* SETF8 W0 (FEAT_FlagM) */
		0x9a022020, /* ADDPT X0, X1, X2 (FEAT_CPA) */
		0x9b620c20, /* MADDPT X0, X1, X2, X3 (FEAT_CPA) */
		0x9ac26020, /* SMAX X0, X1, X2 (FEAT_CSSC) */
		0xdac02020, /* ABS X0, X1 (FEAT_CSSC) */
		0x5ac01c20, /* CNT W0, W1 (FEAT_CSSC) */
		0x9adf1020, /* IRG X0, X1 (FEAT_MTE) */
		0x9ac23020, /* PACGA X0, X1, X2 (FEAT_PAuth) */
		0xdac10020, /* PACIA X0, X1 (FEAT_PAuth) */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
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
	RUN_TEST(test_add_sub_with_carry);
	RUN_TEST(test_divide);
	RUN_TEST(test_shift_by_register);
	RUN_TEST(test_crc32);
	RUN_TEST(test_bit_operations);
	RUN_TEST(test_multiply);
	RUN_TEST(test_multiply_high);
	RUN_TEST(test_undefined);
	return test_summary();
}
