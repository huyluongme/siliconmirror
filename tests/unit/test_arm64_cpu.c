/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the AArch64 CPU framework (src/cpu/arm64/): reset state,
 * registers, top-level decoding and instruction fetch. Section numbers
 * refer to the Arm Architecture Reference Manual, ARM DDI 0487 M.d.
 */

#include <stdint.h>
#include <string.h>

#include "arm64_test.h"
#include "cpu/arm64/internal.h"

static void test_reset_state(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* J1.2.2.30: highest EL, SP_ELx selected, D, A, I and F masked. */
	CHECK_EQ(c->pc, RESET_PC);
	CHECK_EQ(c->el, 3);
	CHECK(c->sp_sel);
	CHECK_EQ(c->daif, 0x3c0);
	CHECK_EQ(c->nzcv, 0);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	for (unsigned i = 0; i < 31; i++)
		CHECK_EQ(c->x[i], 0);

	/* Reset again after changing the state. */
	c->x[5] = 42;
	c->pc = 0x1234;
	c->el = 1;
	machine_reset(m);
	CHECK_EQ(c->x[5], 0);
	CHECK_EQ(c->pc, RESET_PC);
	CHECK_EQ(c->el, 3);
	machine_destroy(m);
}

static void test_reset_el_follows_config(void)
{
	static const Arm64Config el2 = {.reset_pc = 0, .highest_el = 2};
	static const Arm64Config bad0 = {.reset_pc = 0, .highest_el = 0};
	static const Arm64Config bad4 = {.reset_pc = 0, .highest_el = 4};
	Cpu *base = arm64_cpu_create(&el2);

	CHECK_EQ(arm64_cpu_of(base)->el, 2);
	base->ops->destroy(base);
	CHECK(arm64_cpu_create(&bad0) == NULL);
	CHECK(arm64_cpu_create(&bad4) == NULL);
}

static void test_register_31(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* C1.2.6.1: as an Xn operand, 31 is the zero register. */
	set_x(c, 31, 0x1234);
	CHECK_EQ(get_x(c, 31), 0);
	for (unsigned i = 0; i < 31; i++)
		CHECK_EQ(c->x[i], 0);

	/* As an SP operand, 31 is the current stack pointer. */
	set_xsp(c, 31, 0x8000);
	CHECK_EQ(get_xsp(c, 31), 0x8000);
	CHECK_EQ(get_x(c, 31), 0);
	machine_destroy(m);
}

static void test_32_bit_writes_zero_extend(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	set_x(c, 3, UINT64_MAX);
	set_w(c, 3, 0x12345678);
	CHECK_EQ(get_x(c, 3), 0x12345678);
	machine_destroy(m);
}

static void test_stack_pointer_selection(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* J1.2.3.259: PSTATE.SP selects SP_EL0 or SP_ELx of the current EL. */
	for (unsigned el = 0; el < 4; el++)
		c->sp_el[el] = 0x1000 * (el + 1);

	c->el = 2;
	c->sp_sel = true;
	CHECK_EQ(get_xsp(c, 31), 0x3000);
	c->sp_sel = false;
	CHECK_EQ(get_xsp(c, 31), 0x1000);

	c->el = 1;
	c->sp_sel = true;
	set_xsp(c, 31, 0xabc0);
	CHECK_EQ(c->sp_el[1], 0xabc0);
	CHECK_EQ(c->sp_el[0], 0x1000);
	machine_destroy(m);
}

static void test_top_level_groups(void)
{
	/* One instruction from each row of the table in C4.1. */
	static const struct {
		uint32_t insn;
		Arm64Group group;
	} cases[] = {
		{0x00000000, ARM64_GROUP_RESERVED},    /* UDF #0 */
		{0x80000000, ARM64_GROUP_SME},         /* op0 = 1, op1 = 0000 */
		{0x04000000, ARM64_GROUP_SVE},         /* op1 = 0010 */
		{0x02000000, ARM64_GROUP_UNALLOCATED}, /* op1 = 0001 */
		{0x06000000, ARM64_GROUP_UNALLOCATED}, /* op1 = 0011 */
		{0x91000000, ARM64_GROUP_DP_IMM},      /* ADD X0, X0, #0 */
		{0x14000000, ARM64_GROUP_BRANCH_SYS},  /* B . */
		{0xd503201f, ARM64_GROUP_BRANCH_SYS},  /* NOP */
		{0x8b000000, ARM64_GROUP_DP_REG},      /* ADD X0, X0, X0 */
		{0x1e260000, ARM64_GROUP_SIMD_FP},     /* FMOV W0, S0 */
		{0xf9400000, ARM64_GROUP_LOAD_STORE},  /* LDR X0, [X0] */
		{0xb9000000, ARM64_GROUP_LOAD_STORE},  /* STR W0, [X0] */
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		CHECK_EQ(arm64_group(cases[i].insn), cases[i].group);

	/* Every value of bits 28:25 maps to a group, for both values of op0. */
	for (uint32_t op0 = 0; op0 < 2; op0++) {
		for (uint32_t op1 = 0; op1 < 16; op1++) {
			const char *name =
				arm64_group_name(arm64_group(op0 << 31 | op1 << 25));

			CHECK(strcmp(name, "unknown group") != 0);
		}
	}
}

static void test_executed_instructions_advance_pc(void)
{
	static const uint32_t program[] = {
		0xd2800020, /* MOVZ X0, #1 */
		0xd2800041, /* MOVZ X1, #2 */
		0xd2800062, /* MOVZ X2, #3 */
	};
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	for (unsigned i = 0; i < 3; i++)
		CHECK_EQ(bus_write(m->bus, RESET_PC + 4 * i, 4, program[i]), BUS_OK);

	CHECK_EQ(machine_run(m, 3), MACHINE_STOP_LIMIT);
	CHECK_EQ(m->insns, 3);
	CHECK_EQ(c->pc, RESET_PC + 12);
	CHECK_EQ(c->x[0], 1);
	CHECK_EQ(c->x[1], 2);
	CHECK_EQ(c->x[2], 3);
	machine_destroy(m);
}

static void test_undefined_instruction_stops(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	CHECK_EQ(arm64_test_step(m, c, 0x00000000),
	         MACHINE_STOP_ERROR); /* UDF #0 */
	CHECK(strcmp(m->stop_message,
	             "cpu0: undefined instruction 0x00000000 at "
	             "0x0000000000080000 (exceptions are not implemented yet)") ==
	      0);
	CHECK_EQ(m->insns, 0);
	CHECK_EQ(c->pc, RESET_PC);
	machine_destroy(m);
}

static void test_features_this_cpu_lacks_are_undefined(void)
{
	/* SME, SVE and UNALLOCATED encodings are all UNDEFINED here. */
	static const uint32_t insns[] = {0x80000000, 0x04000000, 0x02000000};

	for (unsigned i = 0; i < sizeof(insns) / sizeof(insns[0]); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		CHECK_EQ(arm64_test_step(m, c, insns[i]), MACHINE_STOP_ERROR);
		CHECK(strstr(m->stop_message, "undefined instruction") != NULL);
		machine_destroy(m);
	}
}

static void test_unimplemented_instruction_stops(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	CHECK_EQ(arm64_test_step(m, c, 0x1e260000),
	         MACHINE_STOP_ERROR); /* FMOV W0, S0 */
	CHECK(strcmp(m->stop_message,
	             "cpu0: instruction 0x1e260000 at 0x0000000000080000 is not "
	             "implemented yet (Data Processing -- Scalar Floating-Point "
	             "and Advanced SIMD)") == 0);
	CHECK_EQ(m->insns, 0);
	machine_destroy(m);
}

static void test_fetch_from_unmapped_address(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->pc = 0x200000; /* above the 1 MiB of RAM */
	CHECK_EQ(machine_run(m, 1), MACHINE_STOP_ERROR);
	CHECK(strcmp(m->stop_message,
	             "cpu0: instruction fetch from 0x0000000000200000 failed: no "
	             "region at this address") == 0);
	machine_destroy(m);
}

static void test_misaligned_pc(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* D1.4.10.1: bits 1:0 of the PC must be 0. */
	c->pc = RESET_PC + 2;
	CHECK_EQ(machine_run(m, 1), MACHINE_STOP_ERROR);
	CHECK(strcmp(m->stop_message,
	             "cpu0: PC alignment fault at 0x0000000000080002 (exceptions "
	             "are not implemented yet)") == 0);
	machine_destroy(m);
}

int main(void)
{
	RUN_TEST(test_reset_state);
	RUN_TEST(test_reset_el_follows_config);
	RUN_TEST(test_register_31);
	RUN_TEST(test_32_bit_writes_zero_extend);
	RUN_TEST(test_stack_pointer_selection);
	RUN_TEST(test_top_level_groups);
	RUN_TEST(test_executed_instructions_advance_pc);
	RUN_TEST(test_undefined_instruction_stops);
	RUN_TEST(test_features_this_cpu_lacks_are_undefined);
	RUN_TEST(test_unimplemented_instruction_stops);
	RUN_TEST(test_fetch_from_unmapped_address);
	RUN_TEST(test_misaligned_pc);
	return test_summary();
}
