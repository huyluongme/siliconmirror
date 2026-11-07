/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the Branches, Exception Generating and System
 * instructions of the AArch64 model (src/cpu/arm64/branch_sys.c).
 * Every encoding was produced by the GNU assembler, unless a comment
 * says otherwise. Section numbers refer to the Arm Architecture
 * Reference Manual, ARM DDI 0487 M.d.
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

/* Run one instruction at the PC and return how far the PC moved. */
static int64_t jump(Machine *m, Arm64Cpu *c, uint32_t insn)
{
	uint64_t pc = c->pc;

	arm64_test_exec(m, c, insn);
	return (int64_t)(c->pc - pc);
}

static void test_b_and_bl(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	CHECK_EQ(jump(m, c, 0x14000002), 8);  /* B . + 8 */
	CHECK_EQ(jump(m, c, 0x17ffffff), -4); /* B . - 4 */

	/* BL stores the return address in X30. */
	uint64_t pc = c->pc;

	CHECK_EQ(jump(m, c, 0x94000040), 0x100); /* BL . + 0x100 */
	CHECK_EQ(c->x[30], pc + 4);

	/* The limits of the 26-bit offset, +-128MB; only the PC moves. */
	c->pc = RESET_PC;
	CHECK_EQ(jump(m, c, 0x16000000), -0x8000000); /* B . - 0x8000000 */
	c->pc = RESET_PC;
	CHECK_EQ(jump(m, c, 0x15ffffff), 0x7fffffc); /* B . + 0x7fffffc */
	machine_destroy(m);
}

/* What each condition means, written as the list in C1.2.4 has it. */
static bool condition_meaning(unsigned cond, uint32_t nzcv)
{
	bool n = nzcv & N, z = nzcv & Z, cf = nzcv & C, v = nzcv & V;

	switch (cond) {
	case 0x0: /* EQ */
		return z;
	case 0x1: /* NE */
		return !z;
	case 0x2: /* CS, HS */
		return cf;
	case 0x3: /* CC, LO */
		return !cf;
	case 0x4: /* MI */
		return n;
	case 0x5: /* PL */
		return !n;
	case 0x6: /* VS */
		return v;
	case 0x7: /* VC */
		return !v;
	case 0x8: /* HI */
		return cf && !z;
	case 0x9: /* LS */
		return !cf || z;
	case 0xa: /* GE */
		return n == v;
	case 0xb: /* LT */
		return n != v;
	case 0xc: /* GT */
		return !z && n == v;
	case 0xd: /* LE */
		return z || n != v;
	default: /* AL, NV */
		return true;
	}
}

static void test_conditional_branch_all_conditions(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* B.<cond> . + 8 for every condition and every value of NZCV. */
	for (unsigned cond = 0; cond < 16; cond++) {
		for (uint32_t flags = 0; flags < 16; flags++) {
			c->pc = RESET_PC;
			c->nzcv = flags << 28;
			bool taken = jump(m, c, 0x54000040 | cond) == 8;

			if (taken != condition_meaning(cond, c->nzcv)) {
				fprintf(stderr, "cond %x with NZCV %x\n", cond, flags);
				test_failures++;
			}
		}
	}

	/* Two encodings checked against the assembler. */
	c->pc = RESET_PC;
	c->nzcv = Z;
	CHECK_EQ(jump(m, c, 0x54000040), 8); /* B.EQ . + 8 */
	CHECK_EQ(jump(m, c, 0x5400004f), 8); /* B.NV . + 8: always taken */
	c->nzcv = 0;
	CHECK_EQ(jump(m, c, 0x54800001), -0x100000); /* B.NE . - 0x100000 */
	machine_destroy(m);
}

/* Flip the sign bit, so that unsigned order is the signed order. */
static uint64_t flip(uint64_t x)
{
	return x ^ (uint64_t)1 << 63;
}

static void test_compare_then_branch(void)
{
	/*
	 * CMP X1, #imm and then B.<cond>: each condition must give the
	 * comparison that its name promises, signed or unsigned.
	 */
	static const uint64_t values[] = {
		0, 1, 2, 4095, 0x7fffffffffffffff, 0x8000000000000000, UINT64_MAX,
	};
	static const struct {
		uint32_t insn;
		unsigned imm;
	} compares[] = {
		{0xf100003f, 0},    /* CMP X1, #0 */
		{0xf100043f, 1},    /* CMP X1, #1 */
		{0xf100083f, 2},    /* CMP X1, #2 */
		{0xf13ffc3f, 4095}, /* CMP X1, #4095 */
	};
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
		for (unsigned j = 0; j < ARRAY_SIZE(compares); j++) {
			uint64_t a = values[i], b = compares[j].imm;
			bool negative = (a - b) >> 63;
			bool less = flip(a) < flip(b); /* signed a < b */
			bool overflow = negative != less;
			const bool expected[16] = {
				a == b,      /* EQ */
				a != b,      /* NE */
				a >= b,      /* HS */
				a<b,         /* LO */
			      negative,  /* MI */
			      !negative, /* PL */
			      overflow,  /* VS */
			      !overflow, /* VC */
			      a>
					b,              /* HI */
				a <= b,             /* LS */
				!less,              /* GE */
				less,               /* LT */
				flip(a) > flip(b),  /* GT */
				flip(a) <= flip(b), /* LE */
				true,               /* AL */
				true,               /* NV */
			};

			for (unsigned cond = 0; cond < 16; cond++) {
				c->pc = RESET_PC;
				c->x[1] = a;
				arm64_test_exec(m, c, compares[j].insn);
				if ((jump(m, c, 0x54000040 | cond) == 8) != expected[cond]) {
					fprintf(stderr, "cmp 0x%" PRIx64 ", %u with cond %x\n", a,
					        compares[j].imm, cond);
					test_failures++;
				}
			}
		}
	}
	machine_destroy(m);
}

static void test_cbz_and_cbnz(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 0;
	CHECK_EQ(jump(m, c, 0xb4000041), 8); /* CBZ X1, . + 8 */
	CHECK_EQ(jump(m, c, 0xb5000041), 4); /* CBNZ X1, . + 8 */

	/* The 32-bit forms only look at W1. */
	c->x[1] = 0x100000000;
	CHECK_EQ(jump(m, c, 0x34000041), 8); /* CBZ W1, . + 8 */
	CHECK_EQ(jump(m, c, 0x35000041), 4); /* CBNZ W1, . + 8 */
	CHECK_EQ(jump(m, c, 0xb4000041), 4); /* CBZ X1, . + 8 */
	CHECK_EQ(jump(m, c, 0xb5000041), 8); /* CBNZ X1, . + 8 */

	c->x[1] = 0;
	CHECK_EQ(jump(m, c, 0xb47fffe1), 0xffffc); /* CBZ X1, . + 0xffffc */
	machine_destroy(m);
}

static void test_tbz_and_tbnz(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = (uint64_t)1 << 40 | (uint64_t)1 << 63;
	CHECK_EQ(jump(m, c, 0xb7400041), 8); /* TBNZ X1, #40, . + 8 */
	CHECK_EQ(jump(m, c, 0xb6400041), 4); /* TBZ X1, #40, . + 8 */
	CHECK_EQ(jump(m, c, 0x36180041), 8); /* TBZ W1, #3, . + 8 */
	CHECK_EQ(jump(m, c, 0xb7f80041), 8); /* TBNZ X1, #63, . + 8 */

	/* The lower limit of the 14-bit offset, -32KB. */
	CHECK_EQ(jump(m, c, 0x36040001), -0x8000); /* TBZ W1, #0, . - 0x8000 */
	machine_destroy(m);
}

static void test_branch_to_register(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[1] = 0x90000;
	arm64_test_exec(m, c, 0xd61f0020); /* BR X1 */
	CHECK_EQ(c->pc, 0x90000);

	c->x[1] = 0xa0000;
	arm64_test_exec(m, c, 0xd63f0020); /* BLR X1 */
	CHECK_EQ(c->pc, 0xa0000);
	CHECK_EQ(c->x[30], 0x90004);

	arm64_test_exec(m, c, 0xd65f03c0); /* RET */
	CHECK_EQ(c->pc, 0x90004);

	c->x[2] = 0xb0000;
	arm64_test_exec(m, c, 0xd65f0040); /* RET X2 */
	CHECK_EQ(c->pc, 0xb0000);

	/* BLR X30 jumps to the old X30, then sets X30. */
	c->x[30] = 0xc0000;
	arm64_test_exec(m, c, 0xd63f03c0); /* BLR X30 */
	CHECK_EQ(c->pc, 0xc0000);
	CHECK_EQ(c->x[30], 0xb0004);
	machine_destroy(m);
}

static void test_loop_and_call(void)
{
	/* The first complete program: a loop, a call, and WFI at the end. */
	static const uint32_t program[] = {
		0xd2800000, /*       MOV  X0, #0 */
		0xd2800141, /*       MOV  X1, #10 */
		0x91000c00, /* loop: ADD  X0, X0, #3 */
		0xf1000421, /*       SUBS X1, X1, #1 */
		0x54ffffc1, /*       B.NE loop */
		0x94000002, /*       BL   func */
		0xd503207f, /*       WFI */
		0x91000400, /* func: ADD  X0, X0, #1 */
		0xd65f03c0, /*       RET */
	};
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	for (unsigned i = 0; i < ARRAY_SIZE(program); i++)
		CHECK_EQ(bus_write(m->bus, RESET_PC + 4 * i, 4, program[i]), BUS_OK);

	/* A limit, so that a broken branch cannot loop forever. */
	CHECK_EQ(machine_run(m, 1000), MACHINE_STOP_IDLE);
	CHECK_EQ(c->x[0], 31);
	CHECK_EQ(c->x[30], RESET_PC + 0x18);
	CHECK_EQ(c->pc, RESET_PC + 0x1c); /* after the WFI */
	CHECK_EQ(m->insns, 2 + 10 * 3 + 1 + 2 + 1);
	machine_destroy(m);
}

static void test_hints_without_a_feature_are_nops(void)
{
	static const uint32_t insns[] = {
		0xd503201f, /* NOP */
		0xd503203f, /* YIELD */
		0xd503229f, /* CSDB */
		0xd503233f, /* PACIASP (FEAT_PAuth) */
		0xd50323bf, /* AUTIASP (FEAT_PAuth) */
		0xd503245f, /* BTI C (FEAT_BTI) */
		0xd503221f, /* ESB (FEAT_RAS) */
		0xd5032fff, /* HINT #0x7f, not allocated */
	};
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	c->x[30] = 0x1234; /* PACIASP and AUTIASP would change it */
	for (unsigned i = 0; i < ARRAY_SIZE(insns); i++)
		CHECK_EQ(jump(m, c, insns[i]), 4);
	CHECK_EQ(c->x[30], 0x1234);
	machine_destroy(m);
}

static void test_wfi_waits(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	arm64_test_exec(m, c, 0xd503207f); /* WFI */
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	CHECK_EQ(c->wait, ARM64_WAIT_INTERRUPT);
	CHECK_EQ(c->pc, RESET_PC + 4);

	/* Nothing can wake it: there is no interrupt input yet. */
	CHECK_EQ(machine_run(m, 1000), MACHINE_STOP_IDLE);
	CHECK(strcmp(m->stop_message,
	             "every CPU is waiting and no event is pending") == 0);
	machine_destroy(m);
}

static void test_wfe_and_the_event_register(void)
{
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	/* The Event Register is clear after reset: WFE waits. */
	CHECK(!c->event_register);
	arm64_test_exec(m, c, 0xd503205f); /* WFE */
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	CHECK_EQ(c->wait, ARM64_WAIT_EVENT);

	/* SEVL sets it, and the next WFE clears it instead of waiting. */
	c->cpu.state = CPU_RUNNING;
	arm64_test_exec(m, c, 0xd50320bf); /* SEVL */
	CHECK(c->event_register);
	arm64_test_exec(m, c, 0xd503205f); /* WFE */
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	CHECK(!c->event_register);

	/* SEV signals this PE too. */
	arm64_test_exec(m, c, 0xd503209f); /* SEV */
	CHECK(c->event_register);
	machine_destroy(m);
}

static void test_sev_wakes_other_cpus(void)
{
	static const Arm64Config config = {.reset_pc = RESET_PC, .highest_el = 3};
	Arm64Cpu *c[4];
	Machine *m = arm64_test_machine(&c[0]);

	for (unsigned i = 1; i < 4; i++) {
		Cpu *base = arm64_cpu_create(&config);

		CHECK(machine_add_cpu(m, base));
		c[i] = arm64_cpu_of(base);
	}

	/* CPU 1 waits in WFE, CPU 2 in WFI, CPU 3 is off. */
	CHECK_EQ(arm64_execute(c[1], 0xd503205f), ARM64_EXEC_OK); /* WFE */
	CHECK_EQ(arm64_execute(c[2], 0xd503207f), ARM64_EXEC_OK); /* WFI */
	c[3]->cpu.state = CPU_OFF;

	CHECK_EQ(arm64_execute(c[0], 0xd503209f), ARM64_EXEC_OK); /* SEV */
	CHECK(c[0]->event_register);

	/* The WFE completes, with the Event Register clear. */
	CHECK_EQ(c[1]->cpu.state, CPU_RUNNING);
	CHECK(!c[1]->event_register);

	/* SEV does not wake WFI, but a later WFE will not wait. */
	CHECK_EQ(c[2]->cpu.state, CPU_HALTED);
	CHECK(c[2]->event_register);

	CHECK_EQ(c[3]->cpu.state, CPU_OFF);
	CHECK(!c[3]->event_register);
	machine_destroy(m);
}

static void test_barriers(void)
{
	static const uint32_t insns[] = {
		0xd5033bbf, /* DMB ISH */
		0xd5033f9f, /* DSB SY */
		0xd5033fdf, /* ISB */
		0xd503309f, /* SSBB */
		0xd503349f, /* PSSBB */
	};
	Arm64Cpu *c;
	Machine *m = arm64_test_machine(&c);

	for (unsigned i = 0; i < ARRAY_SIZE(insns); i++)
		CHECK_EQ(jump(m, c, insns[i]), 4);
	machine_destroy(m);
}

static void test_undefined(void)
{
	static const uint32_t insns[] = {
		0x54000050, /* BC.EQ . + 8 (FEAT_HBC) */
		0xf4010040, /* CBGT X0, X1, . + 8 (FEAT_CMPBR) */
		0x5500001f, /* RETAASPPC, from the table in C4.1 (FEAT_PAuth_LR) */
		0xd71f0801, /* BRAA X0, X1 (FEAT_PAuth) */
		0xd50330ff, /* SB (FEAT_SB) */
		0xd5033e3f, /* DSB SYNXS (FEAT_XS) */
		0xd5031000, /* WFET X0 (FEAT_WFxT) */
		0xd61f0021, /* BR X1 with op4 = 00001: UNALLOCATED */
		0xd6bf03e0, /* DRPS, outside Debug state */
		0xd503301f, /* barrier with op2 = 000: UNALLOCATED */
		0xd5033f9e, /* DSB SY with Rt = 30: UNALLOCATED */
		0xd5782000, /* MRRS X0, X1, TTBR0_EL1 (FEAT_SYSREG128) */
		0xd503201e, /* hint with Rt = 30: HINTE (FEAT_HINTE) */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

static void test_not_implemented_yet(void)
{
	static const uint32_t insns[] = {
		0xd4000001, /* SVC #0 */
		0xd4200000, /* BRK #0 */
		0xd4000002, /* HVC #0 */
		0xd4000003, /* SMC #0 */
		0xd69f03e0, /* ERET */
		0xd5384240, /* MRS X0, CurrentEL */
		0xd50342df, /* MSR DAIFSet, #2 */
		0xd50b7e20, /* DC CIVAC, X0 */
		0xd508871f, /* TLBI VMALLE1 */
		0xd5033f5f, /* CLREX */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(insns); i++) {
		Arm64Cpu *c;
		Machine *m = arm64_test_machine(&c);

		CHECK_EQ(arm64_test_step(m, c, insns[i]), MACHINE_STOP_ERROR);
		CHECK(strstr(m->stop_message,
		             "is not implemented yet (Branches, Exception "
		             "Generating and System instructions)") != NULL);
		machine_destroy(m);
	}
}

int main(void)
{
	RUN_TEST(test_b_and_bl);
	RUN_TEST(test_conditional_branch_all_conditions);
	RUN_TEST(test_compare_then_branch);
	RUN_TEST(test_cbz_and_cbnz);
	RUN_TEST(test_tbz_and_tbnz);
	RUN_TEST(test_branch_to_register);
	RUN_TEST(test_loop_and_call);
	RUN_TEST(test_hints_without_a_feature_are_nops);
	RUN_TEST(test_wfi_waits);
	RUN_TEST(test_wfe_and_the_event_register);
	RUN_TEST(test_sev_wakes_other_cpus);
	RUN_TEST(test_barriers);
	RUN_TEST(test_undefined);
	RUN_TEST(test_not_implemented_yet);
	return test_summary();
}
