/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for src/cpu/arm64/debug.c: the self-hosted debug registers
 * of the Cortex-A53, their values, access rules and traps (D24.3), and
 * what stops the machine.
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

/* Write value with MSR, which must stop with a detail that has what. */
static void stops(Arm64Cpu *c, uint32_t insn, uint64_t value, const char *what)
{
	c->detail[0] = '\0';
	c->x[1] = value;
	CHECK_EQ(arm64_execute(c, insn), ARM64_EXEC_UNIMPLEMENTED);
	if (strstr(c->detail, what) == NULL) {
		fprintf(stderr, "    detail \"%s\", expected \"%s\"\n", c->detail,
		        what);
		CHECK(false);
	}
}

/* The values at reset, and what each register keeps. */
static void test_registers(void)
{
	Arm64Cpu *c = make_a53();

	CHECK_EQ(rd(c, ARM64_OSLSR_EL1), 0xa); /* the OS Lock locked */
	CHECK_EQ(rd(c, ARM64_OSDLR_EL1), 0);
	CHECK_EQ(rd(c, ARM64_MDSCR_EL1), 0);

	/* 6 breakpoints and 4 watchpoints, as ID_AA64DFR0_EL1 says. */
	for (unsigned n = 0; n < 6; n++) {
		CHECK_EQ(rd(c, ARM64_DBGBVR_EL1(n)), 0);
		wr(c, ARM64_DBGBVR_EL1(n), 0xffffffffffffffff - n);
		wr(c, ARM64_DBGBCR_EL1(n), 0xfffffffe);
		CHECK_EQ(rd(c, ARM64_DBGBVR_EL1(n)), (0xffffffffffffffff - n) & ~3ull);
		CHECK_EQ(rd(c, ARM64_DBGBCR_EL1(n)), 0x00ffe1e6);
	}
	for (unsigned n = 0; n < 4; n++) {
		wr(c, ARM64_DBGWVR_EL1(n), 0x123456789abcdef3);
		wr(c, ARM64_DBGWCR_EL1(n), 0xfffffffe);
		CHECK_EQ(rd(c, ARM64_DBGWVR_EL1(n)), 0x123456789abcdef0);
		CHECK_EQ(rd(c, ARM64_DBGWCR_EL1(n)), 0x1f1ffffe);
	}
	CHECK_EQ(arm64_execute(c, mrs(ARM64_DBGBVR_EL1(6), 0)),
	         ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, msr(ARM64_DBGBCR_EL1(15), 0)),
	         ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_DBGWVR_EL1(4), 0)),
	         ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, msr(ARM64_DBGWCR_EL1(4), 0)),
	         ARM64_EXEC_UNDEFINED);
	/* op2 3, below the pairs' 4 to 7, has no register. */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(2, 0, 0, 0, 3), 0)),
	         ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, msr(ARM64_SYSREG(2, 0, 0, 5, 3), 0)),
	         ARM64_EXEC_UNDEFINED);

	/* The OS Lock and the OS Double Lock. */
	wr(c, ARM64_OSLAR_EL1, 0xfffffffe);
	CHECK_EQ(rd(c, ARM64_OSLSR_EL1), 0x8);
	wr(c, ARM64_OSLAR_EL1, 1);
	CHECK_EQ(rd(c, ARM64_OSLSR_EL1), 0xa);
	wr(c, ARM64_OSDLR_EL1, 0xffffffff);
	CHECK_EQ(rd(c, ARM64_OSDLR_EL1), 1);
	wr(c, ARM64_OSDLR_EL1, 2); /* DLK is bit 0 alone */
	CHECK_EQ(rd(c, ARM64_OSDLR_EL1), 0);
	wr(c, ARM64_OSDLR_EL1, 1);
	CHECK_EQ(rd(c, ARM64_OSDLR_EL1), 1);
	wr(c, ARM64_OSDLR_EL1, 0);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_OSLAR_EL1, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, msr(ARM64_OSLSR_EL1, 0)), ARM64_EXEC_UNDEFINED);

	/*
	 * MDSCR_EL1: the EDSCR fields take a write while the OS Lock is
	 * locked, for a restore, and keep their value when it is not.
	 */
	wr(c, ARM64_MDSCR_EL1, 0xfffffffe);
	CHECK_EQ(rd(c, ARM64_MDSCR_EL1), 0x6ce0f040);
	wr(c, ARM64_MDSCR_EL1, 0);
	CHECK_EQ(rd(c, ARM64_MDSCR_EL1), 0);
	wr(c, ARM64_MDSCR_EL1, 0x6cc00040); /* EDSCR fields but HDE and TDA */
	wr(c, ARM64_OSLAR_EL1, 0);
	wr(c, ARM64_MDSCR_EL1, 0x0000b000);
	CHECK_EQ(rd(c, ARM64_MDSCR_EL1), 0x6cc0b040);
	wr(c, ARM64_MDSCR_EL1, 0);
	CHECK_EQ(rd(c, ARM64_MDSCR_EL1), 0x6cc00040);

	/* A reset is a Cold reset: the OS Lock locked again. */
	c->cpu.ops->reset(&c->cpu);
	CHECK_EQ(rd(c, ARM64_OSLSR_EL1), 0xa);
	CHECK_EQ(rd(c, ARM64_MDSCR_EL1), 0);
	CHECK_EQ(rd(c, ARM64_DBGWCR_EL1(3)), 0);
	destroy(c);
}

/* What software could do that is not emulated stops the machine. */
static void test_stops(void)
{
	Arm64Cpu *c = make_a53();
	uint32_t mdscr = msr(ARM64_MDSCR_EL1, 1);

	stops(c, mdscr, 1, "software step, with MDSCR_EL1.SS");
	c->debug.mdscr = 0;

	/* MDE with an enabled breakpoint or watchpoint, either way round. */
	wr(c, ARM64_DBGBCR_EL1(0), 1);
	stops(c, mdscr, 1u << 15, "breakpoints and watchpoints");
	c->debug.mdscr = 0;
	wr(c, ARM64_DBGBCR_EL1(0), 0);
	wr(c, ARM64_DBGBCR_EL1(5), 1);
	wr(c, ARM64_MDSCR_EL1, 1u << 13); /* KDE alone */
	stops(c, mdscr, 1u << 15, "breakpoints and watchpoints");
	c->debug.mdscr = 0;
	wr(c, ARM64_DBGBCR_EL1(5), 0);
	wr(c, ARM64_MDSCR_EL1, 1u << 15);
	stops(c, msr(ARM64_DBGWCR_EL1(3), 1), 0x1f1fffff,
	      "breakpoints and watchpoints");
	c->debug.wcr[3] = 0;
	wr(c, ARM64_DBGWCR_EL1(3), 0x1f1ffffe); /* not enabled */
	wr(c, ARM64_MDSCR_EL1, 0);

	/*
	 * EDSCR.HDE, set for a restore, stops once the OS Lock unlocks;
	 * EDSCR.TDA stops at an access to a breakpoint or watchpoint.
	 */
	wr(c, ARM64_OSLAR_EL1, 0); /* unlocked, no TDA: no halt */
	wr(c, ARM64_DBGBVR_EL1(1), 0x40);
	CHECK_EQ(rd(c, ARM64_DBGBVR_EL1(1)), 0x40);
	wr(c, ARM64_OSLAR_EL1, 1);
	wr(c, ARM64_MDSCR_EL1, 1u << 14);
	stops(c, msr(ARM64_OSLAR_EL1, 1), 0, "halting, with EDSCR.HDE");
	c->debug.oslk = true;
	wr(c, ARM64_MDSCR_EL1, 1u << 21);
	CHECK_EQ(rd(c, ARM64_DBGBVR_EL1(0)), 0); /* locked: no halt */
	wr(c, ARM64_OSLAR_EL1, 0);
	c->detail[0] = '\0';
	CHECK_EQ(arm64_execute(c, mrs(ARM64_DBGWVR_EL1(1), 0)),
	         ARM64_EXEC_UNIMPLEMENTED);
	CHECK(strstr(c->detail, "halting on DBGWVR<n>_EL1") != NULL);
	CHECK_EQ(rd(c, ARM64_MDSCR_EL1), 1u << 21);
	CHECK_EQ(rd(c, ARM64_OSDLR_EL1), 0);
	destroy(c);
}

/* EL0 has none; MDCR_EL2 and MDCR_EL3 trap the rest. */
static void test_traps(void)
{
	static const uint16_t tda[] = {
		ARM64_DBGBVR_EL1(0), ARM64_DBGBCR_EL1(5), ARM64_DBGWVR_EL1(3),
		ARM64_DBGWCR_EL1(0), ARM64_MDSCR_EL1,
	};
	static const uint16_t tdosa[] = {
		ARM64_OSLSR_EL1,
		ARM64_OSDLR_EL1,
	};
	Arm64Cpu *c = make_a53();

	c->sys.scr_el3 = ARM64_SCR_NS | ARM64_SCR_RW; /* EL2 enabled */
	c->sys.hcr_el2 = ARM64_HCR_RW;
	c->el = 0;
	for (size_t i = 0; i < ARRAY_SIZE(tda); i++)
		CHECK_EQ(arm64_execute(c, mrs(tda[i], 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, msr(ARM64_OSLAR_EL1, 31)), ARM64_EXEC_UNDEFINED);

	c->el = 1;
	for (size_t i = 0; i < ARRAY_SIZE(tda); i++) {
		c->sys.mdcr_el2 = 6 | 1u << 9; /* TDA */
		CHECK_EQ(trapped_to(c, mrs(tda[i], 0)), 2);
		c->sys.mdcr_el2 = 6 | 1u << 8; /* TDE */
		CHECK_EQ(trapped_to(c, msr(tda[i], 31)), 2);
		c->sys.mdcr_el2 = 6 | 1u << 10; /* TDOSA */
		CHECK_EQ(arm64_execute(c, mrs(tda[i], 0)), ARM64_EXEC_OK);
		c->sys.mdcr_el2 = 6;
		c->sys.mdcr_el3 = 1u << 9; /* TDA */
		CHECK_EQ(trapped_to(c, mrs(tda[i], 0)), 3);
		c->sys.mdcr_el3 = 0;
	}
	for (size_t i = 0; i < ARRAY_SIZE(tdosa); i++) {
		c->sys.mdcr_el2 = 6 | 1u << 10;
		CHECK_EQ(trapped_to(c, mrs(tdosa[i], 0)), 2);
		c->sys.mdcr_el2 = 6 | 1u << 8;
		CHECK_EQ(trapped_to(c, mrs(tdosa[i], 0)), 2);
		c->sys.mdcr_el2 = 6 | 1u << 9;
		CHECK_EQ(arm64_execute(c, mrs(tdosa[i], 0)), ARM64_EXEC_OK);
		c->sys.mdcr_el2 = 6;
		c->sys.mdcr_el3 = 1u << 10;
		CHECK_EQ(trapped_to(c, mrs(tdosa[i], 0)), 3);
		c->sys.mdcr_el3 = 0;
	}
	c->sys.mdcr_el2 = 6 | 1u << 10;
	CHECK_EQ(trapped_to(c, msr(ARM64_OSLAR_EL1, 31)), 2);

	/* Without EL2 enabled, MDCR_EL2 traps nothing. */
	c->sys.scr_el3 = ARM64_SCR_RW;
	c->sys.mdcr_el2 = 6 | 7u << 8;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_MDSCR_EL1, 0)), ARM64_EXEC_OK);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_OSLSR_EL1, 0)), ARM64_EXEC_OK);

	/* EL2 traps to EL3 alone; EL3 is never trapped. */
	c->sys.scr_el3 = ARM64_SCR_NS | ARM64_SCR_RW;
	c->el = 2;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_DBGBCR_EL1(0), 0)), ARM64_EXEC_OK);
	c->sys.mdcr_el3 = 3u << 9;
	CHECK_EQ(trapped_to(c, mrs(ARM64_DBGBCR_EL1(0), 0)), 3);
	CHECK_EQ(trapped_to(c, msr(ARM64_OSDLR_EL1, 31)), 3);
	c->el = 3;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_DBGBCR_EL1(0), 0)), ARM64_EXEC_OK);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_OSDLR_EL1, 0)), ARM64_EXEC_OK);
	destroy(c);
}

/* As a debugger: no checks, but no pair past the last; names. */
static void test_direct(void)
{
	Arm64Cpu *c = make_a53();
	uint64_t value;

	CHECK(arm64_cpu_write_sysreg(c, ARM64_DBGBVR_EL1(5), 0x1007));
	CHECK(arm64_cpu_read_sysreg(c, ARM64_DBGBVR_EL1(5), &value));
	CHECK_EQ(value, 0x1004);
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_DBGBVR_EL1(6), &value));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_DBGWCR_EL1(4), 0));
	CHECK(arm64_cpu_read_sysreg(c, ARM64_DBGWCR_EL1(3), &value));
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_OSLAR_EL1, &value));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_OSLSR_EL1, 0));
	CHECK(arm64_cpu_write_sysreg(c, ARM64_OSLAR_EL1, 0));
	CHECK(arm64_cpu_read_sysreg(c, ARM64_OSLSR_EL1, &value));
	CHECK_EQ(value, 0x8);
	CHECK(strcmp(arm64_sysreg_name(c, ARM64_MDSCR_EL1), "MDSCR_EL1") == 0);
	CHECK(strcmp(arm64_sysreg_name(c, ARM64_DBGWCR_EL1(2)), "DBGWCR<n>_EL1") ==
	      0);
	CHECK(strcmp(arm64_sysreg_name(c, ARM64_OSDLR_EL1), "OSDLR_EL1") == 0);
	/* MDCCINT_EL1 shares CRn 0 and is not one of them. */
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_SYSREG(2, 0, 0, 2, 0), &value));
	destroy(c);

	/* A core with no debug in its ID registers has none of them. */
	static const Arm64Config bare = {.reset_pc = 0, .highest_el = 3};
	c = arm64_cpu_of(arm64_cpu_create(&bare));
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_MDSCR_EL1, &value));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_OSLAR_EL1, 0));
	CHECK_EQ(arm64_execute(c, mrs(ARM64_OSLSR_EL1, 0)), ARM64_EXEC_UNDEFINED);
	destroy(c);
}

/* A write changes its own register and no other. */
static void test_independent(void)
{
	static const uint16_t regs[] = {
		ARM64_DBGBVR_EL1(0), ARM64_DBGBCR_EL1(0), ARM64_DBGWVR_EL1(0),
		ARM64_DBGWCR_EL1(0), ARM64_DBGBVR_EL1(5), ARM64_DBGBCR_EL1(5),
		ARM64_DBGWVR_EL1(3), ARM64_DBGWCR_EL1(3), ARM64_MDSCR_EL1,
		ARM64_OSDLR_EL1,     ARM64_OSLSR_EL1,
	};
	Arm64Cpu *c = make_a53();
	uint64_t before[ARRAY_SIZE(regs)], after;

	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		if (regs[i] == ARM64_OSLSR_EL1)
			continue; /* read-only; OSLAR_EL1 writes it */
		for (size_t j = 0; j < ARRAY_SIZE(regs); j++)
			CHECK(arm64_cpu_read_sysreg(c, regs[j], &before[j]));
		/* No E, no SS, no MDE: nothing that stops. */
		wr(c, regs[i], 0x1000 + 0x10 * i);
		for (size_t j = 0; j < ARRAY_SIZE(regs); j++) {
			if (j == i)
				continue;
			CHECK(arm64_cpu_read_sysreg(c, regs[j], &after));
			if (after != before[j]) {
				fprintf(stderr, "    writing %zu changed %zu\n", i, j);
				CHECK(false);
			}
		}
	}
	/* OSLAR_EL1 changes OSLSR_EL1 alone, either way. */
	for (uint64_t lock = 0; lock <= 1; lock++) {
		for (size_t j = 0; j < ARRAY_SIZE(regs); j++)
			CHECK(arm64_cpu_read_sysreg(c, regs[j], &before[j]));
		wr(c, ARM64_OSLAR_EL1, lock);
		for (size_t j = 0; j + 1 < ARRAY_SIZE(regs); j++) {
			CHECK(arm64_cpu_read_sysreg(c, regs[j], &after));
			CHECK_EQ(after, before[j]);
		}
	}

	/* X16 to X30 work too. */
	c->x[17] = 0x3000;
	CHECK_EQ(arm64_execute(c, msr(ARM64_DBGWVR_EL1(2), 17)), ARM64_EXEC_OK);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_DBGWVR_EL1(2), 25)), ARM64_EXEC_OK);
	CHECK_EQ(c->x[25], 0x3000);
	destroy(c);
}

int main(void)
{
	RUN_TEST(test_registers);
	RUN_TEST(test_stops);
	RUN_TEST(test_traps);
	RUN_TEST(test_direct);
	RUN_TEST(test_independent);
	return test_summary();
}
