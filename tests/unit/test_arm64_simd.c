/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the SIMD&FP registers of the AArch64 model: FPCR and
 * FPSR, and the traps of CPACR_EL1, CPTR_EL2 and CPTR_EL3. Every encoding
 * was produced by the GNU assembler. Section numbers refer to the Arm
 * Architecture Reference Manual, ARM DDI 0487 M.d.
 */

#include <stdint.h>
#include <string.h>

#include "arm64_test.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/macros.h"

#define DATA 0x1000 /* data area in the test machine's RAM */

#define ESR_FP UINT32_C(0x1fe00000) /* EC 0x07, IL, CV = 1, COND = 1110 */
#define ESR_UNKNOWN UINT32_C(0x02000000)

#define MRS_X0_FPCR 0xd53b4400u /* MRS X0, FPCR */
#define MSR_FPCR_X0 0xd51b4400u /* MSR FPCR, X0 */
#define MRS_X0_FPSR 0xd53b4420u /* MRS X0, FPSR */
#define MSR_FPSR_X0 0xd51b4420u /* MSR FPSR, X0 */

/* The byte at DATA + i, after fill(). */
static uint8_t pattern(unsigned i)
{
	return (uint8_t)(i * 13 + 1);
}

/* Fill 8KB at DATA with the pattern. */
static void fill(Machine *m)
{
	for (unsigned i = 0; i < 0x2000; i++)
		CHECK_EQ(bus_write(m->bus, DATA + i, 1, pattern(i)), BUS_OK);
}

/* Set every SIMD&FP register to a value that no test expects. */
static void garbage(Arm64Cpu *c)
{
	for (unsigned i = 0; i < 32; i++)
		set_v(c, i, 0xeeeeeeeeeeeeee00 | i, 0xdddddddddddddd00 | i);
}

/* Check the two halves of Vn. */
#define CHECK_V(c, n, lo, hi)                                                  \
	do {                                                                       \
		CHECK_EQ((c)->v[n].d[0], (uint64_t)(lo));                              \
		CHECK_EQ((c)->v[n].d[1], (uint64_t)(hi));                              \
	} while (0)

/* A machine at EL3, the MMU off, with the pattern at DATA. */
static Machine *machine(Arm64Cpu **c)
{
	Machine *m = arm64_test_machine(c);

	fill(m);
	garbage(*c);
	return m;
}

static void test_reset_and_registers(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->fpcr = 0x07c00000;
	c->fpsr = 0x9f;
	machine_reset(m);
	for (unsigned i = 0; i < 32; i++)
		CHECK_V(c, i, 0, 0);
	CHECK_EQ(c->fpcr, 0);
	CHECK_EQ(c->fpsr, 0);

	/* Only the bits of the Cortex-A53 hold a value, DDI 0502G 2.3. */
	c->x[0] = UINT64_MAX;
	arm64_test_exec(m, c, MSR_FPCR_X0);
	arm64_test_exec(m, c, MSR_FPSR_X0);
	CHECK_EQ(c->fpcr, 0x07c00000); /* AHP, DN, FZ, RMode */
	CHECK_EQ(c->fpsr, 0xf800009f); /* N, Z, C, V, QC, IDC, IXC to IOC */
	c->x[0] = 0;
	arm64_test_exec(m, c, MRS_X0_FPCR);
	CHECK_EQ(c->x[0], 0x07c00000);
	arm64_test_exec(m, c, MRS_X0_FPSR);
	CHECK_EQ(c->x[0], 0xf800009f);
	c->x[0] = 0x00400000; /* RMode = 01 */
	arm64_test_exec(m, c, MSR_FPCR_X0);
	arm64_test_exec(m, c, MRS_X0_FPCR);
	CHECK_EQ(c->x[0], 0x00400000);
	CHECK_EQ(c->fpsr, 0xf800009f); /* FPCR and FPSR are apart */

	/* EL0 may use them when CPACR_EL1.FPEN allows it. */
	c->sys.scr_el3 = ARM64_SCR_RW | ARM64_SCR_NS;
	c->sys.hcr_el2 = ARM64_HCR_RW;
	c->sys.cpacr_el1 = 3u << 20;
	c->el = 0;
	c->x[0] = 0x8000001f;
	arm64_test_exec(m, c, MSR_FPSR_X0);
	CHECK_EQ(c->fpsr, 0x8000001f);
	machine_destroy(m);
}

/*
 * Run insn at el with the given trap controls; EL2 is enabled unless
 * secure. Returns the Exception level the PE ends at, with the syndrome
 * in *esr, or 0xff when the instruction executed.
 */
static unsigned run_trap(uint32_t insn, unsigned el, unsigned fpen, bool cptr2,
                         bool cptr3, bool tge, bool secure, uint64_t *esr)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	unsigned target = 0xff;

	c->sys.scr_el3 = ARM64_SCR_RW | (secure ? 0 : ARM64_SCR_NS);
	c->sys.hcr_el2 = ARM64_HCR_RW | (tge ? ARM64_HCR_TGE : 0);
	c->sys.cpacr_el1 = (uint64_t)fpen << 20;
	c->sys.cptr_el2 = 0x33ff | (cptr2 ? ARM64_CPTR_TFP : 0);
	c->sys.cptr_el3 = cptr3 ? ARM64_CPTR_TFP : 0;
	c->el = el;
	c->x[1] = DATA;
	CHECK_EQ(arm64_test_step(m, c, insn), MACHINE_STOP_LIMIT);
	if (c->pc != RESET_PC + 4) {
		target = c->el;
		*esr = c->sys.esr[target];
		CHECK_EQ(c->sys.elr[target], RESET_PC);
	}
	machine_destroy(m);
	return target;
}

static void check_trap(uint32_t insn, unsigned el, unsigned fpen, bool cptr2,
                       bool cptr3, bool tge, bool secure, unsigned target,
                       uint32_t esr)
{
	uint64_t got = 0;
	unsigned to = run_trap(insn, el, fpen, cptr2, cptr3, tge, secure, &got);

	CHECK_EQ(to, target);
	if (to == target && target != 0xff)
		CHECK_EQ(got, esr);
}

static void test_traps(void)
{
	static const uint32_t users[] = {MRS_X0_FPCR};

	for (unsigned i = 0; i < ARRAY_SIZE(users); i++) {
		uint32_t insn = users[i];

		/* CPACR_EL1.FPEN: 00 and 10 trap EL0 and EL1, 01 EL0 only. */
		check_trap(insn, 1, 0, false, false, false, false, 1, ESR_FP);
		check_trap(insn, 0, 0, false, false, false, false, 1, ESR_FP);
		check_trap(insn, 1, 2, false, false, false, false, 1, ESR_FP);
		check_trap(insn, 0, 2, false, false, false, false, 1, ESR_FP);
		check_trap(insn, 0, 1, false, false, false, false, 1, ESR_FP);
		check_trap(insn, 1, 1, false, false, false, false, 0xff, 0);
		check_trap(insn, 0, 3, false, false, false, false, 0xff, 0);
		/* EL2 and EL3 ignore CPACR_EL1. */
		check_trap(insn, 2, 0, false, false, false, false, 0xff, 0);
		check_trap(insn, 3, 0, false, false, false, false, 0xff, 0);

		/* CPTR_EL2.TFP traps EL0, EL1 and EL2 to EL2, with EL2 enabled. */
		check_trap(insn, 0, 3, true, false, false, false, 2, ESR_FP);
		check_trap(insn, 1, 3, true, false, false, false, 2, ESR_FP);
		check_trap(insn, 2, 3, true, false, false, false, 2, ESR_FP);
		check_trap(insn, 1, 3, true, false, false, true, 0xff, 0);
		check_trap(insn, 3, 3, true, false, false, false, 0xff, 0);

		/* CPTR_EL3.TFP traps every level to EL3. */
		check_trap(insn, 0, 3, false, true, false, false, 3, ESR_FP);
		check_trap(insn, 1, 3, false, true, false, true, 3, ESR_FP);
		check_trap(insn, 2, 3, false, true, false, false, 3, ESR_FP);
		check_trap(insn, 3, 0, false, true, false, false, 3, ESR_FP);

		/* The first control that disables it wins. */
		check_trap(insn, 1, 0, true, true, false, false, 1, ESR_FP);
		check_trap(insn, 0, 1, true, true, false, false, 1, ESR_FP);
		check_trap(insn, 1, 3, true, true, false, false, 2, ESR_FP);
		check_trap(insn, 2, 0, true, true, false, false, 2, ESR_FP);

		/* HCR_EL2.TGE sends a trap of EL0 to EL2, as Unknown reason. */
		check_trap(insn, 0, 0, false, false, true, false, 2, ESR_UNKNOWN);
		/* But TGE only routes from EL0 for MRS and MSR. */
		check_trap(insn, 1, 0, false, false, true, false,
		           insn == MRS_X0_FPCR ? 1 : 2,
		           insn == MRS_X0_FPCR ? ESR_FP : ESR_UNKNOWN);
		/* And it leaves the traps of CPTR_EL2 alone. */
		check_trap(insn, 0, 3, true, false, true, false, 2, ESR_FP);
	}

	/* Both registers trap, for MRS and MSR. */
	static const uint32_t kinds[] = {MSR_FPSR_X0, MRS_X0_FPSR, MSR_FPCR_X0};

	for (unsigned i = 0; i < ARRAY_SIZE(kinds); i++)
		check_trap(kinds[i], 1, 0, false, false, false, false, 1, ESR_FP);
}

int main(void)
{
	RUN_TEST(test_reset_and_registers);
	RUN_TEST(test_traps);
	return test_summary();
}
