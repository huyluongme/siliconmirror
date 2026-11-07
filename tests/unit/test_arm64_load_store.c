/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the Loads and Stores instructions of the AArch64 model
 * (src/cpu/arm64/load_store.c and mem.c). Every encoding was produced by
 * the GNU assembler, unless a comment says otherwise. Section numbers
 * refer to the Arm Architecture Reference Manual, ARM DDI 0487 M.d.
 */

#include <stdint.h>
#include <string.h>

#include "arm64_test.h"
#include "util/macros.h"

#define DATA 0x1000 /* data area in the test machine's RAM */

static uint64_t peek(Machine *m, uint64_t address, unsigned size)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(m->bus, address, size, &value), BUS_OK);
	return value;
}

static void poke(Machine *m, uint64_t address, unsigned size, uint64_t value)
{
	CHECK_EQ(bus_write(m->bus, address, size, value), BUS_OK);
}

static void test_store_sizes(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	poke(m, DATA, 8, 0);
	poke(m, DATA + 8, 8, 0);
	c->x[1] = DATA;
	c->x[2] = 0x1122334455667788;
	arm64_test_exec(m, c, 0xf9000022); /* STR X2, [X1] */
	CHECK_EQ(peek(m, DATA, 8), 0x1122334455667788);
	arm64_test_exec(m, c, 0xb9000822); /* STR W2, [X1, #8] */
	CHECK_EQ(peek(m, DATA + 8, 4), 0x55667788);
	arm64_test_exec(m, c, 0x79001822); /* STRH W2, [X1, #12] */
	CHECK_EQ(peek(m, DATA + 12, 2), 0x7788);
	arm64_test_exec(m, c, 0x39003822); /* STRB W2, [X1, #14] */
	CHECK_EQ(peek(m, DATA + 14, 1), 0x88);
	CHECK_EQ(peek(m, DATA + 15, 1), 0);
	machine_destroy(m);
}

static void test_load_sizes(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* The low byte, halfword and word are all negative as signed values. */
	poke(m, DATA, 8, 0x0123456789abcdef);
	c->x[1] = DATA;
	for (unsigned i = 3; i <= 12; i++)
		c->x[i] = 0x5555555555555555;

	arm64_test_exec(m, c, 0xf9400023); /* LDR X3, [X1] */
	CHECK_EQ(c->x[3], 0x0123456789abcdef);
	arm64_test_exec(m, c, 0xb9400024); /* LDR W4, [X1] */
	CHECK_EQ(c->x[4], 0x89abcdef);
	arm64_test_exec(m, c, 0x79400025); /* LDRH W5, [X1] */
	CHECK_EQ(c->x[5], 0xcdef);
	arm64_test_exec(m, c, 0x39400026); /* LDRB W6, [X1] */
	CHECK_EQ(c->x[6], 0xef);

	arm64_test_exec(m, c, 0x39800028); /* LDRSB X8, [X1] */
	CHECK_EQ(c->x[8], 0xffffffffffffffef);
	arm64_test_exec(m, c, 0x39c00029); /* LDRSB W9, [X1] */
	CHECK_EQ(c->x[9], 0xffffffef);
	arm64_test_exec(m, c, 0x7980002a); /* LDRSH X10, [X1] */
	CHECK_EQ(c->x[10], 0xffffffffffffcdef);
	arm64_test_exec(m, c, 0x79c0002b); /* LDRSH W11, [X1] */
	CHECK_EQ(c->x[11], 0xffffcdef);
	arm64_test_exec(m, c, 0xb980002c); /* LDRSW X12, [X1] */
	CHECK_EQ(c->x[12], 0xffffffff89abcdef);

	/* The largest unsigned offset of an 8-byte load: 4095 * 8. */
	poke(m, DATA + 32760, 8, 0x77);
	arm64_test_exec(m, c, 0xf97ffc27); /* LDR X7, [X1, #32760] */
	CHECK_EQ(c->x[7], 0x77);
	machine_destroy(m);
}

static void test_pre_and_post_index(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	poke(m, DATA, 8, 1);
	poke(m, DATA + 8, 8, 2);
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0xf8408c20); /* LDR X0, [X1, #8]! */
	CHECK_EQ(c->x[0], 2);
	CHECK_EQ(c->x[1], DATA + 8);
	arm64_test_exec(m, c, 0xf8408420); /* LDR X0, [X1], #8 */
	CHECK_EQ(c->x[0], 2);
	CHECK_EQ(c->x[1], DATA + 16);
	arm64_test_exec(m, c, 0xf85f8c20); /* LDR X0, [X1, #-8]! */
	CHECK_EQ(c->x[0], 2);
	CHECK_EQ(c->x[1], DATA + 8);

	/* Push and pop through SP_EL3. */
	c->sp_el[3] = 0x8000;
	c->x[2] = 0xabc;
	arm64_test_exec(m, c, 0xf81f0fe2); /* STR X2, [SP, #-16]! */
	CHECK_EQ(c->sp_el[3], 0x7ff0);
	CHECK_EQ(peek(m, 0x7ff0, 8), 0xabc);
	arm64_test_exec(m, c, 0xf84107e3); /* LDR X3, [SP], #16 */
	CHECK_EQ(c->x[3], 0xabc);
	CHECK_EQ(c->sp_el[3], 0x8000);
	machine_destroy(m);
}

static void test_unscaled_offset(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = DATA + 0x10;
	poke(m, DATA + 8, 8, 0x55);
	arm64_test_exec(m, c, 0xf85f8020); /* LDUR X0, [X1, #-8] */
	CHECK_EQ(c->x[0], 0x55);

	c->x[2] = 0xdeadbeef;
	arm64_test_exec(m, c, 0xb81fc022); /* STUR W2, [X1, #-4] */
	CHECK_EQ(peek(m, DATA + 0xc, 4), 0xdeadbeef);
	arm64_test_exec(m, c, 0x380ff022); /* STURB W2, [X1, #255] */
	CHECK_EQ(peek(m, DATA + 0x10 + 255, 1), 0xef);

	poke(m, DATA + 0x10 - 256, 4, 0x80000000);
	arm64_test_exec(m, c, 0xb8900023); /* LDURSW X3, [X1, #-256] */
	CHECK_EQ(c->x[3], 0xffffffff80000000);
	machine_destroy(m);
}

static void test_literal(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);
	uint64_t pc;

	/* A second MiB of RAM for the limits of the 21-bit offset. */
	CHECK_EQ(bus_map_ram(m->bus, "high", 0x100000, 0x100000), BUS_OK);

	pc = c->pc;
	poke(m, pc + 16, 8, 0x1122334455667788);
	arm64_test_exec(m, c, 0x58000080); /* LDR X0, . + 16 */
	CHECK_EQ(c->x[0], 0x1122334455667788);

	pc = c->pc;
	poke(m, pc - 8, 4, 0xcafef00d);
	arm64_test_exec(m, c, 0x18ffffc1); /* LDR W1, . - 8 */
	CHECK_EQ(c->x[1], 0xcafef00d);

	c->pc = 0;
	poke(m, 0xffffc, 4, 0x80000000);
	arm64_test_exec(m, c, 0x987fffe2); /* LDRSW X2, . + 0xffffc */
	CHECK_EQ(c->x[2], 0xffffffff80000000);

	c->pc = 0x100000;
	poke(m, 0, 8, 0x0123456789abcdef);
	arm64_test_exec(m, c, 0x58800003); /* LDR X3, . - 0x100000 */
	CHECK_EQ(c->x[3], 0x0123456789abcdef);

	/* PRFM (literal) is a hint: nothing to check but that it runs. */
	arm64_test_exec(m, c, 0xd8000040); /* PRFM PLDL1KEEP, . + 8 */
	machine_destroy(m);

	/* A literal is only 4-byte aligned, which is not enough for LDR X. */
	m = arm64_test_machine(&c);
	CHECK_EQ(arm64_test_step(m, c, 0x58000020), /* LDR X0, . + 4 */
	         MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "8-byte read from 0x0000000000080004 "
	                              "failed: alignment fault") != NULL);
	machine_destroy(m);
}

static void test_pairs(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* Rt goes to the lower address. */
	c->sp_el[3] = 0x8000;
	c->x[1] = 1;
	c->x[2] = 2;
	arm64_test_exec(m, c, 0xa9bf0be1); /* STP X1, X2, [SP, #-16]! */
	CHECK_EQ(c->sp_el[3], 0x7ff0);
	CHECK_EQ(peek(m, 0x7ff0, 8), 1);
	CHECK_EQ(peek(m, 0x7ff8, 8), 2);
	arm64_test_exec(m, c, 0xa8c113e3); /* LDP X3, X4, [SP], #16 */
	CHECK_EQ(c->x[3], 1);
	CHECK_EQ(c->x[4], 2);
	CHECK_EQ(c->sp_el[3], 0x8000);

	c->x[10] = DATA;
	poke(m, DATA + 8, 4, 0x11);
	poke(m, DATA + 12, 4, 0x22);
	arm64_test_exec(m, c, 0x29411945); /* LDP W5, W6, [X10, #8] */
	CHECK_EQ(c->x[5], 0x11);
	CHECK_EQ(c->x[6], 0x22);
	arm64_test_exec(m, c, 0x293f1945); /* STP W5, W6, [X10, #-8] */
	CHECK_EQ(peek(m, DATA - 8, 4), 0x11);
	CHECK_EQ(peek(m, DATA - 4, 4), 0x22);

	poke(m, DATA, 4, 0x80000000);
	poke(m, DATA + 4, 4, 0x7fffffff);
	arm64_test_exec(m, c, 0x69402147); /* LDPSW X7, X8, [X10] */
	CHECK_EQ(c->x[7], 0xffffffff80000000);
	CHECK_EQ(c->x[8], 0x7fffffff);

	c->x[1] = 0xa;
	c->x[2] = 0xb;
	arm64_test_exec(m, c, 0xa8010941); /* STNP X1, X2, [X10, #16] */
	CHECK_EQ(peek(m, DATA + 16, 8), 0xa);
	CHECK_EQ(peek(m, DATA + 24, 8), 0xb);
	arm64_test_exec(m, c, 0xa8411143); /* LDNP X3, X4, [X10, #16] */
	CHECK_EQ(c->x[3], 0xa);
	CHECK_EQ(c->x[4], 0xb);

	/* The limits of the 7-bit signed offset, scaled by 8. */
	c->x[10] = 0x4000;
	poke(m, 0x4000 + 504, 8, 0x504);
	poke(m, 0x4000 + 512, 8, 0x512);
	arm64_test_exec(m, c, 0xa95f8540); /* LDP X0, X1, [X10, #504] */
	CHECK_EQ(c->x[0], 0x504);
	CHECK_EQ(c->x[1], 0x512);
	poke(m, 0x4000 - 512, 8, 0x1);
	poke(m, 0x4000 - 504, 8, 0x2);
	arm64_test_exec(m, c, 0xa9600540); /* LDP X0, X1, [X10, #-512] */
	CHECK_EQ(c->x[0], 0x1);
	CHECK_EQ(c->x[1], 0x2);
	machine_destroy(m);
}

static void test_register_offset(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	for (unsigned i = 0; i < 4; i++)
		poke(m, DATA - 8 + 8 * i, 8, 0x1111111111111111 * (i + 1));
	c->x[1] = DATA;

	c->x[2] = 8;
	arm64_test_exec(m, c, 0xf8626820); /* LDR X0, [X1, X2] */
	CHECK_EQ(c->x[0], 0x3333333333333333);
	c->x[2] = 2;
	arm64_test_exec(m, c, 0xf8627820); /* LDR X0, [X1, X2, LSL #3] */
	CHECK_EQ(c->x[0], 0x4444444444444444);

	/* Only W2 counts, zero- or sign-extended. */
	c->x[2] = 0xffffffff00000003;
	arm64_test_exec(m, c, 0xb8625820); /* LDR W0, [X1, W2, UXTW #2] */
	CHECK_EQ(c->x[0], 0x33333333);
	c->x[2] = 0xfffffff8;
	arm64_test_exec(m, c, 0xf862c820); /* LDR X0, [X1, W2, SXTW] */
	CHECK_EQ(c->x[0], 0x1111111111111111);

	c->x[0] = 0xab;
	c->x[2] = 5;
	arm64_test_exec(m, c, 0x38226820); /* STRB W0, [X1, X2] */
	CHECK_EQ(peek(m, DATA + 5, 1), 0xab);

	poke(m, DATA + 6, 2, 0x8001);
	c->x[2] = 3;
	arm64_test_exec(m, c, 0x78a27820); /* LDRSH X0, [X1, X2, LSL #1] */
	CHECK_EQ(c->x[0], 0xffffffffffff8001);
	c->x[2] = 6;
	arm64_test_exec(m, c, 0x38627820); /* LDRB W0, [X1, X2, LSL #0] */
	CHECK_EQ(c->x[0], 0x01);

	/* Rn 31 is SP. */
	c->sp_el[3] = 0x8000;
	c->x[0] = 0x77;
	c->x[2] = 1;
	arm64_test_exec(m, c, 0xf8227be0); /* STR X0, [SP, X2, LSL #3] */
	CHECK_EQ(peek(m, 0x8008, 8), 0x77);

	/* Prefetches do nothing; RPRFM is a NOP without FEAT_RPRFM. */
	c->x[1] = 0x200000;
	arm64_test_exec(m, c, 0xf8a26820); /* PRFM PLDL1KEEP, [X1, X2] */
	arm64_test_exec(m, c, 0xf8a24838); /* RPRFM PLDKEEP, X2, [X1] */
	machine_destroy(m);
}

static void test_prefetch_has_no_effect(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* Prefetching from nowhere, even unaligned, does not fault. */
	c->x[1] = 0x200000;
	arm64_test_exec(m, c, 0xf9800020); /* PRFM PLDL1KEEP, [X1] */
	arm64_test_exec(m, c, 0xf89ff033); /* PRFUM PSTL2STRM, [X1, #-1] */
	CHECK_EQ(c->x[1], 0x200000);
	machine_destroy(m);
}

static void test_register_31(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* As Rt, 31 is the zero register; SP never changes. */
	c->x[1] = DATA;
	c->sp_el[3] = 0x8000;
	poke(m, DATA, 8, UINT64_MAX);
	arm64_test_exec(m, c, 0xf900003f); /* STR XZR, [X1] */
	CHECK_EQ(peek(m, DATA, 8), 0);

	poke(m, DATA, 8, 5);
	arm64_test_exec(m, c, 0xf940003f); /* LDR XZR, [X1] */
	CHECK_EQ(c->sp_el[3], 0x8000);
	machine_destroy(m);
}

static void test_constrained_unpredictable_choices(void)
{
	/* The choices listed at the top of load_store.c. */
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	poke(m, DATA, 8, 1);
	poke(m, DATA + 8, 8, 2);

	/* A load with writeback to its destination: no writeback. */
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0xf8408421); /* LDR X1, [X1], #8 */
	CHECK_EQ(c->x[1], 1);
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0xa8c10420); /* LDP X0, X1, [X1], #16 */
	CHECK_EQ(c->x[0], 1);
	CHECK_EQ(c->x[1], 2);

	/* A load pair to one register: it gets the second value. */
	c->x[2] = DATA;
	arm64_test_exec(m, c, 0xa9400441); /* LDP X1, X1, [X2] */
	CHECK_EQ(c->x[1], 2);

	/* A store with writeback of its source: the old value is stored. */
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0xf8008421); /* STR X1, [X1], #8 */
	CHECK_EQ(peek(m, DATA, 8), DATA);
	CHECK_EQ(c->x[1], DATA + 8);
	c->x[0] = 0xa;
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0xa9810420); /* STP X0, X1, [X1, #16]! */
	CHECK_EQ(peek(m, DATA + 16, 8), 0xa);
	CHECK_EQ(peek(m, DATA + 24, 8), DATA);
	CHECK_EQ(c->x[1], DATA + 16);
	machine_destroy(m);
}

static void test_alignment_faults(void)
{
	/* D8.2.12.1 and J1.2.4.30: with the MMU off, data is Device memory. */
	static const struct {
		uint32_t insn;
		uint64_t x1;
		const char *message;
	} cases[] = {
		{0xf9400020, DATA + 4, /* LDR X0, [X1] */
	     "cpu0: data abort at 0x0000000000080000: 8-byte read from "
	     "0x0000000000001004 failed: alignment fault, as data accesses "
	     "use Device memory while the MMU is off (exceptions are not "
	     "implemented yet)"},
		{0xa9400420, DATA + 4, /* LDP X0, X1, [X1] */
	     "cpu0: data abort at 0x0000000000080000: 8-byte read from "
	     "0x0000000000001004 failed: alignment fault, as data accesses "
	     "use Device memory while the MMU is off (exceptions are not "
	     "implemented yet)"},
		{0x79000020, DATA + 1, /* STRH W0, [X1] */
	     "cpu0: data abort at 0x0000000000080000: 2-byte write to "
	     "0x0000000000001001 failed: alignment fault, as data accesses "
	     "use Device memory while the MMU is off (exceptions are not "
	     "implemented yet)"},
		{0xf8004020, DATA, /* STUR X0, [X1, #4] */
	     "cpu0: data abort at 0x0000000000080000: 8-byte write to "
	     "0x0000000000001004 failed: alignment fault, as data accesses "
	     "use Device memory while the MMU is off (exceptions are not "
	     "implemented yet)"},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		c->x[0] = 0x5555;
		c->x[1] = cases[i].x1;
		CHECK_EQ(arm64_test_step(m, c, cases[i].insn), MACHINE_STOP_ERROR);
		if (strcmp(m->stop_message, cases[i].message) != 0) {
			fprintf(stderr, "unexpected message: %s\n", m->stop_message);
			test_failures++;
		}
		CHECK_EQ(c->x[0], 0x5555);
		CHECK_EQ(c->pc, RESET_PC);
		CHECK_EQ(m->insns, 0);
		machine_destroy(m);
	}

	/* A pair is checked per register: 4-byte alignment is enough here. */
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = DATA + 4;
	arm64_test_exec(m, c, 0x29400420); /* LDP W0, W1, [X1] */
	machine_destroy(m);
}

static void test_bus_errors(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* Nothing changes, not even the base register of a writeback. */
	c->x[0] = 0x5555;
	c->x[1] = 0x200000;
	CHECK_EQ(arm64_test_step(m, c, 0xf8408c20), /* LDR X0, [X1, #8]! */
	         MACHINE_STOP_ERROR);
	CHECK(strcmp(m->stop_message,
	             "cpu0: data abort at 0x0000000000080000: 8-byte read from "
	             "0x0000000000200008 failed: no region at this address "
	             "(exceptions are not implemented yet)") == 0);
	CHECK_EQ(c->x[0], 0x5555);
	CHECK_EQ(c->x[1], 0x200000);
	machine_destroy(m);

	/* A pair that runs off the end of RAM: the second register fails. */
	m = arm64_test_machine(&c);
	c->x[2] = 0xffff8;
	CHECK_EQ(arm64_test_step(m, c, 0xa9000440), /* STP X0, X1, [X2] */
	         MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "8-byte write to 0x0000000000100000 "
	                              "failed: no region at this address") != NULL);
	machine_destroy(m);
}

static void test_classes_newer_than_armv8_0_are_undefined(void)
{
	static const uint32_t insns[] = {
		0xf8200041, /* LDADD X0, X1, [X2] (FEAT_LSE) */
		0x69000440, /* STGP X0, X1, [X2] (FEAT_MTE) */
		0xd9400020, /* LDAPUR X0, [X1] (FEAT_LRCPC2) */
		0xf8200420, /* LDRAA X0, [X1] (FEAT_PAuth) */
		0x19010440, /* CPYFP [X0]!, [X1]!, X2! (FEAT_MOPS) */
		0xe9400440, /* LDTP X0, X1, [X2] (FEAT_LSUI) */
		0xed400440, /* LDTP Q0, Q1, [X2] (FEAT_LSUI) */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

static void test_unallocated(void)
{
	/* Built from the tables in C4.1; binutils agrees they are invalid. */
	static const uint32_t insns[] = {
		0xf8800420, /* PRFM with post-indexing */
		0xb9c00020, /* size = 10, opc = 11: no LDRSW to 32 bits */
		0xdc000000, /* LDR (literal) with opc = 11 and VR = 1 */
		0x68400000, /* LDNP with opc = 01: no LDNPSW */
		0xf8620820, /* LDR (register) with option = 000 */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

static void test_not_implemented_yet(void)
{
	static const uint32_t insns[] = {
		0xc85f7c20, /* LDXR X0, [X1] */
		0xc8dffc20, /* LDAR X0, [X1] */
		0xc89ffc20, /* STLR X0, [X1] */
		0xf8400820, /* LDTR X0, [X1] */
		0x3dc00020, /* LDR Q0, [X1] */
		0x4c407020, /* LD1 {V0.16B}, [X1] */
		0x5c000040, /* LDR D0, . + 8 */
		0x6d400420, /* LDP D0, D1, [X1] */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(insns); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		CHECK_EQ(arm64_test_step(m, c, insns[i]), MACHINE_STOP_ERROR);
		CHECK(strstr(m->stop_message,
		             "is not implemented yet (Loads and Stores)") != NULL);
		machine_destroy(m);
	}
}

int main(void)
{
	RUN_TEST(test_store_sizes);
	RUN_TEST(test_load_sizes);
	RUN_TEST(test_pre_and_post_index);
	RUN_TEST(test_unscaled_offset);
	RUN_TEST(test_literal);
	RUN_TEST(test_pairs);
	RUN_TEST(test_register_offset);
	RUN_TEST(test_prefetch_has_no_effect);
	RUN_TEST(test_register_31);
	RUN_TEST(test_constrained_unpredictable_choices);
	RUN_TEST(test_alignment_faults);
	RUN_TEST(test_bus_errors);
	RUN_TEST(test_classes_newer_than_armv8_0_are_undefined);
	RUN_TEST(test_unallocated);
	RUN_TEST(test_not_implemented_yet);
	return test_summary();
}
