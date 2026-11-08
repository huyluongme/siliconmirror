/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the System registers of the AArch64 model
 * (src/cpu/arm64/sysreg.c and cortex_a53.c). MRS and MSR are built from
 * the encodings in sysreg.h, which come from the GNU assembler; the other
 * encodings were produced by the assembler too. Section numbers refer to
 * the Arm Architecture Reference Manual, ARM DDI 0487 M.d, and to the
 * Cortex-A53 TRM, ARM DDI 0500J.
 */

#include <stdint.h>
#include <string.h>

#include "arm64_test.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/macros.h"

#define CPUECTLR_EL1 ARM64_SYSREG(3, 1, 15, 2, 1)
#define L2CTLR_EL1 ARM64_SYSREG(3, 1, 11, 0, 2)
#define CBAR_EL1 ARM64_SYSREG(3, 1, 15, 3, 0)
#define CPUMERRSR_EL1 ARM64_SYSREG(3, 1, 15, 2, 2)
#define L2MERRSR_EL1 ARM64_SYSREG(3, 1, 15, 2, 3)

/* MRS Xt, <reg> and MSR <reg>, Xt. */
static uint32_t mrs(uint16_t encoding, unsigned t)
{
	return 0xd5200000u | (uint32_t)encoding << 5 | t;
}

static uint32_t msr(uint16_t encoding, unsigned t)
{
	return 0xd5000000u | (uint32_t)encoding << 5 | t;
}

/* A Cortex-A53 of the BCM2837 at EL3, outside any machine. */
static Arm64Cpu *make_a53(uint64_t mpidr)
{
	Arm64Config config = {
		.reset_pc = 0x8000,
		.highest_el = 3,
		.model = &arm64_cortex_a53_bcm2837,
		.mpidr = mpidr,
	};
	Cpu *base = arm64_cpu_create(&config);

	CHECK(base != NULL);
	return arm64_cpu_of(base);
}

static void destroy(Arm64Cpu *c)
{
	c->cpu.ops->destroy(&c->cpu);
}

/* Run MRS X0, <reg> and return X0. */
static uint64_t read_reg(Arm64Cpu *c, uint16_t encoding)
{
	c->x[0] = 0x5555;
	CHECK_EQ(arm64_execute(c, mrs(encoding, 0)), ARM64_EXEC_OK);
	return c->x[0];
}

/* Run MSR <reg>, X1 with X1 = value. */
static Arm64Exec write_reg(Arm64Cpu *c, uint16_t encoding, uint64_t value)
{
	c->x[1] = value;
	return arm64_execute(c, msr(encoding, 1));
}

/* Check that insn traps to target_el with the syndrome esr. */
static void expect_trap(Arm64Cpu *c, uint32_t insn, unsigned target_el,
                        uint32_t esr)
{
	c->exception.target_el = 0;
	c->exception.esr = 0;
	CHECK_EQ(arm64_execute(c, insn), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, target_el);
	CHECK_EQ(c->exception.esr, esr);
}

static void test_encodings(void)
{
	CHECK(arm64_sysreg_table_is_sorted());

	/* The MRS and MSR helpers against the assembler. */
	CHECK_EQ(mrs(ARM64_CURRENTEL, 0), 0xd5384240); /* MRS X0, CurrentEL */
	CHECK_EQ(msr(ARM64_VBAR_EL1, 1), 0xd518c001);  /* MSR VBAR_EL1, X1 */
	CHECK_EQ(msr(ARM64_SCTLR_EL1, 1), 0xd5181001); /* MSR SCTLR_EL1, X1 */
	CHECK_EQ(mrs(ARM64_SCTLR_EL1, 0), 0xd5381000); /* MRS X0, SCTLR_EL1 */
}

static void test_id_registers(void)
{
	/* Values of TRM Table 4-1 for the BCM2837 configuration. */
	static const struct {
		uint16_t encoding;
		uint64_t value;
	} ids[] = {
		{ARM64_MIDR_EL1, 0x410fd034},
		{ARM64_REVIDR_EL1, 0},
		{ARM64_ID_AA64PFR0_EL1, 0x2222},
		{ARM64_ID_AA64DFR0_EL1, 0x10305106},
		{ARM64_ID_AA64ISAR0_EL1, 0x10000},
		{ARM64_ID_AA64MMFR0_EL1, 0x1122},
		{ARM64_ID_ISAR5_EL1, 0x10001},
		{ARM64_ID_PFR1_EL1, 0x11011},
		{ARM64_MVFR0_EL1, 0x10110222},
		{ARM64_CTR_EL0, 0x84448004},
		{ARM64_DCZID_EL0, 4},
		{ARM64_CLIDR_EL1, 0x0a200023},
		{ARM64_ID_AA64PFR1_EL1, 0}, /* listed, and 0 */
		{ARM64_ISR_EL1, 0},
		{ARM64_RVBAR_EL3, 0x8000},
	};
	Arm64Cpu *c = make_a53(0x80000002);

	for (unsigned i = 0; i < ARRAY_SIZE(ids); i++)
		CHECK_EQ(read_reg(c, ids[i].encoding), ids[i].value);
	CHECK_EQ(read_reg(c, ARM64_MPIDR_EL1), 0x80000002);

	/* D23.3: unused ID encodings read as 0, ID_AA64MMFR2_EL1 among them. */
	CHECK_EQ(read_reg(c, ARM64_SYSREG(3, 0, 0, 7, 2)), 0);
	CHECK_EQ(read_reg(c, ARM64_SYSREG(3, 0, 0, 2, 7)), 0); /* ID_ISAR6 */

	/* CCSIDR_EL1 shows the cache that CSSELR_EL1 selects. */
	CHECK_EQ(write_reg(c, ARM64_CSSELR_EL1, 0), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, ARM64_CCSIDR_EL1), 0x700fe01a); /* L1 data 32KB */
	CHECK_EQ(write_reg(c, ARM64_CSSELR_EL1, 1), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, ARM64_CCSIDR_EL1), 0x201fe00a); /* L1 code 32KB */
	CHECK_EQ(write_reg(c, ARM64_CSSELR_EL1, 2), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, ARM64_CCSIDR_EL1), 0x703fe07a); /* L2 512KB */
	CHECK_EQ(write_reg(c, ARM64_CSSELR_EL1, 0xffffffff), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, ARM64_CSSELR_EL1), 0xf); /* the rest is RES0 */
	CHECK_EQ(read_reg(c, ARM64_CCSIDR_EL1), 0);   /* no such cache */

	/* ID registers are read-only. */
	CHECK_EQ(write_reg(c, ARM64_MIDR_EL1, 0), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(write_reg(c, ARM64_SYSREG(3, 0, 0, 7, 2), 0),
	         ARM64_EXEC_UNDEFINED);
	destroy(c);
}

static void test_reset_values(void)
{
	/* TRM Tables 4-3, 4-12, 4-13 and 4-17. */
	static const struct {
		uint16_t encoding;
		uint64_t value;
	} resets[] = {
		{ARM64_SCTLR_EL1, 0x00c50838},
		{ARM64_SCTLR_EL2, 0x30c50838},
		{ARM64_SCTLR_EL3, 0x00c50838},
		{ARM64_HCR_EL2, 2},
		{ARM64_MDCR_EL2, 6},
		{ARM64_CPTR_EL2, 0x33ff},
		{ARM64_RMR_EL3, 1},
		{ARM64_SCR_EL3, 0},
		{ARM64_VPIDR_EL2, 0x410fd034},
		{ARM64_VMPIDR_EL2, 0x80000003},
		{CPUECTLR_EL1, 0},
		{ARM64_SYSREG(3, 1, 15, 2, 0), 0x090ca000}, /* CPUACTLR_EL1 */
		{ARM64_SYSREG(3, 1, 15, 0, 0), 0x80000000}, /* L2ACTLR_EL1 */
		{L2CTLR_EL1, 0x03000000},                   /* four cores */
		{L2MERRSR_EL1, 0}, /* a write sets it to 0x10000000 */
	};
	Arm64Cpu *c = make_a53(0x80000003);
	uint64_t value;

	/* Change everything, then reset. */
	for (unsigned i = 0; i < ARRAY_SIZE(resets); i++)
		arm64_cpu_write_sysreg(c, resets[i].encoding, 0x1234);
	c->cpu.ops->reset(&c->cpu);
	for (unsigned i = 0; i < ARRAY_SIZE(resets); i++) {
		CHECK(arm64_cpu_read_sysreg(c, resets[i].encoding, &value));
		CHECK_EQ(value, resets[i].value);
	}
	destroy(c);
}

static void test_pstate_registers(void)
{
	Arm64Cpu *c = make_a53(0x80000000);

	CHECK_EQ(write_reg(c, ARM64_NZCV, 0xffffffffffffffff), ARM64_EXEC_OK);
	CHECK_EQ(c->nzcv, 0xf0000000);
	CHECK_EQ(read_reg(c, ARM64_NZCV), 0xf0000000);
	CHECK_EQ(write_reg(c, ARM64_DAIF, 0x140), ARM64_EXEC_OK); /* A and F */
	CHECK_EQ(c->daif, 0x140);
	CHECK_EQ(read_reg(c, ARM64_DAIF), 0x140);
	CHECK_EQ(write_reg(c, ARM64_DAIF, 0xffffffffffffffff), ARM64_EXEC_OK);
	CHECK_EQ(c->daif, 0x3c0);
	CHECK_EQ(read_reg(c, ARM64_CURRENTEL), 3 << 2);

	/* SPSel chooses the stack pointer that register 31 means. */
	c->sp_el[3] = 0x3000;
	c->sp_el[0] = 0x1000;
	CHECK_EQ(read_reg(c, ARM64_SPSEL), 1);
	CHECK_EQ(write_reg(c, ARM64_SPSEL, 0), ARM64_EXEC_OK);
	CHECK_EQ(get_xsp(c, 31), 0x1000);

	/* SP_EL0 cannot be named while it is the current stack pointer. */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SP_EL0, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(write_reg(c, ARM64_SP_EL0, 0), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(write_reg(c, ARM64_SPSEL, 1), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, ARM64_SP_EL0), 0x1000);
	CHECK_EQ(write_reg(c, ARM64_SP_EL0, 0x1100), ARM64_EXEC_OK);
	CHECK_EQ(c->sp_el[0], 0x1100);

	/* SP_EL1 from EL2 up, SP_EL2 from EL3. */
	CHECK_EQ(write_reg(c, ARM64_SP_EL1, 0x2100), ARM64_EXEC_OK);
	CHECK_EQ(write_reg(c, ARM64_SP_EL2, 0x2200), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, ARM64_SP_EL1), 0x2100);
	CHECK_EQ(read_reg(c, ARM64_SP_EL2), 0x2200);
	c->el = 2;
	CHECK_EQ(read_reg(c, ARM64_CURRENTEL), 2 << 2);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SP_EL2, 0)), ARM64_EXEC_UNDEFINED);
	c->el = 1;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SP_EL1, 0)), ARM64_EXEC_UNDEFINED);
	destroy(c);
}

static void test_access_by_el(void)
{
	Arm64Cpu *c = make_a53(0x80000000);

	/* Registers of higher Exception levels are UNDEFINED. */
	c->el = 2;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SCR_EL3, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_RVBAR_EL3, 0)), ARM64_EXEC_UNDEFINED);
	c->el = 1;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SCTLR_EL2, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_ELR_EL2, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(write_reg(c, ARM64_VBAR_EL1, 0x8800), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, ARM64_VBAR_EL1), 0x8800);

	/* EL0 may only use the registers made for it. */
	c->el = 0;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SCTLR_EL1, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_MIDR_EL1, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_CURRENTEL, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(3, 0, 0, 7, 2), 0)),
	         ARM64_EXEC_UNDEFINED);
	CHECK_EQ(read_reg(c, ARM64_DCZID_EL0), 4);
	CHECK_EQ(write_reg(c, ARM64_TPIDR_EL0, 0x77), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, ARM64_TPIDR_EL0), 0x77);

	/* TPIDRRO_EL0 is read-only at EL0. */
	c->el = 1;
	CHECK_EQ(write_reg(c, ARM64_TPIDRRO_EL0, 0x88), ARM64_EXEC_OK);
	c->el = 0;
	CHECK_EQ(read_reg(c, ARM64_TPIDRRO_EL0), 0x88);
	CHECK_EQ(write_reg(c, ARM64_TPIDRRO_EL0, 0), ARM64_EXEC_UNDEFINED);

	/* CTR_EL0 and DAIF at EL0 when SCTLR_EL1 allows them. */
	c->sys.sctlr[1] |= ARM64_SCTLR_UCT | ARM64_SCTLR_UMA;
	CHECK_EQ(read_reg(c, ARM64_CTR_EL0), 0x84448004);
	CHECK_EQ(read_reg(c, ARM64_DAIF), c->daif);
	destroy(c);
}

static void test_traps_to_el2(void)
{
	Arm64Cpu *c = make_a53(0x80000000);

	/* EL1, with EL2 enabled: the lower levels are Non-secure. */
	c->sys.scr_el3 = ARM64_SCR_NS;
	c->el = 1;

	c->sys.hcr_el2 = ARM64_HCR_TVM;
	expect_trap(c, msr(ARM64_SCTLR_EL1, 1), 2, 0x62300420);
	CHECK_EQ(read_reg(c, ARM64_SCTLR_EL1), 0x00c50838); /* reads do not */
	c->sys.hcr_el2 = ARM64_HCR_TRVM;
	expect_trap(c, mrs(ARM64_SCTLR_EL1, 0), 2, 0x62300401);
	CHECK_EQ(write_reg(c, ARM64_TTBR0_EL1, 0x4000), ARM64_EXEC_OK);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_TTBR0_EL1, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_ESR_EL1, 0)), ARM64_EXEC_EXCEPTION);

	/* The ID register groups. */
	c->sys.hcr_el2 = ARM64_HCR_TID3;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_ID_AA64PFR0_EL1, 0)),
	         ARM64_EXEC_EXCEPTION);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(3, 0, 0, 7, 2), 0)),
	         ARM64_EXEC_EXCEPTION);
	CHECK_EQ(write_reg(c, ARM64_ID_AA64PFR0_EL1, 0), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(read_reg(c, ARM64_CTR_EL0), 0x84448004);
	c->sys.hcr_el2 = ARM64_HCR_TID2;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_CTR_EL0, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_CCSIDR_EL1, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(write_reg(c, ARM64_CSSELR_EL1, 0), ARM64_EXEC_EXCEPTION);
	c->sys.hcr_el2 = ARM64_HCR_TID1;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_REVIDR_EL1, 0)), ARM64_EXEC_EXCEPTION);
	c->sys.hcr_el2 = ARM64_HCR_TACR;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_ACTLR_EL1, 0)), ARM64_EXEC_EXCEPTION);

	/* CPACR_EL1: CPTR_EL2.TCPAC first, then CPTR_EL3.TCPAC. */
	c->sys.hcr_el2 = 0;
	c->sys.cptr_el2 = ARM64_CPTR_TCPAC;
	c->sys.cptr_el3 = ARM64_CPTR_TCPAC;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_CPACR_EL1, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 2);
	c->sys.cptr_el2 = 0;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_CPACR_EL1, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 3);

	/* Secure EL1 has no EL2: the HCR_EL2 traps do not apply. */
	c->sys.cptr_el3 = 0;
	c->sys.scr_el3 = 0;
	c->sys.hcr_el2 = ARM64_HCR_TVM | ARM64_HCR_TID3;
	CHECK_EQ(write_reg(c, ARM64_SCTLR_EL1, 0x00c50838), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, ARM64_ID_AA64PFR0_EL1), 0x2222);
	c->sys.cptr_el2 = ARM64_CPTR_TCPAC;
	CHECK_EQ(read_reg(c, ARM64_CPACR_EL1), 0);
	destroy(c);
}

static void test_traps_to_el3(void)
{
	Arm64Cpu *c = make_a53(0x80000000);

	c->sys.scr_el3 = ARM64_SCR_NS;
	c->el = 2;
	c->sys.cptr_el3 = ARM64_CPTR_TCPAC;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_CPTR_EL2, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 3);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_CPACR_EL1, 0)), ARM64_EXEC_EXCEPTION);
	c->sys.cptr_el3 = 0;
	c->sys.mdcr_el3 = ARM64_MDCR_TDA;
	CHECK_EQ(write_reg(c, ARM64_MDCR_EL2, 0), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 3);
	destroy(c);
}

static void test_traps_from_el0(void)
{
	Arm64Cpu *c = make_a53(0x80000000);

	c->sys.scr_el3 = ARM64_SCR_NS;
	c->el = 0;

	/* SCTLR_EL1.UMA and UCT are 0 after reset. */
	expect_trap(c, 0xd5034fdf, 1, 0x620cd3fe); /* MSR DAIFSet, #0xf */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_DAIF, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_CTR_EL0, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 1);

	/* HCR_EL2.TGE routes them to EL2. */
	c->sys.hcr_el2 = ARM64_HCR_TGE;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_DAIF, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 2);
	expect_trap(c, 0xd5034fdf, 2, 0x620cd3fe);
	c->sys.scr_el3 = 0;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_DAIF, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 1);
	c->sys.scr_el3 = ARM64_SCR_NS;

	/* With UCT set, CTR_EL0 still traps on HCR_EL2.TID2. */
	c->sys.sctlr[1] |= ARM64_SCTLR_UCT;
	c->sys.hcr_el2 = ARM64_HCR_TID2;
	CHECK_EQ(arm64_execute(c, mrs(ARM64_CTR_EL0, 0)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 2);
	destroy(c);
}

static void test_virtual_ids(void)
{
	Arm64Cpu *c = make_a53(0x80000001);

	CHECK_EQ(write_reg(c, ARM64_VPIDR_EL2, 0x1234), ARM64_EXEC_OK);
	CHECK_EQ(write_reg(c, ARM64_VMPIDR_EL2, 0x80000101), ARM64_EXEC_OK);

	/* EL1 with EL2 enabled reads the values that EL2 chose. */
	c->sys.scr_el3 = ARM64_SCR_NS;
	c->el = 1;
	CHECK_EQ(read_reg(c, ARM64_MIDR_EL1), 0x1234);
	CHECK_EQ(read_reg(c, ARM64_MPIDR_EL1), 0x80000101);
	c->el = 2;
	CHECK_EQ(read_reg(c, ARM64_MIDR_EL1), 0x410fd034);
	CHECK_EQ(read_reg(c, ARM64_MPIDR_EL1), 0x80000001);
	c->sys.scr_el3 = 0;
	c->el = 1;
	CHECK_EQ(read_reg(c, ARM64_MIDR_EL1), 0x410fd034);
	destroy(c);
}

static void test_msr_immediate(void)
{
	Arm64Cpu *c = make_a53(0x80000000);

	c->daif = 0;
	CHECK_EQ(arm64_execute(c, 0xd5034fdf), ARM64_EXEC_OK); /* DAIFSet #15 */
	CHECK_EQ(c->daif, 0x3c0);
	CHECK_EQ(arm64_execute(c, 0xd50342ff), ARM64_EXEC_OK); /* DAIFClr #2 */
	CHECK_EQ(c->daif, 0x340);
	CHECK_EQ(arm64_execute(c, 0xd50342df), ARM64_EXEC_OK); /* DAIFSet #2 */
	CHECK_EQ(c->daif, 0x3c0); /* added to the mask that is there */
	CHECK_EQ(arm64_execute(c, 0xd50040bf), ARM64_EXEC_OK); /* SPSel #0 */
	CHECK(!c->sp_sel);
	CHECK_EQ(arm64_execute(c, 0xd50041bf), ARM64_EXEC_OK); /* SPSel #1 */
	CHECK(c->sp_sel);

	/* DAIF at EL0 with SCTLR_EL1.UMA; SPSel is UNDEFINED there. */
	c->el = 0;
	c->sys.sctlr[1] |= ARM64_SCTLR_UMA;
	CHECK_EQ(arm64_execute(c, 0xd50342ff), ARM64_EXEC_OK); /* DAIFClr #2 */
	CHECK_EQ(arm64_execute(c, 0xd50041bf), ARM64_EXEC_UNDEFINED);

	/* Fields of newer features, and Rt != 31. */
	c->el = 1;
	CHECK_EQ(arm64_execute(c, 0xd500419f), ARM64_EXEC_UNDEFINED); /* PAN */
	CHECK_EQ(arm64_execute(c, 0xd500401f), ARM64_EXEC_UNDEFINED); /* CFINV */
	CHECK_EQ(arm64_execute(c, 0xd503413f), ARM64_EXEC_UNDEFINED); /* SSBS */
	CHECK_EQ(arm64_execute(c, 0xd50342fe), ARM64_EXEC_UNDEFINED); /* Rt 30 */
	destroy(c);
}

static void test_implementation_defined(void)
{
	Arm64Cpu *c = make_a53(0x80000000);
	uint64_t value;

	/* EL3 writes CPUECTLR_EL1 freely. */
	CHECK_EQ(write_reg(c, CPUECTLR_EL1, 0x40), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, CPUECTLR_EL1), 0x40);

	/* Lower levels need the CPUECTLR bit (1) of ACTLR_EL3 and EL2. */
	c->sys.scr_el3 = ARM64_SCR_NS;
	c->el = 2;
	CHECK_EQ(write_reg(c, CPUECTLR_EL1, 0), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 3);
	c->sys.actlr[3] = 1u << 1;
	CHECK_EQ(write_reg(c, CPUECTLR_EL1, 0), ARM64_EXEC_OK);
	c->el = 1;
	CHECK_EQ(write_reg(c, CPUECTLR_EL1, 0), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, 2);
	c->sys.actlr[2] = 1u << 1;
	CHECK_EQ(write_reg(c, CPUECTLR_EL1, 0x40), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, CPUECTLR_EL1), 0x40);
	c->sys.actlr[2] = 0;
	c->sys.scr_el3 = 0; /* Secure EL1: there is no EL2 to ask */
	CHECK_EQ(write_reg(c, CPUECTLR_EL1, 0), ARM64_EXEC_OK);

	/* No write changes L2CTLR_EL1; CBAR_EL1 is read-only. */
	c->el = 3;
	CHECK_EQ(write_reg(c, L2CTLR_EL1, 0x22), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, L2CTLR_EL1), 0x03000000);
	CHECK_EQ(write_reg(c, CBAR_EL1, 0), ARM64_EXEC_UNDEFINED);
	CHECK(!arm64_cpu_write_sysreg(c, CBAR_EL1, 0));
	CHECK(arm64_cpu_write_sysreg(c, L2CTLR_EL1, 0));
	CHECK(arm64_cpu_read_sysreg(c, L2CTLR_EL1, &value));
	CHECK_EQ(value, 0x03000000);

	/*
	 * The error syndrome registers record no errors. EL1 may write them,
	 * whatever ACTLR says, and a write of any value clears them: to 0,
	 * or to 0x10000000 for L2MERRSR_EL1 (sections 4.3.80 and 4.3.81).
	 */
	c->sys.actlr[3] = 0;
	c->sys.scr_el3 = ARM64_SCR_NS;
	c->el = 1;
	CHECK_EQ(read_reg(c, CPUMERRSR_EL1), 0);
	CHECK_EQ(read_reg(c, L2MERRSR_EL1), 0);
	CHECK_EQ(write_reg(c, CPUMERRSR_EL1, 0xffffffffffffffff), ARM64_EXEC_OK);
	CHECK_EQ(write_reg(c, L2MERRSR_EL1, 0xffffffffffffffff), ARM64_EXEC_OK);
	CHECK_EQ(read_reg(c, CPUMERRSR_EL1), 0);
	CHECK_EQ(read_reg(c, L2MERRSR_EL1), 0x10000000);

	/* None of them exists at EL0. */
	c->el = 0;
	CHECK_EQ(arm64_execute(c, mrs(CPUECTLR_EL1, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(L2MERRSR_EL1, 0)), ARM64_EXEC_UNDEFINED);
	destroy(c);
}

static void test_unimplemented_bits_stop(void)
{
	static const Arm64Config config = {
		.reset_pc = RESET_PC,
		.highest_el = 3,
		.model = &arm64_cortex_a53_bcm2837,
		.mpidr = 0x80000000,
	};
	Machine *m = machine_create();
	Cpu *base = arm64_cpu_create(&config);
	Arm64Cpu *c = arm64_cpu_of(base);

	CHECK_EQ(bus_map_ram(m->bus, "ram", 0, 0x100000), BUS_OK);
	CHECK(machine_add_cpu(m, base));

	/* Writing the SCTLR of another level is fine. */
	c->x[1] = ARM64_SCTLR_M;
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_SCTLR_EL1, 1)),
	         MACHINE_STOP_LIMIT);
	CHECK_EQ(c->sys.sctlr[1], ARM64_SCTLR_M);

	/* Turning on the MMU of the current level is not implemented yet. */
	c->x[1] = 0x00c50838 | ARM64_SCTLR_M;
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_SCTLR_EL3, 1)),
	         MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "is not implemented yet (turning on the "
	                              "MMU with SCTLR_EL3)") != NULL);
	CHECK_EQ(c->sys.sctlr[3], 0x00c50838);

	c->x[1] = 0x00c50838 | ARM64_SCTLR_EE;
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_SCTLR_EL3, 1)),
	         MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "(big-endian data") != NULL);

	c->el = 2;
	c->x[1] = ARM64_SCTLR_M;
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_SCTLR_EL1, 1)),
	         MACHINE_STOP_LIMIT);
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_SCTLR_EL2, 1)),
	         MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "(turning on the MMU with SCTLR_EL2)") !=
	      NULL);
	c->el = 1;
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_SCTLR_EL1, 1)),
	         MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "(turning on the MMU with SCTLR_EL1)") !=
	      NULL);
	c->el = 3;

	c->x[1] = 3; /* RR: request a warm reset */
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_RMR_EL3, 1)), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "(reset through RMR_EL3)") != NULL);
	c->x[1] = 0; /* AA64 = 0: the next reset would enter AArch32 */
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_RMR_EL3, 1)), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "(reset through RMR_EL3)") != NULL);
	c->x[1] = 1;
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_RMR_EL3, 1)), MACHINE_STOP_LIMIT);

	/* A register of the core that is not implemented yet is named. */
	CHECK_EQ(arm64_test_step(m, c, mrs(ARM64_SYSREG(3, 3, 14, 0, 0), 0)),
	         MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message,
	             "is not implemented yet (System register CNTFRQ_EL0)") !=
	      NULL);

	/* The detail of one instruction does not stay for the next one. */
	CHECK_EQ(arm64_test_step(m, c, 0x1e622820), /* FADD D0, D1, D2 */
	         MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "is not implemented yet") != NULL);
	CHECK(strstr(m->stop_message, "System register") == NULL);

	/* A trap stops the machine too, until exceptions are implemented. */
	c->sys.scr_el3 = ARM64_SCR_NS;
	c->sys.hcr_el2 = ARM64_HCR_TVM;
	c->el = 1;
	CHECK_EQ(arm64_test_step(m, c, msr(ARM64_SCTLR_EL1, 1)),
	         MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "takes an exception to EL2, ESR "
	                              "0x62300420") != NULL);
	machine_destroy(m);
}

static void test_undefined_encodings(void)
{
	Arm64Cpu *c = make_a53(0x80000000);

	/* D23.3: op1 == 0 and CRm == 0 outside the ID registers. */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(3, 0, 0, 0, 1), 0)),
	         ARM64_EXEC_UNDEFINED);
	/* The GIC CPU interface: the BCM2837 has none. ICC_SRE_EL1. */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(3, 0, 12, 12, 5), 0)),
	         ARM64_EXEC_UNDEFINED);
	/* A seventh breakpoint: the Cortex-A53 has six. DBGBVR6_EL1. */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(2, 0, 0, 6, 4), 0)),
	         ARM64_EXEC_UNDEFINED);
	/* Next to the reserved ID space: ZCR_EL1 (FEAT_SVE), and op1 == 1. */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(3, 0, 1, 2, 0), 0)),
	         ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(3, 1, 0, 2, 0), 0)),
	         ARM64_EXEC_UNDEFINED);
	/* A register of a newer feature: SCTLR_EL12 (FEAT_VHE). */
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(3, 5, 1, 0, 0), 0)),
	         ARM64_EXEC_UNDEFINED);
	destroy(c);
}

static void test_without_a_model(void)
{
	/* A core with no model: ID registers read as 0. */
	static const Arm64Config config = {.reset_pc = 0, .highest_el = 3};
	Arm64Cpu *c = arm64_cpu_of(arm64_cpu_create(&config));

	CHECK_EQ(read_reg(c, ARM64_MIDR_EL1), 0);
	CHECK_EQ(read_reg(c, ARM64_CCSIDR_EL1), 0);
	CHECK_EQ(read_reg(c, ARM64_SCTLR_EL1), 0);
	CHECK_EQ(arm64_execute(c, mrs(CPUECTLR_EL1, 0)), ARM64_EXEC_UNDEFINED);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_SYSREG(3, 3, 14, 0, 0), 0)),
	         ARM64_EXEC_UNDEFINED);
	CHECK(arm64_sysreg_name(c, CPUECTLR_EL1) == NULL);
	CHECK(strcmp(arm64_sysreg_name(c, ARM64_SCTLR_EL1), "SCTLR_EL1") == 0);
	destroy(c);
}

static void test_without_el2(void)
{
	/* A core with EL1 at most: nothing traps to EL2. */
	static const Arm64Config config = {.reset_pc = 0, .highest_el = 1};
	Arm64Cpu *c = arm64_cpu_of(arm64_cpu_create(&config));

	CHECK_EQ(c->el, 1);
	c->sys.hcr_el2 = ARM64_HCR_TID3 | ARM64_HCR_TVM;
	CHECK_EQ(read_reg(c, ARM64_ID_AA64PFR0_EL1), 0);
	CHECK_EQ(write_reg(c, ARM64_SCTLR_EL1, 0), ARM64_EXEC_OK);
	CHECK_EQ(arm64_execute(c, mrs(ARM64_HCR_EL2, 0)), ARM64_EXEC_UNDEFINED);
	destroy(c);
}

static void test_impdef_limit(void)
{
	static const Arm64ImpDefReg regs[ARM64_MAX_IMPDEF + 1];
	static const Arm64CpuModel most = {
		.name = "most",
		.impdef = regs,
		.num_impdef = ARM64_MAX_IMPDEF,
	};
	static const Arm64CpuModel too_many = {
		.name = "too many",
		.impdef = regs,
		.num_impdef = ARM64_MAX_IMPDEF + 1,
	};
	Arm64Config config = {.highest_el = 3, .model = &most};
	Cpu *base = arm64_cpu_create(&config);

	CHECK(base != NULL);
	if (base != NULL)
		base->ops->destroy(base);
	config.model = &too_many;
	CHECK(arm64_cpu_create(&config) == NULL);
}

static void test_direct_access(void)
{
	Arm64Cpu *c = make_a53(0x80000000);
	uint64_t value;

	/* Without the checks of MRS and MSR, but not past read-only. */
	c->sp_sel = false;
	CHECK(arm64_cpu_write_sysreg(c, ARM64_SP_EL0, 0x1000));
	CHECK(arm64_cpu_read_sysreg(c, ARM64_SP_EL0, &value));
	CHECK_EQ(value, 0x1000);
	CHECK(arm64_cpu_write_sysreg(c, ARM64_SCTLR_EL3, ARM64_SCTLR_M));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_MIDR_EL1, 0));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_SYSREG(3, 0, 0, 0, 1), 0));
	CHECK(!arm64_cpu_read_sysreg(c, ARM64_SYSREG(3, 0, 0, 0, 1), &value));
	CHECK(arm64_cpu_read_sysreg(c, ARM64_MPIDR_EL1, &value));
	CHECK_EQ(value, 0x80000000);
	CHECK(arm64_cpu_write_sysreg(c, ARM64_DAIF, 0xffffffff));
	CHECK_EQ(c->daif, 0x3c0); /* still only the bits that exist */
	CHECK(arm64_cpu_write_sysreg(c, ARM64_SP_EL1, 0x2100));
	CHECK_EQ(c->sp_el[1], 0x2100);
	CHECK(strcmp(arm64_sysreg_name(c, CPUECTLR_EL1), "CPUECTLR_EL1") == 0);
	destroy(c);
}

int main(void)
{
	RUN_TEST(test_encodings);
	RUN_TEST(test_id_registers);
	RUN_TEST(test_reset_values);
	RUN_TEST(test_pstate_registers);
	RUN_TEST(test_access_by_el);
	RUN_TEST(test_traps_to_el2);
	RUN_TEST(test_traps_to_el3);
	RUN_TEST(test_traps_from_el0);
	RUN_TEST(test_virtual_ids);
	RUN_TEST(test_msr_immediate);
	RUN_TEST(test_implementation_defined);
	RUN_TEST(test_unimplemented_bits_stop);
	RUN_TEST(test_undefined_encodings);
	RUN_TEST(test_without_a_model);
	RUN_TEST(test_without_el2);
	RUN_TEST(test_impdef_limit);
	RUN_TEST(test_direct_access);
	return test_summary();
}
