/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for src/cpu/arm64/pmu.c: the Performance Monitors of the
 * Cortex-A53, their values, access rules and traps (D24.5), and what stops
 * the machine.
 */

#include <stdint.h>
#include <string.h>

#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "test.h"
#include "util/macros.h"

#define EC_SYS 0x18 /* a trapped MSR, MRS or System instruction */

static uint32_t mrs(uint16_t encoding, unsigned t)
{
	return 0xd5200000u | (uint32_t)encoding << 5 | t;
}

static uint32_t msr(uint16_t encoding, unsigned t)
{
	return 0xd5000000u | (uint32_t)encoding << 5 | t;
}

/* A Cortex-A53 of the BCM2837 at EL3, outside any machine. */
static Arm64Cpu *make_a53(void)
{
	static const Arm64Config config = {
		.reset_pc = 0x8000,
		.highest_el = 3,
		.model = &arm64_cortex_a53_bcm2837,
	};
	Cpu *base = arm64_cpu_create(&config);

	CHECK(base != NULL);
	return arm64_cpu_of(base);
}

static void destroy(Arm64Cpu *c)
{
	c->cpu.ops->destroy(&c->cpu);
}

static uint64_t rd(Arm64Cpu *c, uint16_t encoding)
{
	c->x[0] = 0xdeadbeef;
	CHECK_EQ(arm64_execute(c, mrs(encoding, 0)), ARM64_EXEC_OK);
	return c->x[0];
}

static void wr(Arm64Cpu *c, uint16_t encoding, uint64_t value)
{
	c->x[1] = value;
	CHECK_EQ(arm64_execute(c, msr(encoding, 1)), ARM64_EXEC_OK);
}

/* Run insn, which must trap: the Exception level it went to. */
static unsigned trapped_to(Arm64Cpu *c, uint32_t insn)
{
	unsigned from = c->el, to;

	CHECK_EQ(arm64_execute(c, insn), ARM64_EXEC_EXCEPTION);
	arm64_take_exception(c);
	to = c->el;
	CHECK_EQ(c->sys.esr[to] >> 26, EC_SYS);
	c->el = from;
	return to;
}

/* Run insn, which must stop the machine with a detail that has what. */
static void stops(Arm64Cpu *c, uint32_t insn, const char *what)
{
	c->detail[0] = '\0';
	CHECK_EQ(arm64_execute(c, insn), ARM64_EXEC_UNIMPLEMENTED);
	if (strstr(c->detail, what) == NULL) {
		fprintf(stderr, "    detail \"%s\", expected \"%s\"\n", c->detail,
		        what);
		CHECK(false);
	}
}

/* EL2 enabled: Non-secure, AArch64 below EL3 and EL2. */
static void enable_el2(Arm64Cpu *c)
{
	c->sys.scr_el3 = ARM64_SCR_NS | ARM64_SCR_RW;
	c->sys.hcr_el2 = ARM64_HCR_RW;
}

/* The Cortex-A53's values, section 12.4, at reset. */
static void test_reset_values(void)
{
	Arm64Cpu *c = make_a53();

	CHECK_EQ(rd(c, ARM64_PMCR_EL0), 0x41033000);
	CHECK_EQ(rd(c, ARM64_PMCEID0_EL0), 0x67ffbfff);
	CHECK_EQ(rd(c, ARM64_PMCEID1_EL0), 0);
	CHECK_EQ(rd(c, ARM64_PMUSERENR_EL0), 0);
	CHECK_EQ(rd(c, ARM64_PMCCFILTR_EL0), 0);
	CHECK_EQ(rd(c, ARM64_PMCNTENSET_EL0), 0);

	/* A reset brings them back. */
	wr(c, ARM64_PMCR_EL0, 0x78);
	wr(c, ARM64_PMUSERENR_EL0, 0xf);
	wr(c, ARM64_PMEVCNTR_EL0(2), 7);
	c->cpu.ops->reset(&c->cpu);
	CHECK_EQ(rd(c, ARM64_PMCR_EL0), 0x41033000);
	CHECK_EQ(rd(c, ARM64_PMUSERENR_EL0), 0);
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(2)), 0);
	destroy(c);
}

/* What each register keeps, and the PMCR_EL0 resets. */
static void test_registers(void)
{
	Arm64Cpu *c = make_a53();

	/* PMCR_EL0: D, X, DP and LC stay; IMP, IDCODE, N do not change. */
	wr(c, ARM64_PMCR_EL0, 0xffffff78);
	CHECK_EQ(rd(c, ARM64_PMCR_EL0), 0x41033078);
	wr(c, ARM64_PMCR_EL0, 0);

	/* Set and clear pairs, over counters 0 to 5 and the cycle counter. */
	static const uint16_t pairs[][2] = {
		{ARM64_PMCNTENSET_EL0, ARM64_PMCNTENCLR_EL0},
		{ARM64_PMOVSSET_EL0, ARM64_PMOVSCLR_EL0},
		{ARM64_PMINTENSET_EL1, ARM64_PMINTENCLR_EL1},
	};
	for (size_t i = 0; i < 3; i++) {
		wr(c, pairs[i][0], 0xffffffffffffffff);
		CHECK_EQ(rd(c, pairs[i][0]), 0x8000003f);
		CHECK_EQ(rd(c, pairs[i][1]), 0x8000003f);
		wr(c, pairs[i][1], 0x80000005);
		CHECK_EQ(rd(c, pairs[i][0]), 0x3a);
		wr(c, pairs[i][1], 0xffffffff);
		CHECK_EQ(rd(c, pairs[i][1]), 0);
	}

	/* The counters: event counters of 32 bits, the cycle counter of 64. */
	wr(c, ARM64_PMCCNTR_EL0, 0x123456789abcdef0);
	CHECK_EQ(rd(c, ARM64_PMCCNTR_EL0), 0x123456789abcdef0);
	for (unsigned n = 0; n < 6; n++) {
		wr(c, ARM64_PMEVCNTR_EL0(n), 0xff00000000000000 | (n + 1));
		wr(c, ARM64_PMEVTYPER_EL0(n), 0xffffffff);
	}
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(5)), 6);
	CHECK_EQ(rd(c, ARM64_PMEVTYPER_EL0(5)), 0xfc0003ff);
	wr(c, ARM64_PMCCFILTR_EL0, 0xffffffff);
	CHECK_EQ(rd(c, ARM64_PMCCFILTR_EL0), 0xfc000000);
	wr(c, ARM64_PMUSERENR_EL0, 0xffffffff);
	CHECK_EQ(rd(c, ARM64_PMUSERENR_EL0), 0xf);

	/* PMSELR_EL0 picks the counter of PMXEVCNTR and PMXEVTYPER. */
	wr(c, ARM64_PMSELR_EL0, 0xffffffe3);
	CHECK_EQ(rd(c, ARM64_PMSELR_EL0), 3);
	CHECK_EQ(rd(c, ARM64_PMXEVCNTR_EL0), 4);
	wr(c, ARM64_PMXEVCNTR_EL0, 0x1234567890);
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(3)), 0x34567890);
	wr(c, ARM64_PMXEVTYPER_EL0, 0x11);
	CHECK_EQ(rd(c, ARM64_PMEVTYPER_EL0(3)), 0x11);
	CHECK_EQ(rd(c, ARM64_PMXEVTYPER_EL0), 0x11);
	wr(c, ARM64_PMSELR_EL0, 31); /* PMXEVTYPER_EL0 is PMCCFILTR_EL0 */
	wr(c, ARM64_PMCCFILTR_EL0, 0);
	wr(c, ARM64_PMXEVTYPER_EL0, 0xffffffff);
	CHECK_EQ(rd(c, ARM64_PMCCFILTR_EL0), 0xfc000000);
	CHECK_EQ(rd(c, ARM64_PMEVTYPER_EL0(3)), 0x11);
	wr(c, ARM64_PMCCFILTR_EL0, 0x40000000);
	CHECK_EQ(rd(c, ARM64_PMXEVTYPER_EL0), 0x40000000);

	/* A write without P or C keeps the counters. */
	wr(c, ARM64_PMCR_EL0, 0x78);
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(5)), 6);
	CHECK_EQ(rd(c, ARM64_PMCCNTR_EL0), 0x123456789abcdef0);

	/* P resets the event counters, C the cycle counter; both read 0. */
	wr(c, ARM64_PMCR_EL0, 2);
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(0)), 0);
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(5)), 0);
	CHECK_EQ(rd(c, ARM64_PMCCNTR_EL0), 0x123456789abcdef0);
	wr(c, ARM64_PMCR_EL0, 4);
	CHECK_EQ(rd(c, ARM64_PMCCNTR_EL0), 0);
	CHECK_EQ(rd(c, ARM64_PMCR_EL0), 0x41033000);

	/* PMSWINC_EL0 counts nothing while no counter is on. */
	wr(c, ARM64_PMSWINC_EL0, 0xffffffff);
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(0)), 0);

	/* Write-only and read-only registers. */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_PMSWINC_EL0, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, msr(ARM64_PMCEID0_EL0, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, msr(ARM64_PMCEID1_EL0, 0)), ARM64_EXEC_UNDEFINED);
	/* PMEVTYPER<31> is PMCCFILTR; PMEVCNTR<31> is not a register. */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_PMEVCNTR_EL0(31), 0)),
	         ARM64_EXEC_UNDEFINED);
	destroy(c);
}

/* EL0 needs PMUSERENR_EL0: EN, or SW, CR or ER for some accesses. */
static void test_el0(void)
{
	/* Each access, and the PMUSERENR_EL0 bits that allow it. */
	const struct {
		uint32_t insn, enables;
	} list[] = {
		{mrs(ARM64_PMCR_EL0, 0), 1},
		{msr(ARM64_PMCR_EL0, 31), 1},
		{mrs(ARM64_PMCNTENSET_EL0, 0), 1},
		{msr(ARM64_PMOVSCLR_EL0, 31), 1},
		{mrs(ARM64_PMCEID0_EL0, 0), 1},
		{msr(ARM64_PMSWINC_EL0, 31), 1 | 2},
		{mrs(ARM64_PMCCNTR_EL0, 0), 1 | 4},
		{msr(ARM64_PMCCNTR_EL0, 31), 1},
		{mrs(ARM64_PMEVCNTR_EL0(1), 0), 1 | 8},
		{msr(ARM64_PMEVCNTR_EL0(1), 31), 1},
		{mrs(ARM64_PMXEVCNTR_EL0, 0), 1 | 8},
		{msr(ARM64_PMXEVCNTR_EL0, 31), 1},
		{mrs(ARM64_PMSELR_EL0, 0), 1 | 8},
		{msr(ARM64_PMSELR_EL0, 31), 1 | 8},
		{mrs(ARM64_PMEVTYPER_EL0(1), 0), 1},
		{mrs(ARM64_PMCCFILTR_EL0, 0), 1},
	};
	Arm64Cpu *c = make_a53();

	enable_el2(c);
	c->el = 0;
	for (size_t i = 0; i < ARRAY_SIZE(list); i++) {
		for (uint32_t bit = 1; bit <= 8; bit <<= 1) {
			c->pmu.userenr = bit;
			if (list[i].enables & bit)
				CHECK_EQ(arm64_execute(c, list[i].insn), ARM64_EXEC_OK);
			else
				CHECK_EQ(trapped_to(c, list[i].insn), 1);
		}
	}
	/* HCR_EL2.TGE takes them to EL2 instead. */
	c->pmu.userenr = 0;
	c->sys.hcr_el2 |= ARM64_HCR_TGE;
	CHECK_EQ(trapped_to(c, mrs(ARM64_PMCR_EL0, 0)), 2);
	c->sys.hcr_el2 &= ~ARM64_HCR_TGE;

	/* PMUSERENR_EL0 reads without EN, and does not write; EL1's do not. */
	c->pmu.userenr = 0;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_PMUSERENR_EL0, 0)), ARM64_EXEC_OK);
	c->pmu.userenr = 0xf;
	CHECK_EQ(arm64_execute(c, msr(ARM64_PMUSERENR_EL0, 0)),
	         ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_PMINTENSET_EL1, 0)),
	         ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, msr(ARM64_PMINTENCLR_EL1, 0)),
	         ARM64_EXEC_UNDEFINED);
	destroy(c);
}

/* MDCR_EL2.TPM and TPMCR, and MDCR_EL3.TPM. */
static void test_traps(void)
{
	Arm64Cpu *c = make_a53();
	uint32_t pmcr = mrs(ARM64_PMCR_EL0, 0);
	uint32_t cnten = msr(ARM64_PMCNTENCLR_EL0, 31);
	uint32_t userenr = msr(ARM64_PMUSERENR_EL0, 31);

	enable_el2(c);
	c->pmu.userenr = 1;
	for (unsigned el = 0; el <= 1; el++) {
		c->el = el;
		c->sys.mdcr_el2 = 6 | 1u << 6; /* TPM */
		CHECK_EQ(trapped_to(c, pmcr), 2);
		CHECK_EQ(trapped_to(c, cnten), 2);
		CHECK_EQ(trapped_to(c, mrs(ARM64_PMUSERENR_EL0, 0)), 2);
		c->sys.mdcr_el2 = 6 | 1u << 5; /* TPMCR: PMCR_EL0 alone */
		CHECK_EQ(trapped_to(c, pmcr), 2);
		CHECK_EQ(arm64_execute(c, cnten), ARM64_EXEC_OK);
		c->sys.mdcr_el2 = 6;
		c->sys.mdcr_el3 = 1u << 6; /* TPM */
		CHECK_EQ(trapped_to(c, pmcr), 3);
		CHECK_EQ(trapped_to(c, mrs(ARM64_PMCEID1_EL0, 0)), 3);
		c->sys.mdcr_el3 = 0;
	}
	c->el = 1;
	CHECK_EQ(arm64_execute(c, userenr), ARM64_EXEC_OK);
	c->sys.mdcr_el3 = 1u << 6;
	CHECK_EQ(trapped_to(c, userenr), 3);
	CHECK_EQ(trapped_to(c, mrs(ARM64_PMINTENSET_EL1, 0)), 3);

	/* EL2 traps to EL3 alone; MDCR_EL2 does not trap it. */
	c->el = 2;
	c->sys.mdcr_el2 = 6 | 3u << 5;
	CHECK_EQ(trapped_to(c, pmcr), 3);
	c->sys.mdcr_el3 = 0;
	CHECK_EQ(arm64_execute(c, pmcr), ARM64_EXEC_OK);
	/* EL3 is never trapped. */
	c->el = 3;
	c->sys.mdcr_el3 = 1u << 6;
	CHECK_EQ(arm64_execute(c, pmcr), ARM64_EXEC_OK);

	/* Without EL2 enabled, MDCR_EL2 traps nothing. */
	c->sys.mdcr_el3 = 0;
	c->sys.scr_el3 = ARM64_SCR_RW;
	c->el = 1;
	CHECK_EQ(arm64_execute(c, pmcr), ARM64_EXEC_OK);
	destroy(c);
}

/* MDCR_EL2.HPMN splits the counters: EL0 and EL1 reach the first range. */
static void test_hpmn(void)
{
	Arm64Cpu *c = make_a53();

	enable_el2(c);
	c->el = 2;
	wr(c, ARM64_MDCR_EL2, 4);
	for (unsigned n = 0; n < 6; n++)
		wr(c, ARM64_PMEVCNTR_EL0(n), 10 + n);
	wr(c, ARM64_PMOVSSET_EL0, 0x30);

	c->el = 1;
	CHECK_EQ(rd(c, ARM64_PMCR_EL0), 0x41032000); /* N is HPMN, 4 */
	CHECK_EQ(rd(c, ARM64_PMOVSSET_EL0), 0);
	wr(c, ARM64_PMOVSSET_EL0, 0xffffffff);
	CHECK_EQ(rd(c, ARM64_PMOVSCLR_EL0), 0x8000000f);
	wr(c, ARM64_PMOVSCLR_EL0, 0xffffffff); /* bits 4 and 5 stay */
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(3)), 13);
	stops(c, mrs(ARM64_PMEVCNTR_EL0(4), 0),
	      "event counter 4, past MDCR_EL2.HPMN");
	stops(c, msr(ARM64_PMEVTYPER_EL0(5), 0), "past MDCR_EL2.HPMN");
	wr(c, ARM64_PMSELR_EL0, 4);
	stops(c, mrs(ARM64_PMXEVCNTR_EL0, 0), "past MDCR_EL2.HPMN");
	stops(c, mrs(ARM64_PMXEVTYPER_EL0, 0), "past MDCR_EL2.HPMN");
	wr(c, ARM64_PMSELR_EL0, 31);
	CHECK_EQ(rd(c, ARM64_PMXEVTYPER_EL0), 0);
	stops(c, mrs(ARM64_PMXEVCNTR_EL0, 0), "event counter 31, past PMCR_EL0.N");
	/* P at EL1 resets the first range alone. */
	wr(c, ARM64_PMCR_EL0, 2);
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(3)), 0);

	c->el = 2;
	CHECK_EQ(rd(c, ARM64_PMCR_EL0), 0x41033000);
	CHECK_EQ(rd(c, ARM64_PMOVSSET_EL0), 0x30);
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(4)), 14);
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(5)), 15);
	wr(c, ARM64_PMCR_EL0, 2); /* at EL2, both ranges */
	CHECK_EQ(rd(c, ARM64_PMEVCNTR_EL0(5)), 0);

	/* Past N at any level, and the reserved values of HPMN. */
	c->el = 3;
	stops(c, mrs(ARM64_PMEVCNTR_EL0(6), 0), "event counter 6, past PMCR_EL0.N");
	stops(c, mrs(ARM64_PMEVTYPER_EL0(30), 0), "event counter 30");
	stops(c, msr(ARM64_PMEVCNTR_EL0(30), 0), "event counter 30");
	c->x[1] = 0;
	stops(c, msr(ARM64_MDCR_EL2, 1), "MDCR_EL2.HPMN of 0, reserved");
	c->x[1] = 7;
	stops(c, msr(ARM64_MDCR_EL2, 1), "MDCR_EL2.HPMN of 7, reserved");
	destroy(c);
}

/* Turning a counter on stops the machine: counting is not emulated. */
static void test_counting_stops(void)
{
	Arm64Cpu *c = make_a53();

	/* Enabled counters with E off, and E on with none enabled. */
	wr(c, ARM64_PMCNTENSET_EL0, 0x8000003f);
	wr(c, ARM64_PMCNTENCLR_EL0, 0xffffffff);
	wr(c, ARM64_PMCR_EL0, 1);
	/* The cycle counter, then an event counter. */
	c->x[1] = 1u << 31;
	stops(c, msr(ARM64_PMCNTENSET_EL0, 1), "counting with the Performance");
	c->pmu.cnten = 0;
	c->x[1] = 1;
	stops(c, msr(ARM64_PMCNTENSET_EL0, 1), "counting with the Performance");
	/* E with a counter already enabled. */
	c->pmu.pmcr = 0;
	c->pmu.cnten = 2;
	c->x[1] = 1;
	stops(c, msr(ARM64_PMCR_EL0, 1), "counting with the Performance");

	/*
	 * MDCR_EL2.HPME turns on the second range alone: counter 5 with
	 * HPMN 5, not counter 4.
	 */
	c->pmu.pmcr = 0;
	c->pmu.cnten = 1u << 4;
	wr(c, ARM64_MDCR_EL2, 5 | 1u << 7);
	c->x[1] = 1u << 5;
	stops(c, msr(ARM64_PMCNTENSET_EL0, 1), "counting with the Performance");
	c->pmu.cnten = 0;
	c->x[1] = 5 | 1u << 7;
	CHECK_EQ(arm64_execute(c, msr(ARM64_MDCR_EL2, 1)), ARM64_EXEC_OK);
	c->pmu.cnten = 1u << 5;
	stops(c, msr(ARM64_MDCR_EL2, 1), "counting with the Performance");
	/* Counter 0 is of the first range; without HPME, the second is off. */
	c->pmu.cnten = 1;
	CHECK_EQ(arm64_execute(c, msr(ARM64_MDCR_EL2, 1)), ARM64_EXEC_OK);
	c->pmu.cnten = 1u << 5;
	c->x[1] = 5;
	CHECK_EQ(arm64_execute(c, msr(ARM64_MDCR_EL2, 1)), ARM64_EXEC_OK);
	destroy(c);
}

/* As a debugger: no checks, but no counter past N; names. */
static void test_direct(void)
{
	Arm64Cpu *c = make_a53();
	uint64_t value;

	CHECK(arm64_cpu_write_sysreg(c, ARM64_PMEVCNTR_EL0(5), 9));
	CHECK(arm64_cpu_read_sysreg(c, ARM64_PMEVCNTR_EL0(5), &value));
	CHECK_EQ(value, 9);
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_PMEVCNTR_EL0(6), &value));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_PMEVTYPER_EL0(6), 0));
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_PMSWINC_EL0, &value));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_PMCEID0_EL0, 0));
	CHECK(arm64_cpu_write_sysreg(c, ARM64_PMSELR_EL0, 6));
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_PMXEVCNTR_EL0, &value));
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_PMXEVTYPER_EL0, &value));
	CHECK(arm64_cpu_write_sysreg(c, ARM64_PMSELR_EL0, 31));
	CHECK(arm64_cpu_read_sysreg(c, ARM64_PMXEVTYPER_EL0, &value));
	CHECK(strcmp(arm64_sysreg_name(c, ARM64_PMCR_EL0), "PMCR_EL0") == 0);
	CHECK(strcmp(arm64_sysreg_name(c, ARM64_PMEVCNTR_EL0(3)),
	             "PMEVCNTR<n>_EL0") == 0);
	CHECK(strcmp(arm64_sysreg_name(c, ARM64_PMEVTYPER_EL0(0)),
	             "PMEVTYPER<n>_EL0") == 0);
	CHECK(strcmp(arm64_sysreg_name(c, ARM64_PMCCFILTR_EL0), "PMCCFILTR_EL0") ==
	      0);
	destroy(c);

	/* A core without the PMU has none of it. */
	static const Arm64Config bare = {.reset_pc = 0, .highest_el = 3};
	c = arm64_cpu_of(arm64_cpu_create(&bare));
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_PMCR_EL0, &value));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_PMCCNTR_EL0, 0));
	CHECK_EQ(arm64_execute(c, mrs(ARM64_PMCCNTR_EL0, 0)), ARM64_EXEC_UNDEFINED);
	c->x[1] = 0; /* HPMN 0 matters only with a PMU */
	CHECK_EQ(arm64_execute(c, msr(ARM64_MDCR_EL2, 1)), ARM64_EXEC_OK);
	destroy(c);
}

/*
 * A write changes its own register and no other, but for those that
 * share one: the set and clear pairs, and PMXEV* with PMEV*<SEL>.
 */
static void test_independent(void)
{
	static const struct {
		uint16_t encoding;
		uint64_t value;
		uint16_t also; /* a register that shares its value, or 0 */
	} regs[] = {
		{ARM64_PMCR_EL0, 0x78, 0},
		{ARM64_PMCNTENSET_EL0, 0x15, ARM64_PMCNTENCLR_EL0},
		{ARM64_PMCNTENCLR_EL0, 0x4, ARM64_PMCNTENSET_EL0},
		{ARM64_PMOVSSET_EL0, 0x80000003, ARM64_PMOVSCLR_EL0},
		{ARM64_PMOVSCLR_EL0, 0x1, ARM64_PMOVSSET_EL0},
		{ARM64_PMINTENSET_EL1, 0x2a, ARM64_PMINTENCLR_EL1},
		{ARM64_PMINTENCLR_EL1, 0x8, ARM64_PMINTENSET_EL1},
		{ARM64_PMSELR_EL0, 2, 0},
		{ARM64_PMCCNTR_EL0, 0x55, 0},
		{ARM64_PMXEVTYPER_EL0, 0x13, ARM64_PMEVTYPER_EL0(2)},
		{ARM64_PMXEVCNTR_EL0, 0x66, ARM64_PMEVCNTR_EL0(2)},
		{ARM64_PMUSERENR_EL0, 0xfc000005, 0}, /* PMCCFILTR's bits too */
		{ARM64_PMSWINC_EL0, 0x1d, 0},
		{ARM64_PMCCFILTR_EL0, 0x80000000, 0},
		{ARM64_PMEVCNTR_EL0(0), 0x70, 0},
		{ARM64_PMEVCNTR_EL0(2), 0x72, ARM64_PMXEVCNTR_EL0},
		{ARM64_PMEVCNTR_EL0(5), 0x75, 0},
		{ARM64_PMEVTYPER_EL0(0), 0x20, 0},
		{ARM64_PMEVTYPER_EL0(2), 0x22, ARM64_PMXEVTYPER_EL0},
		{ARM64_PMEVTYPER_EL0(5), 0x25, 0},
	};
	Arm64Cpu *c = make_a53();
	uint64_t before[ARRAY_SIZE(regs)], after;

	wr(c, ARM64_PMSELR_EL0, 2);
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		for (size_t j = 0; j < ARRAY_SIZE(regs); j++)
			if (regs[j].encoding != ARM64_PMSWINC_EL0)
				CHECK(arm64_cpu_read_sysreg(c, regs[j].encoding, &before[j]));
		wr(c, regs[i].encoding, regs[i].value);
		for (size_t j = 0; j < ARRAY_SIZE(regs); j++) {
			if (j == i || regs[j].encoding == regs[i].also ||
			    regs[j].also == regs[i].encoding ||
			    regs[j].encoding == ARM64_PMSWINC_EL0)
				continue;
			CHECK(arm64_cpu_read_sysreg(c, regs[j].encoding, &after));
			if (after != before[j]) {
				fprintf(stderr, "    writing %s changed %s\n",
				        arm64_sysreg_name(c, regs[i].encoding),
				        arm64_sysreg_name(c, regs[j].encoding));
				CHECK(false);
			}
		}
	}

	/* X16 to X30 work too. */
	c->x[17] = 0x1234;
	CHECK_EQ(arm64_execute(c, msr(ARM64_PMCCNTR_EL0, 17)), ARM64_EXEC_OK);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_PMCCNTR_EL0, 25)), ARM64_EXEC_OK);
	CHECK_EQ(c->x[25], 0x1234);
	destroy(c);
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_registers);
	RUN_TEST(test_el0);
	RUN_TEST(test_traps);
	RUN_TEST(test_hpmn);
	RUN_TEST(test_counting_stops);
	RUN_TEST(test_direct);
	RUN_TEST(test_independent);
	return test_summary();
}
