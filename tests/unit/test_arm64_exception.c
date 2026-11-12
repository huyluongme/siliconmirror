/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the exceptions of the AArch64 model
 * (src/cpu/arm64/exception.c): taking an exception, ERET, the exception
 * generating instructions, and the faults and traps that the CPU checks.
 * The encodings come from the GNU assembler; the expected ESR values were
 * computed separately. Section numbers refer to the Arm Architecture
 * Reference Manual, ARM DDI 0487 M.d.
 */

#include <stdint.h>
#include <string.h>

#include "arm64_test.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/macros.h"

#define SVC 0xd4024681 /* SVC #0x1234 */
#define HVC 0xd4000442 /* HVC #0x22 */
#define SMC 0xd4000663 /* SMC #0x33 */
#define BRK 0xd4200880 /* BRK #0x44 */
#define ERET 0xd69f03e0
#define WFI 0xd503207f
#define WFE 0xd503205f
#define NOP 0xd503201f
#define UDF 0x00000000

#define ESR_SVC 0x56001234
#define ESR_HVC 0x5a000022
#define ESR_SMC 0x5e000033
#define ESR_BRK 0xf2000044
#define ESR_UNKNOWN 0x02000000
#define ESR_ILLEGAL 0x3a000000

/* The vector table of each level: VBAR_ELn = n * 0x10000. */
#define VBAR(el) ((uint64_t)(el) * 0x10000)
#define CURRENT_SP0 0x000
#define CURRENT_SPX 0x200
#define LOWER_A64 0x400
#define LOWER_A32 0x600

#define A53 (&arm64_cortex_a53_bcm2837)

/* A core that has AArch64 only at every level: no AArch32. */
static const Arm64RegValue aarch64_only_values[] = {
	{ARM64_ID_AA64PFR0_EL1, 0x1111},
};
static const Arm64CpuModel aarch64_only = {
	.name = "AArch64 only",
	.values = aarch64_only_values,
	.num_values = ARRAY_SIZE(aarch64_only_values),
};

/*
 * A machine with 1 MiB of RAM and one core: a Cortex-A53 of the BCM2837,
 * which has AArch32 at every level, a core with AArch64 only, or a core
 * without a model, whose ID registers read as 0.
 */
static Machine *machine(Arm64Cpu **cpu, const Arm64CpuModel *model,
                        unsigned highest_el)
{
	Arm64Config config = {
		.reset_pc = RESET_PC,
		.highest_el = highest_el,
		.model = model,
		.mpidr = 0x80000000,
	};
	Machine *m = machine_create();
	Cpu *base = arm64_cpu_create(&config);

	CHECK(m != NULL && base != NULL);
	CHECK_EQ(bus_map_ram(m->bus, "ram", 0, 0x100000), BUS_OK);
	CHECK(machine_add_cpu(m, base));
	*cpu = arm64_cpu_of(base);
	for (unsigned el = 1; el <= 3; el++)
		(*cpu)->sys.vbar[el] = VBAR(el);
	return m;
}

static Machine *a53(Arm64Cpu **cpu)
{
	return machine(cpu, &arm64_cortex_a53_bcm2837, 3);
}

/*
 * Put the core at el, with EL2 enabled and every level using AArch64, as
 * firmware would leave it, and with SP_ELx or SP_EL0.
 */
static void enter(Arm64Cpu *c, unsigned el, bool sp_elx)
{
	c->sys.scr_el3 |= ARM64_SCR_NS | ARM64_SCR_RW | ARM64_SCR_HCE;
	c->sys.hcr_el2 |= ARM64_HCR_RW;
	c->el = el;
	c->sp_sel = el > 0 && sp_elx;
}

/* Run insn, which must not stop the machine. */
static void run(Machine *m, Arm64Cpu *c, uint32_t insn)
{
	CHECK_EQ(arm64_test_step(m, c, insn), MACHINE_STOP_LIMIT);
}

/*
 * Run insn and check the exception it takes: the target level, the entry
 * of its vector table, ESR_ELx, and ELR_ELx as an offset from the
 * instruction (0 for the instruction itself, 4 for the next one).
 */
static void expect(Machine *m, Arm64Cpu *c, uint32_t insn, unsigned el,
                   uint64_t vector, uint32_t esr, uint64_t elr_offset)
{
	uint64_t at = c->pc;

	run(m, c, insn);
	CHECK_EQ(c->el, el);
	CHECK_EQ(c->pc, VBAR(el) + vector);
	CHECK_EQ(c->sys.esr[el], esr);
	CHECK_EQ(c->sys.elr[el], at + elr_offset);
}

static void test_entry(void)
{
	Arm64Cpu *c;
	Machine *m = a53(&c);

	/* D1.4.2: SPSR, ELR and ESR are written, PSTATE changes. */
	c->nzcv = ARM64_NZCV_N | ARM64_NZCV_C;
	c->daif = ARM64_DAIF_A;
	c->sys.far[3] = 0x1234;
	c->sys.vbar[3] = VBAR(3) | 0x7ff; /* bits 10:0 are not used */
	expect(m, c, SVC, 3, CURRENT_SPX, ESR_SVC, 4);
	CHECK_EQ(c->sys.spsr[3], 0xa000010d); /* N, C; A; EL3 with SP_EL3 */
	CHECK_EQ(c->daif, 0x3c0);
	CHECK(c->sp_sel);
	CHECK(!c->il);
	CHECK_EQ(c->nzcv, ARM64_NZCV_N | ARM64_NZCV_C); /* NZCV stays */
	CHECK_EQ(c->sys.far[3], 0x1234); /* not an exception with an address */

	/* Reset clears PSTATE.IL. */
	c->il = true;
	machine_reset(m);
	CHECK(!c->il);
	machine_destroy(m);

	/* From the current level with SP_EL0: offset 0, then SP_EL3. */
	m = a53(&c);
	c->sp_sel = false;
	expect(m, c, SVC, 3, CURRENT_SP0, ESR_SVC, 4);
	CHECK_EQ(c->sys.spsr[3], 0x3cc); /* EL3 with SP_EL0 */
	CHECK(c->sp_sel);
	machine_destroy(m);
}

static void test_vectors_from_lower_levels(void)
{
	Arm64Cpu *c;
	Machine *m = a53(&c);

	/* From EL1 using AArch64 to EL2: offset 0x400. */
	enter(c, 1, true);
	expect(m, c, HVC, 2, LOWER_A64, ESR_HVC, 4);
	CHECK_EQ(c->sys.spsr[2], 0x3c5); /* EL1 with SP_EL1 */
	machine_destroy(m);

	/*
	 * The offset follows the RW bits of the level below the target, as
	 * AArch64_TakeException() reads them: 0x600 when HCR_EL2.RW makes EL1
	 * AArch32. (The model cannot run EL1 in AArch32; the test only sets
	 * the bit.)
	 */
	m = a53(&c);
	enter(c, 1, true);
	c->sys.hcr_el2 &= ~(uint64_t)ARM64_HCR_RW;
	expect(m, c, HVC, 2, LOWER_A32, ESR_HVC, 4);
	machine_destroy(m);

	/* To EL3 with EL2 enabled, EL2's state decides: SCR_EL3.RW. */
	m = a53(&c);
	enter(c, 1, true);
	expect(m, c, SMC, 3, LOWER_A64, ESR_SMC, 4);
	machine_destroy(m);
	m = a53(&c);
	enter(c, 2, true);
	c->sys.scr_el3 &= ~(uint64_t)ARM64_SCR_RW;
	expect(m, c, SMC, 3, LOWER_A32, ESR_SMC, 4);
	machine_destroy(m);
	m = a53(&c);
	enter(c, 2, true);
	c->sys.hcr_el2 &= ~(uint64_t)ARM64_HCR_RW; /* EL1 AArch32, EL2 not */
	expect(m, c, SMC, 3, LOWER_A64, ESR_SMC, 4);
	machine_destroy(m);

	/* Without AArch32, the RW bits change nothing. */
	m = machine(&c, &aarch64_only, 3);
	enter(c, 1, true);
	c->sys.scr_el3 &= ~(uint64_t)ARM64_SCR_RW;
	c->sys.hcr_el2 &= ~(uint64_t)ARM64_HCR_RW;
	expect(m, c, HVC, 2, LOWER_A64, ESR_HVC, 4);
	machine_destroy(m);
}

static void test_svc(void)
{
	static const struct {
		unsigned from;
		uint64_t hcr;
		unsigned to;
		uint64_t vector;
	} cases[] = {
		{0, 0, 1, LOWER_A64},
		{0, ARM64_HCR_TGE, 2, LOWER_A64}, /* TGE routes it from EL0 */
		{1, 0, 1, CURRENT_SPX},
		{2, 0, 2, CURRENT_SPX},
		{3, 0, 3, CURRENT_SPX},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = a53(&c);

		enter(c, cases[i].from, true);
		c->sys.hcr_el2 |= cases[i].hcr;
		expect(m, c, SVC, cases[i].to, cases[i].vector, ESR_SVC, 4);
		machine_destroy(m);
	}
}

static void test_hvc(void)
{
	Arm64Cpu *c;
	Machine *m;

	/* To EL2 from EL1 and EL2, to EL3 from EL3. */
	for (unsigned el = 1; el <= 3; el++) {
		m = a53(&c);
		enter(c, el, true);
		expect(m, c, HVC, el == 3 ? 3 : 2, el == 1 ? LOWER_A64 : CURRENT_SPX,
		       ESR_HVC, 4);
		machine_destroy(m);
	}

	/* UNDEFINED at EL0, at EL1 without EL2 (Secure), with SCR_EL3.HCE 0. */
	m = a53(&c);
	enter(c, 0, false);
	expect(m, c, HVC, 1, LOWER_A64, ESR_UNKNOWN, 0);
	machine_destroy(m);
	m = a53(&c);
	enter(c, 1, true);
	c->sys.scr_el3 &= ~(uint64_t)ARM64_SCR_NS;
	expect(m, c, HVC, 1, CURRENT_SPX, ESR_UNKNOWN, 0);
	machine_destroy(m);
	m = a53(&c);
	enter(c, 2, true);
	c->sys.scr_el3 &= ~(uint64_t)ARM64_SCR_HCE;
	expect(m, c, HVC, 2, CURRENT_SPX, ESR_UNKNOWN, 0);
	machine_destroy(m);

	/* Without EL3, HCR_EL2.HCD disables it instead. */
	m = machine(&c, NULL, 2);
	c->sys.hcr_el2 = ARM64_HCR_HCD;
	expect(m, c, HVC, 2, CURRENT_SPX, ESR_UNKNOWN, 0);
	c->sys.hcr_el2 = 0;
	expect(m, c, HVC, 2, CURRENT_SPX, ESR_HVC, 4);
	machine_destroy(m);
}

static void test_smc(void)
{
	Arm64Cpu *c;
	Machine *m;

	/* To EL3 from EL1 and EL2. */
	for (unsigned el = 1; el <= 2; el++) {
		m = a53(&c);
		enter(c, el, true);
		expect(m, c, SMC, 3, LOWER_A64, ESR_SMC, 4);
		machine_destroy(m);
	}

	/* UNDEFINED at EL0, and with SCR_EL3.SMD set. */
	m = a53(&c);
	enter(c, 0, false);
	expect(m, c, SMC, 1, LOWER_A64, ESR_UNKNOWN, 0);
	machine_destroy(m);
	m = a53(&c);
	enter(c, 1, true);
	c->sys.scr_el3 |= ARM64_SCR_SMD;
	expect(m, c, SMC, 1, CURRENT_SPX, ESR_UNKNOWN, 0);
	machine_destroy(m);

	/* HCR_EL2.TSC traps it from EL1 to EL2, SMD or not; back to the SMC. */
	m = a53(&c);
	enter(c, 1, true);
	c->sys.scr_el3 |= ARM64_SCR_SMD;
	c->sys.hcr_el2 |= ARM64_HCR_TSC;
	expect(m, c, SMC, 2, LOWER_A64, ESR_SMC, 0);
	machine_destroy(m);

	/* Without EL3 it is UNDEFINED, TSC or not. */
	m = machine(&c, NULL, 2);
	enter(c, 1, true);
	c->sys.hcr_el2 |= ARM64_HCR_TSC;
	expect(m, c, SMC, 1, CURRENT_SPX, ESR_UNKNOWN, 0);
	machine_destroy(m);
}

static void test_brk(void)
{
	static const struct {
		unsigned from;
		uint64_t hcr, mdcr;
		unsigned to;
		uint64_t vector;
	} cases[] = {
		{0, 0, 0, 1, LOWER_A64},
		{1, 0, 0, 1, CURRENT_SPX},
		{0, ARM64_HCR_TGE, 0, 2, LOWER_A64},
		{0, 0, ARM64_MDCR_TDE, 2, LOWER_A64}, /* MDCR_EL2.TDE: debug to EL2 */
		{1, 0, ARM64_MDCR_TDE, 2, LOWER_A64},
		{2, 0, ARM64_MDCR_TDE, 2, CURRENT_SPX},
		{3, 0, 0, 3, CURRENT_SPX},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = a53(&c);

		enter(c, cases[i].from, true);
		c->sys.hcr_el2 |= cases[i].hcr;
		c->sys.mdcr_el2 = cases[i].mdcr;
		/* A breakpoint returns to the BRK itself. */
		expect(m, c, BRK, cases[i].to, cases[i].vector, ESR_BRK, 0);
		machine_destroy(m);
	}
}

static void test_undefined_exception_generation(void)
{
	/* The Exception generation table of C4.1, checked with binutils. */
	static const uint32_t insns[] = {
		0xd4400aa0, /* HLT #0x55: no external debugger enables it */
		0xd4a00001, /* DCPS1: only in Debug state */
		0xd4a00002, /* DCPS2 */
		0xd4a00003, /* DCPS3 */
		0xd4000000, /* opc 000, LL 00 */
		0xd4000005, /* op2 != 000 */
		0xd4200001, /* BRK with LL != 00 */
		0xd4600000, /* opc 011 */
		0xd4800000, /* opc 100 */
		0xd4a00000, /* opc 101, LL 00 */
		0xd4e00000, /* opc 111 */
		0xd69f0000, /* ERET with Rn != 11111 */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

static void test_undefined_routing(void)
{
	Arm64Cpu *c;
	Machine *m = a53(&c);

	/* AArch64_Undefined(): TGE sends it to EL2 from EL0, not from EL1. */
	enter(c, 0, false);
	c->sys.hcr_el2 |= ARM64_HCR_TGE;
	expect(m, c, UDF, 2, LOWER_A64, ESR_UNKNOWN, 0);
	machine_destroy(m);
	m = a53(&c);
	enter(c, 1, true);
	c->sys.hcr_el2 |= ARM64_HCR_TGE;
	expect(m, c, UDF, 1, CURRENT_SPX, ESR_UNKNOWN, 0);
	machine_destroy(m);
}

static void test_eret(void)
{
	Arm64Cpu *c;
	Machine *m = a53(&c);

	/* From EL2 to EL1 with SP_EL1. */
	enter(c, 2, true);
	c->sys.spsr[2] = 0x60000145; /* Z, C; A, F; EL1h */
	c->sys.elr[2] = 0x40000;
	c->event_register = false;
	run(m, c, ERET);
	CHECK_EQ(c->el, 1);
	CHECK(c->sp_sel);
	CHECK_EQ(c->nzcv, 0x60000000);
	CHECK_EQ(c->daif, 0x140);
	CHECK_EQ(c->pc, 0x40000);
	CHECK(!c->il);
	CHECK(c->event_register); /* SendEventLocal() */

	/* From EL1 to EL0, with SP_EL0. */
	c->sys.spsr[1] = 0x3c0;
	c->sys.elr[1] = 0x50000;
	run(m, c, ERET);
	CHECK_EQ(c->el, 0);
	CHECK(!c->sp_sel);
	CHECK_EQ(c->pc, 0x50000);

	/* ERET is UNDEFINED at EL0. */
	expect(m, c, ERET, 1, LOWER_A64, ESR_UNKNOWN, 0);

	/* To EL1 with SP_EL0, and SPSR.IL set: a legal return keeps IL. */
	c->sys.spsr[1] = 0x00100004; /* IL; EL1t */
	c->sys.elr[1] = 0x60000;
	run(m, c, ERET);
	CHECK_EQ(c->el, 1);
	CHECK(!c->sp_sel);
	CHECK(c->il);
	machine_destroy(m);
}

/*
 * D1.4.4.2: an illegal return keeps the level and SP, sets PSTATE.IL,
 * still takes NZCV, DAIF and the PC; the next instruction then takes an
 * Illegal Execution state exception to the same level.
 */
static void check_illegal_return(Machine *m, Arm64Cpu *c, uint64_t spsr)
{
	unsigned el = c->el;
	bool sp_sel = c->sp_sel;

	c->sys.spsr[el] = spsr | 0x80000000 | 0x80; /* N; I */
	c->sys.elr[el] = 0x40000;
	run(m, c, ERET);
	CHECK_EQ(c->el, el);
	CHECK_EQ(c->sp_sel, sp_sel);
	CHECK(c->il);
	CHECK_EQ(c->nzcv, 0x80000000);
	CHECK_EQ(c->daif, 0x80);
	CHECK_EQ(c->pc, 0x40000);

	/* Any instruction: the exception comes before it runs. */
	expect(m, c, NOP, el, sp_sel ? CURRENT_SPX : CURRENT_SP0, ESR_ILLEGAL, 0);
	CHECK_EQ(c->sys.spsr[el] & 0x00100000, 0x00100000); /* SPSR.IL */
	CHECK(!c->il);
}

static void test_illegal_returns(void)
{
	static const struct {
		const char *what;
		const Arm64CpuModel *model;
		unsigned el;
		uint64_t scr, hcr; /* bits to clear, then to set */
		uint64_t clear_scr, clear_hcr;
		uint64_t spsr;
	} cases[] = {
		{"to a higher level", A53, 2, 0, 0, 0, 0, 0xd},
		{"with M[1] set", A53, 2, 0, 0, 0, 0, 0x6},
		{"to EL0 with M[0] set", A53, 2, 0, 0, 0, 0, 0x1},
		{"to Secure EL2", A53, 3, 0, 0, ARM64_SCR_NS, 0, 0x9},
		{"to EL1 in AArch64 when HCR_EL2.RW is 0", A53, 2, 0, 0, 0,
	     ARM64_HCR_RW, 0x5},
		{"to EL0 in AArch64 when EL1 uses AArch32", A53, 2, 0, 0, 0,
	     ARM64_HCR_RW, 0x0},
		{"to EL1 in AArch32 when HCR_EL2.RW is 1", A53, 2, 0, 0, 0, 0, 0x13},
		{"to EL1 when HCR_EL2.TGE is set", A53, 2, 0, ARM64_HCR_TGE, 0, 0, 0x5},
		{"to a reserved AArch32 mode", A53, 1, 0, 0, 0, 0, 0x14},
		{"to AArch32 Monitor mode: EL3 uses AArch64", A53, 3, 0, 0, 0, 0, 0x16},
		{"to AArch32 Hyp mode in Secure state", A53, 3, 0, 0, ARM64_SCR_NS, 0,
	     0x1a},
		{"to AArch32 on a core without a model", NULL, 1, 0, 0, 0, 0, 0x10},
		{"to AArch32 on an AArch64-only core", &aarch64_only, 1, 0, 0, 0, 0,
	     0x10},
		{"to a reserved AArch32 mode when EL1 uses AArch32", A53, 2, 0, 0, 0,
	     ARM64_HCR_RW, 0x14},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = machine(&c, cases[i].model, 3);
		int before = test_failures;

		enter(c, cases[i].el, true);
		c->sys.scr_el3 = (c->sys.scr_el3 & ~cases[i].clear_scr) | cases[i].scr;
		c->sys.hcr_el2 = (c->sys.hcr_el2 & ~cases[i].clear_hcr) | cases[i].hcr;
		check_illegal_return(m, c, cases[i].spsr);
		if (test_failures != before)
			fprintf(stderr, "  in the illegal return %s\n", cases[i].what);
		machine_destroy(m);
	}

	/* AArch32 Monitor mode, on a core without EL3: the core starts at EL2. */
	Arm64Cpu *c;
	Machine *m = machine(&c, A53, 2);

	check_illegal_return(m, c, 0x16);
	machine_destroy(m);

	/* A PC alignment fault comes first (priority 6 of D1.4.5.6). */
	m = a53(&c);

	enter(c, 2, true);
	c->sys.spsr[2] = 0xd; /* EL3: illegal */
	c->sys.elr[2] = 0x40002;
	run(m, c, ERET);
	CHECK(c->il);
	run(m, c, NOP);
	CHECK_EQ(c->sys.esr[2], 0x8a000000); /* PC alignment */
	CHECK_EQ(c->sys.far[2], 0x40002);
	CHECK_EQ(c->sys.spsr[2] & 0x00100000, 0x00100000);
	machine_destroy(m);

	/*
	 * A legal return to EL0 with SPSR.IL set: with HCR_EL2.TGE, the
	 * Illegal Execution state exception goes to EL2.
	 */
	m = a53(&c);
	enter(c, 2, true);
	c->sys.hcr_el2 |= ARM64_HCR_TGE;
	c->sys.spsr[2] = 0x00100000; /* IL; EL0 */
	c->sys.elr[2] = 0x40000;
	run(m, c, ERET);
	CHECK_EQ(c->el, 0);
	expect(m, c, NOP, 2, LOWER_A64, ESR_ILLEGAL, 0);
	machine_destroy(m);
}

static void test_eret_without_aarch32(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c, &aarch64_only, 3);

	/* SCR_EL3.RW is 0 after reset, but EL2 can only use AArch64. */
	c->sys.scr_el3 = ARM64_SCR_NS;
	c->sys.spsr[3] = 0x9; /* EL2h */
	c->sys.elr[3] = 0x40000;
	run(m, c, ERET);
	CHECK_EQ(c->el, 2);
	CHECK(!c->il);
	machine_destroy(m);

	/* In Secure state there is no EL2: HCR_EL2.RW does not apply. */
	m = a53(&c);
	c->sys.scr_el3 = ARM64_SCR_RW;
	c->sys.hcr_el2 = 0;
	c->sys.spsr[3] = 0x5; /* EL1h */
	c->sys.elr[3] = 0x40000;
	run(m, c, ERET);
	CHECK_EQ(c->el, 1);
	CHECK(!c->il);
	machine_destroy(m);
}

static void test_eret_to_aarch32_stops(void)
{
	/* Legal returns to AArch32: the model cannot run AArch32. */
	static const struct {
		unsigned el;
		uint64_t clear_scr, clear_hcr;
		uint64_t spsr;
	} cases[] = {
		{1, 0, 0, 0x10},                           /* EL1 to User */
		{2, 0, ARM64_HCR_RW, 0x13},                /* EL2 to Supervisor */
		{3, ARM64_SCR_RW, 0, 0x1a},                /* EL3 to Hyp */
		{3, ARM64_SCR_RW | ARM64_SCR_NS, 0, 0x13}, /* Secure Supervisor */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = a53(&c);

		enter(c, cases[i].el, true);
		c->sys.scr_el3 &= ~cases[i].clear_scr;
		c->sys.hcr_el2 &= ~cases[i].clear_hcr;
		c->sys.spsr[cases[i].el] = cases[i].spsr;
		CHECK_EQ(arm64_test_step(m, c, ERET), MACHINE_STOP_ERROR);
		CHECK(strstr(m->stop_message, "is not implemented yet (exception "
		                              "return to AArch32 state)") != NULL);
		CHECK_EQ(c->el, cases[i].el);
		machine_destroy(m);
	}
}

static void test_wfx_traps(void)
{
	static const struct {
		unsigned from;
		uint64_t sctlr1, hcr, scr; /* SCTLR_EL1, HCR_EL2, SCR_EL3 bits */
		uint32_t insn;
		unsigned to;
		uint32_t esr;
	} cases[] = {
		/* SCTLR_EL1.nTWI and nTWE are 0: EL0 traps to EL1. */
		{0, 0, 0, 0, WFI, 1, 0x07e00000},
		{0, 0, 0, 0, WFE, 1, 0x07e00001},
		{0, 0, ARM64_HCR_TGE, 0, WFI, 2, 0x07e00000},
		/* EL1's trap comes before EL2's, EL2's before EL3's. */
		{0, 0, ARM64_HCR_TWI, ARM64_SCR_TWI, WFI, 1, 0x07e00000},
		{0, ARM64_SCTLR_NTWI, ARM64_HCR_TWI, ARM64_SCR_TWI, WFI, 2, 0x07e00000},
		{1, 0, ARM64_HCR_TWE, ARM64_SCR_TWE, WFE, 2, 0x07e00001},
		{1, 0, 0, ARM64_SCR_TWE, WFE, 3, 0x07e00001},
		{2, 0, ARM64_HCR_TWI, ARM64_SCR_TWI, WFI, 3, 0x07e00000},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = a53(&c);

		enter(c, cases[i].from, true);
		c->sys.sctlr[1] = cases[i].sctlr1;
		c->sys.hcr_el2 |= cases[i].hcr;
		c->sys.scr_el3 |= cases[i].scr;
		c->event_register = false;
		expect(m, c, cases[i].insn, cases[i].to,
		       cases[i].from == cases[i].to ? CURRENT_SPX : LOWER_A64,
		       cases[i].esr, 0);
		CHECK_EQ(c->cpu.state, CPU_RUNNING);
		machine_destroy(m);
	}

	/* A WFE that finds the Event Register set neither waits nor traps. */
	Arm64Cpu *c;
	Machine *m = a53(&c);

	enter(c, 0, false);
	c->sys.sctlr[1] = 0;
	c->event_register = true;
	run(m, c, WFE);
	CHECK_EQ(c->el, 0);
	CHECK(!c->event_register);
	CHECK_EQ(c->pc, RESET_PC + 4);
	machine_destroy(m);

	/* Nothing traps at EL3: the core waits. */
	m = a53(&c);
	c->sys.scr_el3 = ARM64_SCR_TWI;
	run(m, c, WFI);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	machine_destroy(m);
}

static void test_sp_alignment(void)
{
	Arm64Cpu *c;
	Machine *m = a53(&c);

	/* SCTLR_EL3.SA is set at reset: SP must be 16-byte aligned. */
	c->sp_el[3] = 0x1008;
	c->sys.far[3] = 0x1234;
	expect(m, c, 0xf94003e0, 3, CURRENT_SPX, 0x9a000000, 0); /* LDR X0, [SP] */
	CHECK_EQ(c->sys.far[3], 0x1234); /* FAR is not written */
	/* SP itself is checked, not the address. */
	expect(m, c, 0xf94007e0, 3, CURRENT_SPX, 0x9a000000, 0); /* [SP, #8] */
	expect(m, c, 0xa94007e0, 3, CURRENT_SPX, 0x9a000000, 0); /* LDP */
	/* STR X0, [SP, #-16]!: no writeback either. */
	expect(m, c, 0xf81f0fe0, 3, CURRENT_SPX, 0x9a000000, 0);
	CHECK_EQ(c->sp_el[3], 0x1008);
	/* PRFM and ADD use SP without the check. */
	run(m, c, 0xf98007e0); /* PRFM PLDL1KEEP, [SP, #8] */
	run(m, c, 0x910007e0); /* ADD X0, SP, #1 */
	CHECK_EQ(c->x[0], 0x1009);
	/* An aligned SP is fine, whatever the offset. */
	c->sp_el[3] = 0x1000;
	run(m, c, 0xf94007e0);
	/* Without SA, an 8-byte aligned SP is enough for LDR X. */
	c->sp_el[3] = 0x1008;
	c->sys.sctlr[3] = 0;
	run(m, c, 0xf94003e0);
	machine_destroy(m);

	/* EL0 checks SCTLR_EL1.SA0, and the fault goes to EL1. */
	m = a53(&c);
	enter(c, 0, false);
	c->sp_el[0] = 0x1008;
	c->sys.sctlr[1] = ARM64_SCTLR_SA0;
	expect(m, c, 0xf94003e0, 1, LOWER_A64, 0x9a000000, 0);
	machine_destroy(m);
	m = a53(&c);
	enter(c, 0, false);
	c->sp_el[0] = 0x1008;
	c->sys.sctlr[1] = ARM64_SCTLR_SA;
	run(m, c, 0xf94003e0);
	CHECK_EQ(c->el, 0);
	machine_destroy(m);

	/* AArch64_SPAlignmentFault(): TGE sends it to EL2, from EL1 too. */
	m = a53(&c);
	enter(c, 1, true);
	c->sp_el[1] = 0x1008;
	c->sys.hcr_el2 |= ARM64_HCR_TGE;
	expect(m, c, 0xf94003e0, 2, LOWER_A64, 0x9a000000, 0);
	machine_destroy(m);
}

static void test_data_aborts_from_lower_levels(void)
{
	Arm64Cpu *c;
	Machine *m = a53(&c);

	/* From EL0 to EL1: EC 0x24, from a lower level. */
	enter(c, 0, false);
	c->x[1] = 0x1001;
	expect(m, c, 0xf9400020, 1, LOWER_A64, 0x92000021, 0); /* LDR X0, [X1] */
	CHECK_EQ(c->sys.far[1], 0x1001);
	machine_destroy(m);

	/* AArch64_DataAbort(): TGE sends it to EL2 from EL0 and from EL1. */
	for (unsigned el = 0; el <= 1; el++) {
		m = a53(&c);
		enter(c, el, true);
		c->sys.hcr_el2 |= ARM64_HCR_TGE;
		c->x[1] = 0x1002;
		/* STR X0, [X1]: WnR. */
		expect(m, c, 0xf9000020, 2, LOWER_A64, 0x92000061, 0);
		CHECK_EQ(c->sys.far[2], 0x1002);
		machine_destroy(m);
	}

	/* The PC alignment fault follows the same routing. */
	m = a53(&c);
	enter(c, 1, true);
	c->sys.hcr_el2 |= ARM64_HCR_TGE;
	c->pc = RESET_PC + 1;
	CHECK_EQ(machine_run(m, 1), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->el, 2);
	CHECK_EQ(c->sys.esr[2], 0x8a000000);
	CHECK_EQ(c->sys.far[2], RESET_PC + 1);
	machine_destroy(m);
}

static void test_tidcp(void)
{
	Arm64Cpu *c;
	Machine *m = a53(&c);

	/*
	 * HCR_EL2.TIDCP traps EL1 accesses to the IMPLEMENTATION DEFINED
	 * encodings to EL2: those the core has, those it has not, and the
	 * System instructions with CRn = 11 or 15.
	 */
	enter(c, 1, true);
	c->sys.hcr_el2 |= ARM64_HCR_TIDCP;
	/* MRS X0, S3_1_C15_C2_1 (CPUECTLR_EL1) */
	expect(m, c, 0xd539f220, 2, LOWER_A64, 0x62327c05, 0);
	c->el = 1;
	/* MRS X0, S3_0_C11_C0_0: no such register, UNDEFINED without TIDCP */
	expect(m, c, 0xd538b000, 2, LOWER_A64, 0x62302c01, 0);
	c->el = 1;
	/* SYS #0, C15, C0, #0 */
	expect(m, c, 0xd508f01f, 2, LOWER_A64, 0x62103fe0, 0);
	c->el = 1;
	/* Other System instructions are not trapped: IC IALLU. */
	run(m, c, 0xd508751f);
	CHECK_EQ(c->el, 1);
	machine_destroy(m);

	/* EL2 is not trapped; at EL0 the encodings stay UNDEFINED. */
	m = a53(&c);
	enter(c, 2, true);
	c->sys.hcr_el2 |= ARM64_HCR_TIDCP;
	run(m, c, 0xd539f220);
	CHECK_EQ(c->el, 2);
	enter(c, 0, false);
	expect(m, c, 0xd539f220, 1, LOWER_A64, ESR_UNKNOWN, 0);
	machine_destroy(m);

	/* Secure EL1 has no EL2 to trap to. */
	m = a53(&c);
	enter(c, 1, true);
	c->sys.scr_el3 &= ~(uint64_t)ARM64_SCR_NS;
	c->sys.hcr_el2 |= ARM64_HCR_TIDCP;
	run(m, c, 0xd539f220);
	CHECK_EQ(c->el, 1);
	machine_destroy(m);

	/* Without TIDCP, EL1 reads CPUECTLR_EL1. */
	m = a53(&c);
	enter(c, 1, true);
	run(m, c, 0xd539f220);
	CHECK_EQ(c->el, 1);
	machine_destroy(m);
}

int main(void)
{
	RUN_TEST(test_entry);
	RUN_TEST(test_vectors_from_lower_levels);
	RUN_TEST(test_svc);
	RUN_TEST(test_hvc);
	RUN_TEST(test_smc);
	RUN_TEST(test_brk);
	RUN_TEST(test_undefined_exception_generation);
	RUN_TEST(test_undefined_routing);
	RUN_TEST(test_eret);
	RUN_TEST(test_illegal_returns);
	RUN_TEST(test_eret_without_aarch32);
	RUN_TEST(test_eret_to_aarch32_stops);
	RUN_TEST(test_wfx_traps);
	RUN_TEST(test_sp_alignment);
	RUN_TEST(test_data_aborts_from_lower_levels);
	RUN_TEST(test_tidcp);
	return test_summary();
}
