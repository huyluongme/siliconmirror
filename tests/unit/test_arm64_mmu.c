/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the MMU of the AArch64 model (src/cpu/arm64/mmu.c), the
 * data accesses through it (mem.c) and the System instructions DC, IC,
 * AT and TLBI (sysinsn.c). Section numbers refer to the Arm Architecture
 * Reference Manual, ARM DDI 0487 M.d.
 *
 * The tests build translation tables in RAM, then call arm64_translate()
 * or run instructions on a Cortex-A53, whose physical addresses have 40
 * bits and which has the 4KB and 64KB granules.
 */

#include <stdint.h>
#include <string.h>

#include "arm64_test.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"

#define VBAR(el) (UINT64_C(0x1000) * (el))
#define SAME 0x200  /* vector offset: current level with SP_ELx */
#define LOWER 0x400 /* lower level using AArch64 */
#define CODE RESET_PC
#define DATA 0x40000

/* Tables for virtual addresses 0 to 2MB: L1[0] -> L2, L2[0] -> L3. */
#define L1 0x10000
#define L2 0x11000
#define L3 0x12000

/* Descriptor fields, D8.3. */
#define AF (UINT64_C(1) << 10)
#define PAGE(pa) ((uint64_t)(pa) | 3 | AF)
#define BLOCK(pa) ((uint64_t)(pa) | 1 | AF)
#define TABLE(pa) ((uint64_t)(pa) | 3)
#define ATTR(n) ((uint64_t)(n) << 2)
#define NS (UINT64_C(1) << 5)
#define AP_EL0 (UINT64_C(1) << 6)
#define AP_RO (UINT64_C(1) << 7)
#define SH(n) ((uint64_t)(n) << 8)
#define PXN (UINT64_C(1) << 53)
#define UXN (UINT64_C(1) << 54)
#define PXN_TABLE (UINT64_C(1) << 59)
#define UXN_TABLE (UINT64_C(1) << 60)
#define AP_TABLE0 (UINT64_C(1) << 61)
#define AP_TABLE1 (UINT64_C(1) << 62)
#define NS_TABLE (UINT64_C(1) << 63)

/*
 * MAIR_ELx: attribute 0 Normal Write-Back, 1 Device-nGnRnE, 2 Normal
 * Non-cacheable, 3 Device-nGnRE, 4 a Device encoding with bit 0 set.
 */
#define MAIR UINT64_C(0x05044400ff)

/* T0SZ = T1SZ = 25: 39-bit addresses, from level 1; 40-bit {I}PS. */
#define TCR_EL1 UINT64_C(0x280190019)
#define TCR_ELX UINT64_C(0x20019)

#define ESR(ec, iss) ((uint32_t)(ec) << 26 | UINT32_C(1) << 25 | (iss))

/*
 * The ISS of a trapped System instruction with Rt = 1, D24.2.44: Op0 in
 * bits 21:20, Op2 19:17, Op1 16:14, CRn 13:10, Rt 9:5, CRm 4:1.
 */
#define ISS_SYS(op1, crn, crm, op2)                                            \
	(1u << 20 | (op2) << 17 | (op1) << 14 | (crn) << 10 | 1u << 5 | (crm) << 1)
#define ISS_DC_ZVA ISS_SYS(3u, 7u, 4u, 1u)
#define ISS_DC_CVAC ISS_SYS(3u, 7u, 10u, 1u)
#define ISS_IC_IVAU ISS_SYS(3u, 7u, 5u, 1u)

/* Instructions. */
#define LDR 0xf9400020u  /* LDR X0, [X1] */
#define STR 0xf9000020u  /* STR X0, [X1] */
#define LDRB 0x39400020u /* LDRB W0, [X1] */
#define BR 0xd61f0020u   /* BR X1 */
#define ERET 0xd69f03e0u
#define NOP 0xd503201fu

/* SYS #op1, Crn, Crm, #op2, X1. */
#define SYS(op1, crn, crm, op2)                                                \
	(0xd5080000u | (op1) << 16 | (crn) << 12 | (crm) << 8 | (op2) << 5 | 1u)

#define DC_ZVA SYS(3, 7, 4, 1)
#define DC_IVAC SYS(0, 7, 6, 1)
#define DC_ISW SYS(0, 7, 6, 2)
#define DC_CVAC SYS(3, 7, 10, 1)
#define DC_CIVAC SYS(3, 7, 14, 1)
#define IC_IALLU SYS(0, 7, 5, 0)
#define IC_IVAU SYS(3, 7, 5, 1)
#define AT_S1E1R SYS(0, 7, 8, 0)
#define AT_S1E1W SYS(0, 7, 8, 1)
#define AT_S1E0R SYS(0, 7, 8, 2)
#define AT_S1E2R SYS(4, 7, 8, 0)
#define AT_S12E1R SYS(4, 7, 8, 4)
#define AT_S1E3R SYS(6, 7, 8, 0)
#define TLBI_VMALLE1 SYS(0, 8, 7, 0)
#define TLBI_ALLE2 SYS(4, 8, 7, 0)
#define TLBI_ALLE1 SYS(4, 8, 7, 4)
#define TLBI_ALLE3 SYS(6, 8, 7, 0)

static uint32_t msr(uint16_t encoding, unsigned t)
{
	return 0xd5000000u | (uint32_t)encoding << 5 | t;
}

static Arm64Cpu *add_cpu(Machine *m, const Arm64CpuModel *model, uint64_t mpidr)
{
	Arm64Config config = {
		.reset_pc = RESET_PC,
		.highest_el = 3,
		.model = model,
		.mpidr = mpidr,
	};
	Cpu *base = arm64_cpu_create(&config);
	Arm64Cpu *c;

	CHECK(base != NULL);
	CHECK(machine_add_cpu(m, base));
	c = arm64_cpu_of(base);
	for (unsigned el = 1; el <= 3; el++)
		c->sys.vbar[el] = VBAR(el);
	return c;
}

/* A core of model at EL3, with 1MB of RAM at 0. */
static Machine *machine_with(const Arm64CpuModel *model, Arm64Cpu **c)
{
	Machine *m = machine_create();

	CHECK(m != NULL);
	CHECK_EQ(bus_map_ram(m->bus, "ram", 0, 0x100000), BUS_OK);
	*c = add_cpu(m, model, 0);
	return m;
}

/* A Cortex-A53 at EL3, with 1MB of RAM at 0. */
static Machine *machine(Arm64Cpu **c)
{
	return machine_with(&arm64_cortex_a53_bcm2837, c);
}

static void put(Machine *m, uint64_t pa, uint64_t value)
{
	CHECK_EQ(bus_write(m->bus, pa, 8, value), BUS_OK);
}

static uint64_t get(Machine *m, uint64_t pa, unsigned size)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(m->bus, pa, size, &value), BUS_OK);
	return value;
}

/* The descriptor of the 4KB page at va, below 2MB. */
static void map(Machine *m, uint64_t va, uint64_t desc)
{
	put(m, L3 + (va >> 12) * 8, desc);
}

/*
 * Put the PE at el, Non-secure unless secure, with stage 1 translation
 * on for the regime of el through the tables at L1. mmu_on() also makes
 * the tables, which map the code, and empties the TLB.
 */
static void registers(Arm64Cpu *c, unsigned el, bool secure)
{
	unsigned r = el > 1 ? el : 1;

	c->sys.scr_el3 = ARM64_SCR_RW | (secure ? 0 : ARM64_SCR_NS);
	c->sys.hcr_el2 = ARM64_HCR_RW;
	c->sys.tcr[r] = r == 1 ? TCR_EL1 : TCR_ELX;
	c->sys.ttbr0[r] = L1;
	c->sys.mair[r] = MAIR;
	c->sys.sctlr[r] |= ARM64_SCTLR_M;
	c->el = el;
	c->sp_sel = el > 0;
	c->daif = ARM64_DAIF_D | ARM64_DAIF_A | ARM64_DAIF_I | ARM64_DAIF_F;
	c->pc = CODE;
}

static void mmu_on(Machine *m, Arm64Cpu *c, unsigned el, bool secure)
{
	registers(c, el, secure);
	put(m, L1, TABLE(L2));
	put(m, L2, TABLE(L3));
	map(m, CODE, PAGE(CODE) | (el == 0 ? AP_EL0 | AP_RO : 0));
	arm64_tlb_flush(c);
}

static Arm64Translation translate(Arm64Cpu *c, uint64_t va, Arm64Access access,
                                  unsigned el)
{
	Arm64Translation t;

	CHECK_EQ(arm64_translate(c, va, access, el, true, &t), ARM64_EXEC_OK);
	return t;
}

/* Check that va translates to pa, or faults; with the caller's line. */
#define maps(c, va, el, pa) maps_at(__LINE__, c, va, el, pa)
#define faults(c, va, el, fault, level)                                        \
	faults_at(__LINE__, c, va, el, fault, level)

static void maps_at(int line, Arm64Cpu *c, uint64_t va, unsigned el,
                    uint64_t pa)
{
	Arm64Translation t = translate(c, va, ARM64_ACCESS_READ, el);

	if (t.fault != ARM64_FAULT_NONE || t.pa != pa) {
		fprintf(stderr,
		        "%s:%d: 0x%" PRIx64 " gives fault %d at %u, PA 0x%" PRIx64
		        ", not PA 0x%" PRIx64 "\n",
		        __FILE__, line, va, (int)t.fault, t.level, t.pa, pa);
		test_failures++;
	}
}

static void faults_at(int line, Arm64Cpu *c, uint64_t va, unsigned el,
                      Arm64Fault fault, unsigned level)
{
	Arm64Translation t = translate(c, va, ARM64_ACCESS_READ, el);

	if (t.fault != fault || t.level != level) {
		fprintf(stderr,
		        "%s:%d: 0x%" PRIx64 " gives fault %d at %u, not %d at %u\n",
		        __FILE__, line, va, (int)t.fault, t.level, (int)fault, level);
		test_failures++;
	}
}

static void unimplemented(Arm64Cpu *c, uint64_t va, unsigned el,
                          const char *detail)
{
	Arm64Translation t;

	arm64_tlb_flush(c);
	CHECK_EQ(arm64_translate(c, va, ARM64_ACCESS_READ, el, true, &t),
	         ARM64_EXEC_UNIMPLEMENTED);
	CHECK(strcmp(c->detail, detail) == 0);
}

/* Run insn, which must not stop the machine. */
static void step(Machine *m, Arm64Cpu *c, uint32_t insn)
{
	CHECK_EQ(arm64_test_step(m, c, insn), MACHINE_STOP_LIMIT);
}

/* Check the synchronous exception that the last step took. */
static void taken(Arm64Cpu *c, unsigned el, uint64_t offset, uint32_t esr,
                  uint64_t elr)
{
	CHECK_EQ(c->el, el);
	CHECK_EQ(c->pc, VBAR(el) + offset);
	CHECK_EQ(c->sys.esr[el], esr);
	CHECK_EQ(c->sys.elr[el], elr);
}

/* Check that insn is UNDEFINED at the current level, EL1 or above. */
static void undefined(Machine *m, Arm64Cpu *c, uint32_t insn)
{
	unsigned el = c->el;

	step(m, c, insn);
	taken(c, el, SAME, ESR(0, 0), CODE);
	c->el = el;
	c->pc = CODE;
}

/*
 * Stage 1 disabled: the physical address is the virtual address, Device
 * memory for data and Normal memory for instructions, D8.2.12.
 */
static void test_disabled(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	Arm64Translation t;

	t = translate(c, 0x1234, ARM64_ACCESS_READ, 3);
	CHECK_EQ(t.fault, ARM64_FAULT_NONE);
	CHECK_EQ(t.pa, 0x1234);
	CHECK_EQ(t.attr, 0x00); /* Device-nGnRnE */
	CHECK_EQ(t.sh, 2);
	CHECK(!t.ns);
	t = translate(c, 0x1234, ARM64_ACCESS_IFETCH, 3);
	CHECK_EQ(t.attr, 0x44); /* Normal */

	/* Above PAMax, 40 bits: an Address size fault, level 0. */
	maps(c, 0xffffffffff, 3, 0xffffffffff);
	faults(c, UINT64_C(1) << 40, 3, ARM64_FAULT_ADDRESS_SIZE, 0);
	faults(c, UINT64_C(0x8000000000000000), 3, ARM64_FAULT_ADDRESS_SIZE, 0);
	faults(c, UINT64_C(0x0100000000001000), 3, ARM64_FAULT_ADDRESS_SIZE, 0);
	c->sys.tcr[3] = UINT64_C(1) << 20; /* TBI */
	maps(c, UINT64_C(0x0100000000001000), 3, 0x1000);
	faults(c, UINT64_C(0x0080000000001000), 3, ARM64_FAULT_ADDRESS_SIZE, 0);

	/* Device memory: unaligned accesses and DC ZVA fault. */
	CHECK_EQ(arm64_translate(c, 0x1001, ARM64_ACCESS_READ, 3, false, &t),
	         ARM64_EXEC_OK);
	CHECK_EQ(t.fault, ARM64_FAULT_ALIGNMENT);
	CHECK_EQ(arm64_translate(c, 0x1001, ARM64_ACCESS_IFETCH, 3, false, &t),
	         ARM64_EXEC_OK);
	CHECK_EQ(t.fault, ARM64_FAULT_NONE);
	t = translate(c, 0x1000, ARM64_ACCESS_DC_ZVA, 3);
	CHECK_EQ(t.fault, ARM64_FAULT_ALIGNMENT);

	/* EL1&0 in Non-secure state; TBI0 and TBI1 by bit 55. */
	c->sys.scr_el3 = ARM64_SCR_NS | ARM64_SCR_RW;
	c->sys.hcr_el2 = ARM64_HCR_RW;
	t = translate(c, 0x1000, ARM64_ACCESS_READ, 1);
	CHECK(t.ns);
	c->sys.tcr[1] = UINT64_C(1) << 37; /* TBI0 */
	maps(c, UINT64_C(0x5a00000000001000), 0, 0x1000);
	c->sys.tcr[1] = UINT64_C(1) << 38; /* TBI1 */
	faults(c, UINT64_C(0x5a00000000001000), 0, ARM64_FAULT_ADDRESS_SIZE, 0);

	/* HCR_EL2.TGE turns stage 1 off for EL1&0, but not with EL2 off. */
	c->sys.sctlr[1] |= ARM64_SCTLR_M;
	c->sys.tcr[1] = TCR_EL1;
	c->sys.hcr_el2 |= ARM64_HCR_TGE;
	maps(c, 0x1000, 0, 0x1000);
	c->sys.scr_el3 = ARM64_SCR_RW; /* Secure: EL2 is not enabled */
	faults(c, 0x1000, 0, ARM64_FAULT_TRANSLATION, 1);
	machine_destroy(m);
}

/* The 4KB granule, from level 0 to 3, in the EL3 regime. */
static void test_walk_4k(void)
{
	const uint64_t gb = UINT64_C(1) << 30, mb2 = UINT64_C(1) << 21;
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->sys.tcr[3] = 16 | 2 << 16; /* T0SZ = 16: 48 bits, from level 0 */
	c->sys.ttbr0[3] = 0x20000;
	c->sys.mair[3] = MAIR;
	c->sys.sctlr[3] |= ARM64_SCTLR_M;
	put(m, 0x20000, TABLE(0x21000));             /* L0[0] */
	put(m, 0x20008, BLOCK(0));                   /* L0[1]: no block */
	put(m, 0x21000, BLOCK(0));                   /* L1[0]: 1GB */
	put(m, 0x21008, TABLE(0x22000));             /* L1[1] */
	put(m, 0x21018, TABLE(UINT64_C(1) << 40));   /* L1[3] */
	put(m, 0x21020, BLOCK(UINT64_C(1) << 32));   /* L1[4] */
	put(m, 0x22000, BLOCK(0x200000) | 0x1ff000); /* L2[0]: 2MB */
	put(m, 0x22008, TABLE(0x23000) | 0xffc);     /* L2[1] */
	put(m, 0x23000, PAGE(0x5000));               /* L3[0] */
	put(m, 0x23010, BLOCK(0x6000));              /* L3[2]: reserved */
	put(m, 0x23018, PAGE(0x7000) & ~AF);         /* L3[3] */
	put(m, 0x23020, PAGE(UINT64_C(1) << 40));    /* L3[4] */
	arm64_tlb_flush(c);

	maps(c, 0x12345678, 3, 0x12345678);
	maps(c, gb | 0x154321, 3, 0x354321);
	maps(c, gb | mb2 | 0xabc, 3, 0x5abc);
	faults(c, gb | mb2 | 0x1000, 3, ARM64_FAULT_TRANSLATION, 3);
	faults(c, gb | mb2 | 0x2000, 3, ARM64_FAULT_TRANSLATION, 3);
	faults(c, gb | mb2 | 0x3000, 3, ARM64_FAULT_ACCESS_FLAG, 3);
	faults(c, gb | mb2 | 0x4000, 3, ARM64_FAULT_ADDRESS_SIZE, 3);
	faults(c, 2 * gb, 3, ARM64_FAULT_TRANSLATION, 1);
	faults(c, 3 * gb, 3, ARM64_FAULT_ADDRESS_SIZE, 1);
	maps(c, 4 * gb + 5, 3, (UINT64_C(1) << 32) + 5);
	faults(c, UINT64_C(1) << 39, 3, ARM64_FAULT_TRANSLATION, 0);
	faults(c, UINT64_C(1) << 48, 3, ARM64_FAULT_TRANSLATION, 0);

	/* The table base, and output addresses, within TCR_EL3.PS. */
	c->sys.ttbr0[3] = UINT64_C(1) << 40 | 0x20000;
	arm64_tlb_flush(c);
	faults(c, 0x1000, 3, ARM64_FAULT_ADDRESS_SIZE, 0);
	c->sys.ttbr0[3] = UINT64_C(0xffff000000020001); /* ASID and CnP */
	arm64_tlb_flush(c);
	maps(c, 0x1000, 3, 0x1000);
	c->sys.tcr[3] = 16; /* PS = 0: 32 bits */
	arm64_tlb_flush(c);
	faults(c, 4 * gb, 3, ARM64_FAULT_ADDRESS_SIZE, 1);
	c->sys.tcr[3] = 16 | 7 << 16; /* reserved: PAMax */
	arm64_tlb_flush(c);
	maps(c, 4 * gb, 3, UINT64_C(1) << 32);
	faults(c, 3 * gb, 3, ARM64_FAULT_ADDRESS_SIZE, 1);

	/* The start level and table size follow T0SZ. */
	c->sys.tcr[3] = 25 | 2 << 16; /* 39 bits: level 1 */
	c->sys.ttbr0[3] = 0x21000;
	arm64_tlb_flush(c);
	maps(c, 0x1234, 3, 0x1234);
	maps(c, gb | mb2 | 0x10, 3, 0x5010);
	faults(c, UINT64_C(1) << 39, 3, ARM64_FAULT_TRANSLATION, 0);
	c->sys.tcr[3] = 30 | 2 << 16;     /* 34 bits: level 1, 16 entries */
	c->sys.ttbr0[3] = 0x21000 | 0x78; /* aligned down to 128 bytes */
	arm64_tlb_flush(c);
	maps(c, 4 * gb + 1, 3, (UINT64_C(1) << 32) + 1);
	c->sys.tcr[3] = 34 | 2 << 16; /* 30 bits: level 2 */
	c->sys.ttbr0[3] = 0x22000;
	arm64_tlb_flush(c);
	maps(c, 0x54321, 3, 0x254321);
	maps(c, mb2 | 0x6, 3, 0x5006);
	c->sys.tcr[3] = 39 | 2 << 16; /* 25 bits: level 2, 16 entries */
	c->sys.ttbr0[3] = 0x22000 | 0x40;
	arm64_tlb_flush(c);
	maps(c, mb2 | 0x7, 3, 0x5007);
	faults(c, UINT64_C(1) << 25, 3, ARM64_FAULT_TRANSLATION, 0);

	/* What the Cortex-A53 does with these is not known. */
	c->sys.tcr[3] = 40;
	unimplemented(c, 0, 3, "TCR_EL3.T0SZ = 40, outside 16 to 39");
	c->sys.tcr[3] = 15;
	unimplemented(c, 0, 3, "TCR_EL3.T0SZ = 15, outside 16 to 39");
	c->sys.tcr[3] = 25 | 2 << 14;
	unimplemented(c, 0, 3,
	              "TCR_EL3.TG0 = 2, a granule that the core does not have");
	c->sys.tcr[3] = 25 | 3 << 14;
	unimplemented(c, 0, 3,
	              "TCR_EL3.TG0 = 3, a granule that the core does not have");
	machine_destroy(m);
}

/* The 64KB granule, in the EL2 regime. */
static void test_walk_64k(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->sys.scr_el3 = ARM64_SCR_NS | ARM64_SCR_RW;
	c->sys.tcr[2] = 29 | 1 << 14 | 2 << 16;
	c->sys.ttbr0[2] = 0x10000;
	c->sys.sctlr[2] |= ARM64_SCTLR_M;
	put(m, 0x10000, TABLE(0x20000) | 0xf000);  /* L2[0] */
	put(m, 0x10008, BLOCK(0x20000000));        /* L2[1]: 512MB */
	put(m, 0x20008, PAGE(0x30000));            /* L3[1] */
	put(m, 0x20010, PAGE(0x40000) | 0xf000);   /* L3[2]: no FEAT_LPA */
	put(m, 0x20018, BLOCK(0x50000));           /* L3[3]: reserved */
	put(m, 0x10010, BLOCK(UINT64_C(1) << 32)); /* L2[2]: PS, not IPS */
	put(m, 0x60000, BLOCK(0));
	arm64_tlb_flush(c);

	maps(c, 0x40000001, 2, (UINT64_C(1) << 32) + 1);
	maps(c, 0x13abc, 2, 0x33abc);
	maps(c, 0x2fffe, 2, 0x4fffe);
	maps(c, 0x21234567, 2, 0x21234567);
	faults(c, 0x30000, 2, ARM64_FAULT_TRANSLATION, 3);
	faults(c, 0x40000, 2, ARM64_FAULT_TRANSLATION, 3);
	faults(c, UINT64_C(1) << 35, 2, ARM64_FAULT_TRANSLATION, 0);

	/* 48 bits start at level 1, which has no blocks. */
	c->sys.tcr[2] = 16 | 1 << 14 | 2 << 16;
	c->sys.ttbr0[2] = 0x60000;
	arm64_tlb_flush(c);
	faults(c, 0x1000, 2, ARM64_FAULT_TRANSLATION, 1);
	/* 42 bits from level 2, 43 from level 1. */
	c->sys.tcr[2] = 22 | 1 << 14 | 2 << 16;
	c->sys.ttbr0[2] = 0x10000;
	arm64_tlb_flush(c);
	maps(c, 0x13abc, 2, 0x33abc);
	c->sys.tcr[2] = 21 | 1 << 14 | 2 << 16;
	c->sys.ttbr0[2] = 0x60000;
	arm64_tlb_flush(c);
	faults(c, 0x1000, 2, ARM64_FAULT_TRANSLATION, 1);
	machine_destroy(m);
}

/* EL1&0: TTBR0_EL1 and TTBR1_EL1, EPDn, TBIn, TG1 and IPS. */
static void test_el1_ranges(void)
{
	const uint64_t upper = UINT64_C(0xffffff8000000000);
	Arm64Cpu *c;
	Machine *m = machine(&c);

	mmu_on(m, c, 1, false);
	c->sys.ttbr1_el1 = 0x20000;
	put(m, L1 + 8, BLOCK(0x40000000));
	put(m, 0x20000, BLOCK(0x40000000));
	put(m, 0x20008, BLOCK(UINT64_C(1) << 32));
	arm64_tlb_flush(c);

	maps(c, 0x40001234, 1, 0x40001234);
	maps(c, upper | 0x1234, 1, 0x40001234);
	maps(c, upper | 0x40000005, 1, (UINT64_C(1) << 32) + 5);
	faults(c, UINT64_C(1) << 39, 1, ARM64_FAULT_TRANSLATION, 0);
	faults(c, UINT64_C(0xffffff0000000000), 1, ARM64_FAULT_TRANSLATION, 0);
	faults(c, UINT64_C(0x5a00000040000000), 1, ARM64_FAULT_TRANSLATION, 0);
	faults(c, UINT64_C(0x5affff8000000000), 1, ARM64_FAULT_TRANSLATION, 0);

	c->sys.tcr[1] = TCR_EL1 | UINT64_C(1) << 37; /* TBI0 */
	arm64_tlb_flush(c);
	maps(c, UINT64_C(0x5a00000040000000), 1, 0x40000000);
	faults(c, UINT64_C(0x5affff8000000000), 1, ARM64_FAULT_TRANSLATION, 0);
	c->sys.tcr[1] = TCR_EL1 | UINT64_C(1) << 38; /* TBI1 */
	arm64_tlb_flush(c);
	maps(c, UINT64_C(0x5affff8000000000), 1, 0x40000000);
	faults(c, UINT64_C(0x5a00000040000000), 1, ARM64_FAULT_TRANSLATION, 0);

	c->sys.tcr[1] = TCR_EL1 | UINT64_C(1) << 7; /* EPD0 */
	arm64_tlb_flush(c);
	faults(c, 0x40000000, 1, ARM64_FAULT_TRANSLATION, 0);
	maps(c, upper, 1, 0x40000000);
	c->sys.tcr[1] = TCR_EL1 | UINT64_C(1) << 23; /* EPD1 */
	arm64_tlb_flush(c);
	faults(c, upper, 1, ARM64_FAULT_TRANSLATION, 0);
	maps(c, 0x40000000, 1, 0x40000000);

	/* IPS, bits 34:32, sets the output size. */
	c->sys.tcr[1] = TCR_EL1 & ~(UINT64_C(7) << 32);
	arm64_tlb_flush(c);
	faults(c, upper | 0x40000000, 1, ARM64_FAULT_ADDRESS_SIZE, 1);

	c->sys.tcr[1] = TCR_EL1 & ~(UINT64_C(3) << 30); /* TG1 = 0, reserved */
	unimplemented(c, upper, 1,
	              "TCR_EL1.TG1 = 0, a granule that the core does not have");
	c->sys.tcr[1] = (TCR_EL1 & ~(UINT64_C(3) << 30)) | UINT64_C(1) << 30;
	unimplemented(c, upper, 1,
	              "TCR_EL1.TG1 = 1, a granule that the core does not have");
	c->sys.tcr[1] = TCR_EL1 | 32 << 16 | 0x3f;
	unimplemented(c, upper, 1, "TCR_EL1.T1SZ = 57, outside 16 to 39");
	unimplemented(c, 0, 1, "TCR_EL1.T0SZ = 63, outside 16 to 39");

	/* 64KB pages through TTBR1_EL1, from level 2. */
	c->sys.tcr[1] = TCR_EL1 | UINT64_C(3) << 30;
	c->sys.ttbr1_el1 = 0x30000;
	put(m, 0x30000 + 0x3fc * 8, BLOCK(0x60000000));
	arm64_tlb_flush(c);
	maps(c, UINT64_C(0xffffffff80000010), 1, 0x60000010);
	machine_destroy(m);
}

#define R 4
#define W 2
#define X 1

/* What an access from el may do at va: R, W and X. */
static unsigned rwx(Arm64Cpu *c, uint64_t va, unsigned el)
{
	static const Arm64Access accesses[3] = {
		ARM64_ACCESS_READ, ARM64_ACCESS_WRITE, ARM64_ACCESS_IFETCH};
	unsigned result = 0;

	for (unsigned i = 0; i < 3; i++) {
		Arm64Translation t = translate(c, va, accesses[i], el);

		if (t.fault == ARM64_FAULT_NONE)
			result |= R >> i;
		else
			CHECK_EQ(t.fault, ARM64_FAULT_PERMISSION);
	}
	return result;
}

/* AArch64_S1DirectBasePermissions(), J1.2.4.29. */
static void test_permissions(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	Arm64Translation t;

	mmu_on(m, c, 1, false);
	map(m, 0x1000, PAGE(0x1000));
	map(m, 0x2000, PAGE(0x2000) | AP_EL0);
	map(m, 0x3000, PAGE(0x3000) | AP_RO);
	map(m, 0x4000, PAGE(0x4000) | AP_RO | AP_EL0);
	map(m, 0x5000, PAGE(0x5000) | AP_RO | AP_EL0 | UXN);
	map(m, 0x6000, PAGE(0x6000) | PXN);
	/* Tables with APTable, XNTable and PXNTable, at 2MB, 4MB, 6MB. */
	put(m, L2 + 8, TABLE(0x13000) | AP_TABLE0);
	put(m, L2 + 16, TABLE(0x14000) | AP_TABLE1);
	put(m, L2 + 24, TABLE(0x15000) | UXN_TABLE | PXN_TABLE);
	put(m, 0x13000, PAGE(0) | AP_EL0);
	put(m, 0x14000, PAGE(0) | AP_EL0);
	put(m, 0x15000, PAGE(0) | AP_EL0 | AP_RO);
	put(m, L2 + 32, BLOCK(0) | AP_RO);

	CHECK_EQ(rwx(c, 0x1000, 1), R | W | X);
	CHECK_EQ(rwx(c, 0x1000, 0), X);     /* UXN alone controls execution */
	CHECK_EQ(rwx(c, 0x2000, 1), R | W); /* writable at EL0: PXN */
	CHECK_EQ(rwx(c, 0x2000, 0), R | W | X);
	CHECK_EQ(rwx(c, 0x3000, 1), R | X);
	CHECK_EQ(rwx(c, 0x3000, 0), X);
	CHECK_EQ(rwx(c, 0x4000, 1), R | X);
	CHECK_EQ(rwx(c, 0x4000, 0), R | X);
	CHECK_EQ(rwx(c, 0x5000, 0), R);
	CHECK_EQ(rwx(c, 0x5000, 1), R | X);
	CHECK_EQ(rwx(c, 0x6000, 1), R | W);
	CHECK_EQ(rwx(c, 0x200000, 1), R | W | X);
	CHECK_EQ(rwx(c, 0x200000, 0), X);
	CHECK_EQ(rwx(c, 0x400000, 1), R | X);
	CHECK_EQ(rwx(c, 0x400000, 0), R | X);
	CHECK_EQ(rwx(c, 0x600000, 1), R);
	CHECK_EQ(rwx(c, 0x600000, 0), R);
	t = translate(c, 0x3000, ARM64_ACCESS_WRITE, 1);
	CHECK_EQ(t.level, 3);
	t = translate(c, 0x800000, ARM64_ACCESS_WRITE, 1);
	CHECK_EQ(t.fault, ARM64_FAULT_PERMISSION);
	CHECK_EQ(t.level, 2);

	/* What the cache maintenance instructions need. */
	t = translate(c, 0x1000, ARM64_ACCESS_DC_CLEAN, 0);
	CHECK_EQ(t.fault, ARM64_FAULT_PERMISSION);
	t = translate(c, 0x4000, ARM64_ACCESS_DC_CLEAN, 0);
	CHECK_EQ(t.fault, ARM64_FAULT_NONE);
	t = translate(c, 0x3000, ARM64_ACCESS_DC_CLEAN, 1);
	CHECK_EQ(t.fault, ARM64_FAULT_NONE);
	t = translate(c, 0x3000, ARM64_ACCESS_DC_INVALIDATE, 1);
	CHECK_EQ(t.fault, ARM64_FAULT_PERMISSION);
	t = translate(c, 0x3000, ARM64_ACCESS_DC_ZVA, 1);
	CHECK_EQ(t.fault, ARM64_FAULT_PERMISSION);
	t = translate(c, 0x1000, ARM64_ACCESS_IC, 0);
	CHECK_EQ(t.fault, ARM64_FAULT_NONE);
	CHECK_EQ(t.pa, 0x1000);

	/* SCTLR_EL1.WXN: what can be written cannot be executed. */
	c->sys.sctlr[1] |= ARM64_SCTLR_WXN;
	arm64_tlb_flush(c);
	CHECK_EQ(rwx(c, 0x1000, 1), R | W);
	CHECK_EQ(rwx(c, 0x2000, 0), R | W);
	CHECK_EQ(rwx(c, 0x4000, 0), R | X);
	CHECK_EQ(rwx(c, 0x3000, 1), R | X);
	c->sys.sctlr[1] &= ~(uint64_t)ARM64_SCTLR_WXN;

	/* EL2: AP[2] and XN; AP[1], PXN, APTable[0], PXNTable are ignored. */
	mmu_on(m, c, 2, false);
	CHECK_EQ(rwx(c, 0x1000, 2), R | W | X);
	CHECK_EQ(rwx(c, 0x2000, 2), R | W | X);
	CHECK_EQ(rwx(c, 0x3000, 2), R | X);
	CHECK_EQ(rwx(c, 0x5000, 2), R);
	CHECK_EQ(rwx(c, 0x6000, 2), R | W | X);
	CHECK_EQ(rwx(c, 0x200000, 2), R | W | X);
	CHECK_EQ(rwx(c, 0x400000, 2), R | X);
	CHECK_EQ(rwx(c, 0x600000, 2), R);
	put(m, L2 + 24, TABLE(0x15000) | PXN_TABLE);
	arm64_tlb_flush(c);
	CHECK_EQ(rwx(c, 0x600000, 2), R | X);
	c->sys.sctlr[2] |= ARM64_SCTLR_WXN;
	arm64_tlb_flush(c);
	CHECK_EQ(rwx(c, 0x1000, 2), R | W);
	CHECK_EQ(rwx(c, 0x3000, 2), R | X);

	/* Secure EL3: NS, NSTable, and SCR_EL3.SIF for instructions. */
	mmu_on(m, c, 3, true);
	map(m, 0x2000, PAGE(0x2000) | NS);
	put(m, L2 + 8, TABLE(0x13000) | NS_TABLE);
	put(m, 0x13000, PAGE(0));
	arm64_tlb_flush(c);
	CHECK(!translate(c, 0x1000, ARM64_ACCESS_READ, 3).ns);
	CHECK(translate(c, 0x2000, ARM64_ACCESS_READ, 3).ns);
	CHECK(translate(c, 0x200000, ARM64_ACCESS_READ, 3).ns);
	CHECK_EQ(rwx(c, 0x2000, 3), R | W | X);
	c->sys.scr_el3 |= ARM64_SCR_SIF;
	arm64_tlb_flush(c);
	CHECK_EQ(rwx(c, 0x1000, 3), R | W | X);
	CHECK_EQ(rwx(c, 0x2000, 3), R | W);
	CHECK_EQ(rwx(c, 0x200000, 3), R | W);

	/* Secure EL1&0 too; SIF does not apply to Non-secure EL1&0. */
	mmu_on(m, c, 1, true);
	c->sys.scr_el3 |= ARM64_SCR_SIF;
	arm64_tlb_flush(c);
	CHECK(!translate(c, 0x1000, ARM64_ACCESS_READ, 1).ns);
	CHECK_EQ(rwx(c, 0x2000, 1), R | W);
	c->sys.scr_el3 |= ARM64_SCR_NS;
	arm64_tlb_flush(c);
	CHECK(translate(c, 0x1000, ARM64_ACCESS_READ, 1).ns);
	CHECK_EQ(rwx(c, 0x2000, 1), R | W | X);

	/* PXNTable alone leaves EL0 free to execute. */
	mmu_on(m, c, 1, false);
	put(m, L1, TABLE(L2) | PXN_TABLE);
	map(m, 0x1000, PAGE(0x1000) | AP_EL0 | AP_RO);
	CHECK_EQ(rwx(c, 0x1000, 1), R);
	CHECK_EQ(rwx(c, 0x1000, 0), R | X);
	machine_destroy(m);
}

/* The TLB keeps a translation until something empties it. */
static void test_tlb(void)
{
	static const uint16_t flushing[] = {
		ARM64_SCTLR_EL1, ARM64_TTBR0_EL1, ARM64_TTBR1_EL1, ARM64_TCR_EL1,
		ARM64_MAIR_EL1,  ARM64_SCTLR_EL2, ARM64_HCR_EL2,   ARM64_TTBR0_EL2,
		ARM64_TCR_EL2,   ARM64_MAIR_EL2,  ARM64_SCTLR_EL3, ARM64_SCR_EL3,
		ARM64_TTBR0_EL3, ARM64_TCR_EL3,   ARM64_MAIR_EL3,
	};
	uint64_t pa = 0x5000;
	Arm64Cpu *c, *other;
	Machine *m = machine(&c);

	other = add_cpu(m, &arm64_cortex_a53_bcm2837, 1);
	mmu_on(m, c, 1, false);
	mmu_on(m, other, 1, false);
	map(m, 0x1000, PAGE(pa));
	maps(c, 0x1000, 1, pa);
	maps(other, 0x1000, 1, pa);
	map(m, 0x1000, PAGE(0x6000));
	maps(c, 0x1000, 1, pa);
	step(m, c, TLBI_VMALLE1); /* empties both TLBs */
	maps(c, 0x1000, 1, 0x6000);
	maps(other, 0x1000, 1, 0x6000);

	/* Each register that controls translation empties the TLB. */
	c->el = 3;
	for (unsigned i = 0; i < ARRAY_SIZE(flushing); i++) {
		arm64_tlb_flush(c);
		map(m, 0x1000, PAGE(0x5000));
		maps(c, 0x1000, 1, 0x5000);
		map(m, 0x1000, PAGE(0x6000));
		CHECK(arm64_cpu_read_sysreg(c, flushing[i], &c->x[1]));
		c->pc = CODE;
		step(m, c, msr(flushing[i], 1));
		maps(c, 0x1000, 1, 0x6000);
	}
	/* Other registers do not; arm64_cpu_write_sysreg() does. */
	map(m, 0x1000, PAGE(0x7000));
	c->pc = CODE;
	step(m, c, msr(ARM64_VBAR_EL1, 1));
	maps(c, 0x1000, 1, 0x6000);
	CHECK(arm64_cpu_write_sysreg(c, ARM64_VBAR_EL1, 0));
	maps(c, 0x1000, 1, 0x6000);
	CHECK(arm64_cpu_write_sysreg(c, ARM64_TCR_EL1, TCR_EL1));
	maps(c, 0x1000, 1, 0x7000);

	/* A fault is not kept. */
	map(m, 0x9000, 0);
	faults(c, 0x9000, 1, ARM64_FAULT_TRANSLATION, 3);
	map(m, 0x9000, PAGE(0x9000));
	maps(c, 0x9000, 1, 0x9000);

	/* Entries for one index, and for each regime, stay apart. */
	map(m, 0x101000, PAGE(0xa000));
	maps(c, 0x101000, 1, 0xa000);
	maps(c, 0x1000, 1, 0x7000);
	maps(c, 0x101000, 1, 0xa000);
	c->sys.tcr[2] = TCR_ELX;
	c->sys.ttbr0[2] = 0x30000;
	c->sys.sctlr[2] |= ARM64_SCTLR_M;
	put(m, 0x30000, BLOCK(0x40000000));
	maps(c, 0x1000, 2, 0x40001000);
	maps(c, 0x1000, 1, 0x7000);
	maps(c, 0x1000, 2, 0x40001000);

	/* Reset empties it. */
	map(m, 0x1000, PAGE(0xb000));
	c->cpu.ops->reset(&c->cpu);
	registers(c, 1, false);
	maps(c, 0x1000, 1, 0xb000);
	machine_destroy(m);
}

/*
 * Instructions come straight from the page of the last fetch (fetch() in
 * cpu.c), past the MMU. What changes the translation, the level, the
 * page or the bytes there must still show.
 */
static void test_fetch_page(void)
{
	const uint32_t movz7 = 0xd28000e0u; /* MOVZ X0, #7 */
	const uint32_t movz9 = 0xd2800120u; /* MOVZ X0, #9 */
	const uint32_t movz5 = 0xd28000a0u; /* MOVZ X0, #5 */
	Arm64Cpu *c;
	Machine *m = machine(&c);

	mmu_on(m, c, 1, false);
	/* An instruction that the code stores, then runs. */
	c->x[0] = movz7;
	c->x[1] = CODE + 4;
	step(m, c, STR);
	CHECK_EQ(machine_run(m, 1), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->x[0], 7);
	/* The page mapped elsewhere: after TLBI, the new one runs. */
	map(m, CODE, PAGE(0x90000));
	CHECK_EQ(bus_write(m->bus, 0x90000 + 0xc, 4, movz9), BUS_OK);
	step(m, c, TLBI_VMALLE1); /* at CODE + 8, still the old page */
	CHECK_EQ(c->pc, CODE + 0xc);
	CHECK_EQ(machine_run(m, 1), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->x[0], 9);
	/* The next page, which is elsewhere. */
	map(m, CODE + 0x1000, PAGE(0x95000));
	CHECK_EQ(bus_write(m->bus, 0x95000, 4, movz5), BUS_OK);
	arm64_tlb_flush(c);
	c->pc = CODE + 0xffc;
	CHECK_EQ(bus_write(m->bus, 0x90ffc, 4, NOP), BUS_OK);
	CHECK_EQ(machine_run(m, 2), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->x[0], 5);
	/* The same page at EL0, which may not execute it. */
	map(m, CODE, PAGE(CODE) | UXN);
	arm64_tlb_flush(c);
	c->pc = CODE;
	c->sys.spsr[1] = 0; /* EL0t */
	c->sys.elr[1] = CODE;
	step(m, c, NOP);
	step(m, c, ERET);
	CHECK_EQ(c->el, 0);
	CHECK_EQ(machine_run(m, 1), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->el, 1);
	CHECK_EQ(c->pc, VBAR(1) + LOWER);
	CHECK_EQ(c->sys.esr[1] >> 26, 0x20); /* Instruction Abort, lower EL */
	machine_destroy(m);

	/* Reads: two pages that share an entry, 64 pages apart. */
	m = machine(&c);
	mmu_on(m, c, 1, false);
	map(m, 0x2000, PAGE(0x60000));
	map(m, 0x42000, PAGE(0x61000));
	put(m, 0x60000, 0x1111);
	put(m, 0x61000, 0x2222);
	for (unsigned i = 0; i < 4; i++) { /* A, B, B from its entry, A */
		bool b = i == 1 || i == 2;

		c->x[1] = b ? 0x42000 : 0x2000;
		step(m, c, LDR);
		CHECK_EQ(c->x[0], b ? 0x2222 : 0x1111);
		c->pc = CODE;
	}
	/* An unaligned read into the next page, which is elsewhere. */
	map(m, 0x3000, PAGE(0x70000));
	map(m, 0x4000, PAGE(0x72000));
	put(m, 0x70ff8, 0x1111111111111111u);
	put(m, 0x72000, 0x2222222222222222u);
	c->x[1] = 0x3ff8;
	step(m, c, LDR); /* the page is kept */
	c->pc = CODE;
	c->x[1] = 0x3ffc;
	step(m, c, LDR);
	CHECK_EQ(c->x[0], 0x2222222211111111u);
	machine_destroy(m);
}

/* Faults become Data Aborts and Instruction Aborts, D1.4.3. */
static void test_aborts(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	mmu_on(m, c, 1, false);
	c->x[1] = 0x3000;
	step(m, c, LDR);
	taken(c, 1, SAME, ESR(0x25, 0x07), CODE);
	CHECK_EQ(c->sys.far[1], 0x3000);
	mmu_on(m, c, 1, false);
	c->x[1] = 0x3008;
	step(m, c, STR);
	taken(c, 1, SAME, ESR(0x25, 0x47), CODE);
	CHECK_EQ(c->sys.far[1], 0x3008);

	/* From EL0, and to EL2 with HCR_EL2.TGE. */
	mmu_on(m, c, 0, false);
	c->x[1] = 0x3000;
	step(m, c, LDR);
	taken(c, 1, LOWER, ESR(0x24, 0x07), CODE);
	mmu_on(m, c, 0, false);
	c->sys.hcr_el2 |= ARM64_HCR_TGE; /* stage 1 off: PA = VA */
	c->x[1] = UINT64_C(1) << 40;
	step(m, c, LDR);
	taken(c, 2, LOWER, ESR(0x24, 0x00), CODE);
	CHECK_EQ(c->sys.far[2], UINT64_C(1) << 40);

	/* Instruction fetches: from EL1 at a PXN page, from EL0 at nothing. */
	mmu_on(m, c, 1, false);
	map(m, 0x2000, PAGE(0x2000) | PXN);
	c->pc = 0x2000;
	step(m, c, NOP);
	taken(c, 1, SAME, ESR(0x21, 0x0f), 0x2000);
	CHECK_EQ(c->sys.far[1], 0x2000);
	mmu_on(m, c, 0, false);
	c->pc = 0x3000;
	step(m, c, NOP);
	taken(c, 1, LOWER, ESR(0x20, 0x07), 0x3000);
	CHECK_EQ(c->sys.far[1], 0x3000);

	/* SCTLR_EL1.A checks alignment before translation. */
	mmu_on(m, c, 1, false);
	map(m, DATA, PAGE(DATA));
	put(m, DATA, UINT64_C(0x8877665544332211));
	put(m, DATA + 8, UINT64_C(0xffeeddccbbaa9900));
	c->x[1] = DATA + 1;
	step(m, c, LDR);
	CHECK_EQ(c->x[0], UINT64_C(0x0088776655443322));
	c->pc = CODE;
	c->sys.sctlr[1] |= ARM64_SCTLR_A;
	c->x[1] = 0x3001;
	step(m, c, LDR);
	taken(c, 1, SAME, ESR(0x25, 0x21), CODE);
	CHECK_EQ(c->sys.far[1], 0x3001);
	c->sys.sctlr[1] &= ~(uint64_t)ARM64_SCTLR_A;

	/* Device memory from MAIR_EL1. */
	mmu_on(m, c, 1, false);
	map(m, 0x4000, PAGE(DATA) | ATTR(1));
	c->x[1] = 0x4008;
	step(m, c, LDR);
	CHECK_EQ(c->x[0], UINT64_C(0xffeeddccbbaa9900));
	c->pc = CODE;
	c->x[1] = 0x4004;
	step(m, c, LDR);
	taken(c, 1, SAME, ESR(0x25, 0x21), CODE);
	CHECK_EQ(c->sys.far[1], 0x4004);

	/* Unaligned across two pages, mapped apart. */
	mmu_on(m, c, 1, false);
	map(m, 0x5000, PAGE(DATA + 0x1000));
	map(m, 0x6000, PAGE(DATA));
	put(m, DATA + 0x1ff8, UINT64_C(0x0706050403020100));
	c->x[1] = 0x5ffc;
	step(m, c, LDR);
	CHECK_EQ(c->x[0], UINT64_C(0x4433221107060504));
	c->pc = CODE;
	c->x[0] = UINT64_C(0x0123456789abcdef);
	c->x[1] = 0x5ffd;
	step(m, c, STR);
	CHECK_EQ(get(m, DATA + 0x1ff8, 8), UINT64_C(0xabcdef0403020100));
	CHECK_EQ(get(m, DATA, 8), UINT64_C(0x8877660123456789));
	/* A fault in the second page: FAR holds its start; nothing written. */
	c->pc = CODE;
	c->x[1] = 0x6ffe;
	step(m, c, STR);
	taken(c, 1, SAME, ESR(0x25, 0x47), CODE);
	CHECK_EQ(c->sys.far[1], 0x7000);
	CHECK_EQ(get(m, DATA + 0xff8, 8), 0);
	map(m, 0x7000, PAGE(0x7000) | ATTR(1));
	mmu_on(m, c, 1, false);
	c->x[1] = 0x6ffe;
	step(m, c, LDR);
	taken(c, 1, SAME, ESR(0x25, 0x21), CODE);
	CHECK_EQ(c->sys.far[1], 0x7000);
	machine_destroy(m);
}

/* AArch64_BranchAddr(), J1.4.3.221: branches remove the tag. */
static void test_branch_addr(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	mmu_on(m, c, 1, false);
	c->x[1] = UINT64_C(0x5a00000000080000);
	step(m, c, BR);
	CHECK_EQ(c->pc, UINT64_C(0x5a00000000080000));
	c->sys.tcr[1] |= UINT64_C(1) << 37; /* TBI0 */
	c->pc = CODE;
	step(m, c, BR);
	CHECK_EQ(c->pc, CODE);
	c->x[1] = UINT64_C(0x12ffff8000000000);
	step(m, c, BR);
	CHECK_EQ(c->pc, UINT64_C(0x12ffff8000000000));
	c->sys.tcr[1] |= UINT64_C(1) << 38; /* TBI1 */
	c->pc = CODE;
	step(m, c, BR);
	CHECK_EQ(c->pc, UINT64_C(0xffffff8000000000));

	/* The PC-relative branches too, at the top of the lower range. */
	c->pc = UINT64_C(0x00fffffffffffffc);
	c->next_pc = c->pc + 4;
	CHECK_EQ(arm64_execute(c, 0x14000002), ARM64_EXEC_OK); /* B .+8 */
	CHECK_EQ(c->next_pc, 4);
	c->nzcv = ARM64_NZCV_Z;
	CHECK_EQ(arm64_execute(c, 0x54000040), ARM64_EXEC_OK); /* B.EQ .+8 */
	CHECK_EQ(c->next_pc, 4);
	c->x[2] = 0;
	CHECK_EQ(arm64_execute(c, 0xb4000042), ARM64_EXEC_OK); /* CBZ X2, .+8 */
	CHECK_EQ(c->next_pc, 4);
	CHECK_EQ(arm64_execute(c, 0x36000042), ARM64_EXEC_OK); /* TBZ W2, #0 */
	CHECK_EQ(c->next_pc, 4);
	CHECK_EQ(arm64_execute(c, 0xd63f0020), ARM64_EXEC_OK); /* BLR X1 */
	CHECK_EQ(c->next_pc, UINT64_C(0xffffff8000000000));

	/* Bit 55, not bit 54, picks TBI1 and fills the top byte. */
	c->sys.tcr[1] = TCR_EL1 | UINT64_C(1) << 38;
	c->pc = CODE;
	c->x[1] = UINT64_C(0x0080000000001000);
	step(m, c, BR);
	CHECK_EQ(c->pc, UINT64_C(0xff80000000001000));

	/* EL2 and EL3 clear the top byte. */
	mmu_on(m, c, 2, false);
	c->sys.tcr[2] |= UINT64_C(1) << 20;
	c->x[1] = UINT64_C(0xa080000000080000);
	step(m, c, BR);
	CHECK_EQ(c->pc, UINT64_C(0x0080000000080000));

	/* ERET goes through it at the new level, and so do the vectors. */
	mmu_on(m, c, 2, false);
	c->sys.tcr[1] = TCR_EL1;
	c->sys.spsr[2] = 0x3c5; /* EL1 with SP_EL1 */
	c->sys.elr[2] = UINT64_C(0x7700000000001000);
	step(m, c, ERET);
	CHECK_EQ(c->pc, UINT64_C(0x7700000000001000));
	mmu_on(m, c, 2, false);
	c->sys.tcr[1] |= UINT64_C(1) << 37;
	step(m, c, ERET);
	CHECK_EQ(c->el, 1);
	CHECK_EQ(c->pc, 0x1000);
	mmu_on(m, c, 0, false);
	c->sys.vbar[1] = UINT64_C(0x6600000000001000);
	c->pc = 0x3000;
	step(m, c, NOP);
	CHECK_EQ(c->pc, UINT64_C(0x6600000000001400));
	mmu_on(m, c, 0, false);
	c->sys.tcr[1] |= UINT64_C(1) << 37;
	c->pc = 0x3000;
	step(m, c, NOP);
	CHECK_EQ(c->pc, 0x1400);
	machine_destroy(m);
}

/* AT and PAR_EL1, AArch64_AT() and AArch64_EncodePAR(). */
static void test_at(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	mmu_on(m, c, 1, false);
	map(m, 0x1000, PAGE(0x1000) | SH(3));
	map(m, 0x2000, PAGE(0x2000) | AP_RO | ATTR(3) | SH(3));
	map(m, 0x3000, PAGE(0x3000) | ATTR(2) | SH(3));
	map(m, 0x4000, PAGE(0x4000) | ATTR(4) | SH(1));
	c->x[1] = 0x1234;
	step(m, c, AT_S1E1R);
	CHECK_EQ(c->sys.par_el1, UINT64_C(0xff00000000001b80));
	c->x[1] = 0x2fff;
	step(m, c, AT_S1E1R);
	CHECK_EQ(c->sys.par_el1, UINT64_C(0x0400000000002b00));
	c->x[1] = 0x3000;
	step(m, c, AT_S1E1W);
	CHECK_EQ(c->sys.par_el1, UINT64_C(0x4400000000003b00));
	c->x[1] = 0x4000;
	step(m, c, AT_S1E1R);
	CHECK_EQ(c->sys.par_el1, UINT64_C(0x0400000000004b00));

	/* Faults go to PAR_EL1: F, FST, and RES1 bit 11. */
	c->x[1] = 0x2000;
	step(m, c, AT_S1E1W);
	CHECK_EQ(c->sys.par_el1, 0x81f);
	c->x[1] = 0x1000;
	step(m, c, AT_S1E0R);
	CHECK_EQ(c->sys.par_el1, 0x81f);
	c->x[1] = 0x5000;
	step(m, c, AT_S1E1R);
	CHECK_EQ(c->sys.par_el1, 0x80f);
	map(m, 0x6000, PAGE(0x6000) & ~AF);
	c->x[1] = 0x6000;
	step(m, c, AT_S1E1R);
	CHECK_EQ(c->sys.par_el1, 0x817);
	CHECK_EQ(c->el, 1);
	CHECK_EQ(c->pc, CODE + 0x20);

	/* Secure EL1&0 from EL3: NS from the descriptor. */
	mmu_on(m, c, 1, true);
	map(m, 0x2000, PAGE(0x2000) | NS);
	arm64_tlb_flush(c);
	c->el = 3;
	c->x[1] = 0x1000;
	step(m, c, AT_S1E1R);
	CHECK_EQ(c->sys.par_el1, UINT64_C(0xff00000000001980));
	c->x[1] = 0x2000;
	step(m, c, AT_S12E1R); /* stage 1 only: EL2 is not enabled */
	CHECK_EQ(c->sys.par_el1, UINT64_C(0xff00000000002a00));
	c->x[1] = 0x1234;
	step(m, c, AT_S1E3R); /* stage 1 is off at EL3 */
	CHECK_EQ(c->sys.par_el1, 0x1900);

	/* Which levels may use which AT. */
	c->pc = CODE;
	undefined(m, c, AT_S1E2R); /* Secure EL3 */
	c->sys.scr_el3 |= ARM64_SCR_NS;
	step(m, c, AT_S1E2R); /* stage 1 is off at EL2: Non-secure */
	CHECK_EQ(c->sys.par_el1, 0x1b00);
	c->pc = CODE;
	c->el = 2;
	undefined(m, c, AT_S1E3R);
	c->el = 1;
	undefined(m, c, AT_S1E2R);
	undefined(m, c, AT_S12E1R);
	mmu_on(m, c, 0, false);
	step(m, c, AT_S1E0R);
	taken(c, 1, LOWER, ESR(0, 0), CODE);

	/* Stage 2 is not implemented yet. */
	mmu_on(m, c, 2, false);
	c->sys.hcr_el2 |= ARM64_HCR_VM;
	CHECK_EQ(arm64_test_step(m, c, AT_S1E1R), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "(stage 2 translation, for HCR_EL2.VM)") !=
	      NULL);
	machine_destroy(m);
}

/* DC, IC and TLBI, C5.3 and C5.5. */
static void test_cache_and_tlb(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	uint64_t value;

	CHECK(arm64_sys_table_is_sorted());

	/* DC ZVA zeroes the 64-byte block of DCZID_EL0.BS = 4. */
	mmu_on(m, c, 1, false);
	map(m, DATA, PAGE(DATA));
	map(m, 0x2000, PAGE(0x2000) | AP_RO);
	map(m, 0x4000, PAGE(0x4000) | ATTR(1));
	for (uint64_t a = DATA; a < DATA + 0x100; a += 8)
		put(m, a, UINT64_C(0x1111111111111111));
	c->x[1] = DATA + 0x47;
	step(m, c, DC_ZVA);
	CHECK_EQ(get(m, DATA + 0x38, 8), UINT64_C(0x1111111111111111));
	CHECK_EQ(get(m, DATA + 0x40, 8), 0);
	CHECK_EQ(get(m, DATA + 0x78, 8), 0);
	CHECK_EQ(get(m, DATA + 0x80, 8), UINT64_C(0x1111111111111111));
	c->x[1] = 0x3047;
	step(m, c, DC_ZVA);
	taken(c, 1, SAME, ESR(0x25, 0x47), CODE + 4);
	CHECK_EQ(c->sys.far[1], 0x3047);
	mmu_on(m, c, 1, false);
	c->x[1] = 0x2040;
	step(m, c, DC_ZVA);
	taken(c, 1, SAME, ESR(0x25, 0x4f), CODE);
	mmu_on(m, c, 1, false);
	c->x[1] = 0x4040;
	step(m, c, DC_ZVA);
	taken(c, 1, SAME, ESR(0x25, 0x61), CODE);
	mmu_on(m, c, 3, true);
	c->sys.sctlr[3] &= ~(uint64_t)ARM64_SCTLR_M;
	c->x[1] = DATA;
	step(m, c, DC_ZVA);
	taken(c, 3, SAME, ESR(0x25, 0x61), CODE);

	/* EL0 needs SCTLR_EL1.DZE; HCR_EL2.TDZ traps EL0 and EL1. */
	mmu_on(m, c, 0, false);
	map(m, DATA, PAGE(DATA) | AP_EL0);
	arm64_tlb_flush(c);
	CHECK(arm64_cpu_read_sysreg(c, ARM64_DCZID_EL0, &value));
	CHECK_EQ(value, 0x14);
	step(m, c, DC_ZVA);
	taken(c, 1, LOWER, ESR(0x18, ISS_DC_ZVA), CODE);
	mmu_on(m, c, 0, false);
	c->sys.sctlr[1] |= ARM64_SCTLR_DZE;
	CHECK(arm64_cpu_read_sysreg(c, ARM64_DCZID_EL0, &value));
	CHECK_EQ(value, 0x04);
	c->x[1] = DATA;
	step(m, c, DC_ZVA);
	CHECK_EQ(c->el, 0);
	c->pc = CODE;
	c->sys.hcr_el2 |= ARM64_HCR_TDZ;
	CHECK(arm64_cpu_read_sysreg(c, ARM64_DCZID_EL0, &value));
	CHECK_EQ(value, 0x14);
	step(m, c, DC_ZVA);
	taken(c, 2, LOWER, ESR(0x18, ISS_DC_ZVA), CODE);
	c->el = 1;
	c->pc = CODE;
	CHECK(arm64_cpu_read_sysreg(c, ARM64_DCZID_EL0, &value));
	CHECK_EQ(value, 0x14);
	step(m, c, DC_ZVA);
	taken(c, 2, LOWER, ESR(0x18, ISS_DC_ZVA), CODE);
	CHECK(arm64_cpu_read_sysreg(c, ARM64_DCZID_EL0, &value));
	CHECK_EQ(value, 0x04);

	/* DC and IC by VA translate it: CM and WnR are set in a fault. */
	mmu_on(m, c, 1, false);
	c->x[1] = 0x3000;
	step(m, c, DC_CIVAC);
	taken(c, 1, SAME, ESR(0x25, 0x147), CODE);
	CHECK_EQ(c->sys.far[1], 0x3000);
	mmu_on(m, c, 1, false);
	c->x[1] = 0x2000;
	step(m, c, DC_CVAC);
	step(m, c, DC_IVAC);
	taken(c, 1, SAME, ESR(0x25, 0x14f), CODE + 4);
	mmu_on(m, c, 1, false);
	c->x[1] = 0x3000;
	step(m, c, IC_IVAU);
	taken(c, 1, SAME, ESR(0x25, 0x147), CODE);

	/* EL0 needs SCTLR_EL1.UCI, and read permission for DC. */
	mmu_on(m, c, 0, false);
	map(m, 0x1000, PAGE(0x1000));
	c->x[1] = 0x1000;
	step(m, c, DC_CVAC);
	taken(c, 1, LOWER, ESR(0x18, ISS_DC_CVAC), CODE);
	mmu_on(m, c, 0, false);
	c->sys.sctlr[1] |= ARM64_SCTLR_UCI;
	step(m, c, IC_IVAU);
	CHECK_EQ(c->el, 0);
	step(m, c, DC_CVAC);
	taken(c, 1, LOWER, ESR(0x24, 0x14f), CODE + 4);
	mmu_on(m, c, 0, false);
	c->sys.sctlr[1] |= ARM64_SCTLR_UCI;
	c->sys.hcr_el2 |= ARM64_HCR_TPU;
	step(m, c, IC_IVAU);
	taken(c, 2, LOWER, ESR(0x18, ISS_IC_IVAU), CODE);

	/* The other traps of HCR_EL2, from EL1. */
	static const struct {
		uint32_t hcr;
		uint32_t insn;
	} traps[] = {
		{ARM64_HCR_TPC, DC_CVAC},  {ARM64_HCR_TPC, DC_IVAC},
		{ARM64_HCR_TPC, DC_CIVAC}, {ARM64_HCR_TPU, IC_IALLU},
		{ARM64_HCR_TSW, DC_ISW},   {ARM64_HCR_TTLB, TLBI_VMALLE1},
	};
	for (unsigned i = 0; i < ARRAY_SIZE(traps); i++) {
		mmu_on(m, c, 1, false);
		c->sys.hcr_el2 |= traps[i].hcr;
		step(m, c, traps[i].insn);
		CHECK_EQ(c->el, 2);
		CHECK_EQ(c->sys.esr[2] >> 26, 0x18);
		mmu_on(m, c, 1, true); /* Secure: EL2 is not enabled */
		c->sys.hcr_el2 |= traps[i].hcr;
		c->x[1] = 0x80000;
		step(m, c, traps[i].insn);
		CHECK_EQ(c->el, 1);
		CHECK_EQ(c->pc, CODE + 4);
	}

	/* Which levels may use which instruction. */
	mmu_on(m, c, 0, false);
	c->sys.sctlr[1] |= ARM64_SCTLR_UCI;
	static const uint32_t not_el0[] = {
		DC_ISW,       DC_IVAC,  IC_IALLU,
		TLBI_VMALLE1, AT_S1E1R, 0xd528751f, /* SYSL X31, #0, C7, C5, #0 */
	};
	for (unsigned i = 0; i < ARRAY_SIZE(not_el0); i++) {
		c->el = 0;
		c->pc = CODE;
		step(m, c, not_el0[i]);
		taken(c, 1, LOWER, ESR(0, 0), CODE);
	}
	mmu_on(m, c, 1, false);
	undefined(m, c, TLBI_ALLE2);
	undefined(m, c, TLBI_ALLE1);
	undefined(m, c, SYS(0, 7, 0, 0)); /* no such instruction */
	undefined(m, c, SYS(0, 8, 7, 4));
	mmu_on(m, c, 2, false);
	undefined(m, c, TLBI_ALLE3);
	step(m, c, TLBI_ALLE2);
	step(m, c, TLBI_ALLE1);
	mmu_on(m, c, 3, true);
	undefined(m, c, TLBI_ALLE2); /* Secure EL3: no EL2 regime */
	step(m, c, TLBI_ALLE1);
	step(m, c, SYS(4, 8, 4, 1)); /* TLBI IPAS2E1 */
	step(m, c, TLBI_ALLE3);
	c->sys.scr_el3 |= ARM64_SCR_NS;
	step(m, c, TLBI_ALLE2);
	CHECK_EQ(c->el, 3);
	CHECK_EQ(c->pc, CODE + 16);
	machine_destroy(m);
}

/*
 * Every System instruction of the model, with its encoding from the GNU
 * assembler of binutils, not from the tables of sysinsn.c: from which
 * level it runs, the bit of SCTLR_EL1 that EL0 needs, the HCR_EL2 bit
 * that traps it, and whether it is UNDEFINED at Secure EL3 (el2).
 */
static void test_encodings(void)
{
	static const struct {
		uint32_t insn;
		unsigned min_el;
		uint32_t el0, hcr;
		bool el2;
	} insns[] = {
		{0xd508711f, 1, 0, ARM64_HCR_TPU, false},  /* IC IALLUIS */
		{0xd508751f, 1, 0, ARM64_HCR_TPU, false},  /* IC IALLU */
		{0xd5087621, 1, 0, ARM64_HCR_TPC, false},  /* DC IVAC, X1 */
		{0xd5087641, 1, 0, ARM64_HCR_TSW, false},  /* DC ISW, X1 */
		{0xd5087801, 1, 0, 0, false},              /* AT S1E1R, X1 */
		{0xd5087821, 1, 0, 0, false},              /* AT S1E1W, X1 */
		{0xd5087841, 1, 0, 0, false},              /* AT S1E0R, X1 */
		{0xd5087861, 1, 0, 0, false},              /* AT S1E0W, X1 */
		{0xd5087a41, 1, 0, ARM64_HCR_TSW, false},  /* DC CSW, X1 */
		{0xd5087e41, 1, 0, ARM64_HCR_TSW, false},  /* DC CISW, X1 */
		{0xd508831f, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VMALLE1IS */
		{0xd5088321, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VAE1IS, X1 */
		{0xd5088341, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI ASIDE1IS, X1 */
		{0xd5088361, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VAAE1IS, X1 */
		{0xd50883a1, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VALE1IS, X1 */
		{0xd50883e1, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VAALE1IS, X1 */
		{0xd508871f, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VMALLE1 */
		{0xd5088721, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VAE1, X1 */
		{0xd5088741, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI ASIDE1, X1 */
		{0xd5088761, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VAAE1, X1 */
		{0xd50887a1, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VALE1, X1 */
		{0xd50887e1, 1, 0, ARM64_HCR_TTLB, false}, /* TLBI VAALE1, X1 */
		{0xd50b7421, 0, ARM64_SCTLR_DZE, ARM64_HCR_TDZ, false}, /* DC ZVA */
		{0xd50b7521, 0, ARM64_SCTLR_UCI, ARM64_HCR_TPU, false}, /* IC IVAU */
		{0xd50b7a21, 0, ARM64_SCTLR_UCI, ARM64_HCR_TPC, false}, /* DC CVAC */
		{0xd50b7b21, 0, ARM64_SCTLR_UCI, ARM64_HCR_TPU, false}, /* DC CVAU */
		{0xd50b7e21, 0, ARM64_SCTLR_UCI, ARM64_HCR_TPC, false}, /* DC CIVAC */
		{0xd50c7801, 2, 0, 0, true},  /* AT S1E2R, X1 */
		{0xd50c7821, 2, 0, 0, true},  /* AT S1E2W, X1 */
		{0xd50c7881, 2, 0, 0, false}, /* AT S12E1R, X1 */
		{0xd50c78a1, 2, 0, 0, false}, /* AT S12E1W, X1 */
		{0xd50c78c1, 2, 0, 0, false}, /* AT S12E0R, X1 */
		{0xd50c78e1, 2, 0, 0, false}, /* AT S12E0W, X1 */
		{0xd50c8021, 2, 0, 0, false}, /* TLBI IPAS2E1IS, X1 */
		{0xd50c80a1, 2, 0, 0, false}, /* TLBI IPAS2LE1IS, X1 */
		{0xd50c831f, 2, 0, 0, true},  /* TLBI ALLE2IS */
		{0xd50c8321, 2, 0, 0, true},  /* TLBI VAE2IS, X1 */
		{0xd50c839f, 2, 0, 0, false}, /* TLBI ALLE1IS */
		{0xd50c83a1, 2, 0, 0, true},  /* TLBI VALE2IS, X1 */
		{0xd50c83df, 2, 0, 0, false}, /* TLBI VMALLS12E1IS */
		{0xd50c8421, 2, 0, 0, false}, /* TLBI IPAS2E1, X1 */
		{0xd50c84a1, 2, 0, 0, false}, /* TLBI IPAS2LE1, X1 */
		{0xd50c871f, 2, 0, 0, true},  /* TLBI ALLE2 */
		{0xd50c8721, 2, 0, 0, true},  /* TLBI VAE2, X1 */
		{0xd50c879f, 2, 0, 0, false}, /* TLBI ALLE1 */
		{0xd50c87a1, 2, 0, 0, true},  /* TLBI VALE2, X1 */
		{0xd50c87df, 2, 0, 0, false}, /* TLBI VMALLS12E1 */
		{0xd50e7801, 3, 0, 0, false}, /* AT S1E3R, X1 */
		{0xd50e7821, 3, 0, 0, false}, /* AT S1E3W, X1 */
		{0xd50e831f, 3, 0, 0, false}, /* TLBI ALLE3IS */
		{0xd50e8321, 3, 0, 0, false}, /* TLBI VAE3IS, X1 */
		{0xd50e83a1, 3, 0, 0, false}, /* TLBI VALE3IS, X1 */
		{0xd50e871f, 3, 0, 0, false}, /* TLBI ALLE3 */
		{0xd50e8721, 3, 0, 0, false}, /* TLBI VAE3, X1 */
		{0xd50e87a1, 3, 0, 0, false}, /* TLBI VALE3, X1 */
	};
	Arm64Cpu *c;
	Machine *m = machine(&c);

	for (unsigned i = 0; i < ARRAY_SIZE(insns); i++) {
		unsigned min = insns[i].min_el;

		/* It runs from its lowest level, with what EL0 needs. */
		mmu_on(m, c, min, false);
		map(m, DATA, PAGE(DATA) | AP_EL0);
		c->sys.sctlr[1] |= insns[i].el0;
		c->x[1] = DATA;
		step(m, c, insns[i].insn);
		if (c->el != min || c->pc != CODE + 4) {
			fprintf(stderr, "0x%08" PRIx32 " does not run at EL%u\n",
			        insns[i].insn, min);
			test_failures++;
		}
		/* It is UNDEFINED below. */
		if (min > 0) {
			mmu_on(m, c, min - 1, false);
			step(m, c, insns[i].insn);
			taken(c, min > 1 ? min - 1 : 1, min > 1 ? SAME : LOWER, ESR(0, 0),
			      CODE);
		}
		/* EL0 traps without its SCTLR_EL1 bit. */
		if (min == 0) {
			mmu_on(m, c, 0, false);
			c->sys.sctlr[1] &= ~(uint64_t)insns[i].el0;
			step(m, c, insns[i].insn);
			CHECK_EQ(c->el, 1);
			CHECK_EQ(c->sys.esr[1] >> 26, 0x18);
		}
		/* HCR_EL2 traps it from EL1. */
		if (min <= 1) {
			mmu_on(m, c, 1, false);
			map(m, DATA, PAGE(DATA) | AP_EL0);
			c->x[1] = DATA;
			c->sys.hcr_el2 |= insns[i].hcr;
			step(m, c, insns[i].insn);
			CHECK_EQ(c->el, insns[i].hcr != 0 ? 2 : 1);
			/* But not from EL2. */
			mmu_on(m, c, 2, false);
			map(m, DATA, PAGE(DATA));
			c->sys.hcr_el2 |= insns[i].hcr;
			step(m, c, insns[i].insn);
			CHECK_EQ(c->pc, CODE + 4);
		}
		/* Secure EL3 has no EL2 translation regime. */
		mmu_on(m, c, 3, true);
		map(m, DATA, PAGE(DATA));
		step(m, c, insns[i].insn);
		CHECK_EQ(c->pc, insns[i].el2 ? VBAR(3) + SAME : CODE + 4);
	}

	/* SYSL, here with the fields of IC IALLU, is UNDEFINED. */
	mmu_on(m, c, 1, false);
	undefined(m, c, 0xd528751f);

	/* Rt has five bits: AT S1E3R, X17. */
	mmu_on(m, c, 3, true);
	c->x[1] = 0x1000;
	c->x[17] = CODE;
	step(m, c, (AT_S1E3R & ~31u) | 17);
	CHECK_EQ(c->sys.par_el1 & ~UINT64_C(0xff00000000000fff), CODE);
	machine_destroy(m);
}

/*
 * A start level with fewer index bits than the next levels, and the top
 * bits of an EL2 address, with and without TBI.
 */
static void test_walk_edges(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* T0SZ 32: level 1 uses bits 31:30 only, level 2 bits 29:21. */
	mmu_on(m, c, 3, true);
	c->sys.tcr[3] = 32;
	c->sys.ttbr0[3] = 0x20000;
	put(m, 0x20000, TABLE(0x21000));
	put(m, 0x21000 + 5 * 8, TABLE(0x22000));
	put(m, 0x22000 + 3 * 8, PAGE(0x5000));
	maps(c, 0xa03123, 3, 0x5123);

	mmu_on(m, c, 2, false);
	faults(c, UINT64_C(0x8000000000080000), 2, ARM64_FAULT_TRANSLATION, 0);
	c->sys.tcr[2] |= UINT64_C(1) << 20; /* TBI */
	maps(c, UINT64_C(0x8000000000080000), 2, CODE);
	faults(c, UINT64_C(0x0080000000080000), 2, ARM64_FAULT_TRANSLATION, 0);
	machine_destroy(m);
}

/*
 * The fault status codes, EncodeLDFSC(), for each kind of fault and
 * level, through PAR_EL1.FST. Bit 47 is above the 40 bits of the A53.
 */
static void test_fault_status(void)
{
	const uint64_t big = UINT64_C(1) << 47, gb = UINT64_C(1) << 30,
				   mb2 = UINT64_C(1) << 21;
	const struct {
		uint64_t entry, desc, va;
		bool write;
		uint64_t fst;
	} faults[] = {
		{L1 + 8, 0, gb, false, 0x05}, /* Translation */
		{L2 + 8, 0, mb2, false, 0x06},
		{L3 + 8, 0, 0x1000, false, 0x07},
		{L1 + 8, TABLE(big), gb, false, 0x01}, /* Address size */
		{L2 + 8, BLOCK(big), mb2, false, 0x02},
		{L3 + 8, PAGE(big), 0x1000, false, 0x03},
		{L1 + 8, gb | 1, gb, false, 0x09}, /* Access flag */
		{L2 + 8, mb2 | 1, mb2, false, 0x0a},
		{L3 + 8, 0x1000 | 3, 0x1000, false, 0x0b},
		{L1 + 8, BLOCK(gb) | AP_RO, gb, true, 0x0d}, /* Permission */
		{L2 + 8, BLOCK(mb2) | AP_RO, mb2, true, 0x0e},
		{L3 + 8, PAGE(0x1000) | AP_RO, 0x1000, true, 0x0f},
	};
	Arm64Cpu *c;
	Machine *m = machine(&c);

	mmu_on(m, c, 3, true);
	for (unsigned i = 0; i < ARRAY_SIZE(faults); i++) {
		put(m, faults[i].entry, faults[i].desc);
		arm64_tlb_flush(c);
		CHECK_EQ(arm64_at(c, faults[i].va, 3, faults[i].write), ARM64_EXEC_OK);
		CHECK_EQ(c->sys.par_el1, 0x801 | faults[i].fst << 1);
		put(m, faults[i].entry, 0);
	}
	/* Level 0: an address out of range, then TTBR0_EL3. */
	CHECK_EQ(arm64_at(c, gb << 9, 3, false), ARM64_EXEC_OK);
	CHECK_EQ(c->sys.par_el1, 0x801 | 0x04 << 1);
	c->sys.ttbr0[3] |= big;
	arm64_tlb_flush(c);
	CHECK_EQ(arm64_at(c, 0x1000, 3, false), ARM64_EXEC_OK);
	CHECK_EQ(c->sys.par_el1, 0x801);
	machine_destroy(m);
}

/* TCR_ELx.{I}PS in each regime, on a core with 48-bit addresses. */
static void test_output_size(void)
{
	static const Arm64RegValue values[] = {
		{ARM64_ID_AA64MMFR0_EL1, 5}, /* PARange: 48 bits */
	};
	static const Arm64CpuModel model = {
		.name = "test",
		.values = values,
		.num_values = 1,
	};
	static const unsigned bits[8] = {32, 36, 40, 42, 44, 48, 48, 48};
	Arm64Cpu *c;
	Machine *m = machine_with(&model, &c);

	for (unsigned el = 1; el <= 3; el++) {
		unsigned shift = el == 1 ? 32 : 16;

		for (unsigned ps = 0; ps < 8; ps++) {
			uint64_t top = UINT64_C(1) << bits[ps];

			mmu_on(m, c, el, false);
			c->sys.tcr[el] &= ~(UINT64_C(7) << shift);
			c->sys.tcr[el] |= (uint64_t)ps << shift;
			map(m, 0x1000, PAGE(top >> 1));
			maps(c, 0x1000, el, top >> 1);
			if (bits[ps] < 48) {
				map(m, 0x1000, PAGE(top));
				arm64_tlb_flush(c);
				faults(c, 0x1000, el, ARM64_FAULT_ADDRESS_SIZE, 3);
			}
		}
	}
	machine_destroy(m);
}

/* What ID_AA64MMFR0_EL1 tells the MMU: PARange and the granules. */
static void test_features(void)
{
	static const unsigned pa_bits[9] = {32, 36, 40, 42, 44, 48, 52, 48, 48};

	for (unsigned i = 0; i < 10; i++) {
		/* 0 to 8: PARange, and every granule but 16KB. */
		const Arm64RegValue values[] = {
			{ARM64_ID_AA64MMFR0_EL1, i < 9 ? i : UINT64_C(0x0f0f0005)},
		};
		const Arm64CpuModel model = {
			.name = "test",
			.values = values,
			.num_values = 1,
		};
		Arm64Cpu *c;
		Machine *m = machine_with(&model, &c);

		CHECK_EQ(c->pa_bits, i < 9 ? pa_bits[i] : 48);
		CHECK(c->granule_4k && !c->granule_16k);
		CHECK_EQ(c->granule_64k, i < 9);
		machine_destroy(m);
	}
}

/*
 * The registers of the MMU: UNDEFINED below their Exception level; MSR
 * sets the field that the MMU reads and empties the TLB. So does reset.
 */
static void test_registers(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	const struct {
		uint16_t encoding;
		unsigned el;
		uint64_t *field;
		uint64_t value;
	} regs[] = {
		{ARM64_SCTLR_EL1, 1, &c->sys.sctlr[1], c->sys.sctlr[1] | ARM64_SCTLR_A},
		{ARM64_TTBR0_EL1, 1, &c->sys.ttbr0[1], 0x1000},
		{ARM64_TTBR1_EL1, 1, &c->sys.ttbr1_el1, 0x2000},
		/* Bit 25 is not SCTLR_ELx.EE here. */
		{ARM64_TCR_EL1, 1, &c->sys.tcr[1], TCR_EL1 | 1u << 25},
		{ARM64_MAIR_EL1, 1, &c->sys.mair[1], MAIR},
		{ARM64_SCTLR_EL2, 2, &c->sys.sctlr[2], c->sys.sctlr[2] | ARM64_SCTLR_A},
		{ARM64_HCR_EL2, 2, &c->sys.hcr_el2, ARM64_HCR_RW | ARM64_HCR_TSW},
		{ARM64_TTBR0_EL2, 2, &c->sys.ttbr0[2], 0x3000},
		{ARM64_TCR_EL2, 2, &c->sys.tcr[2], TCR_ELX},
		{ARM64_MAIR_EL2, 2, &c->sys.mair[2], MAIR},
		{ARM64_SCTLR_EL3, 3, &c->sys.sctlr[3], c->sys.sctlr[3] | ARM64_SCTLR_A},
		{ARM64_SCR_EL3, 3, &c->sys.scr_el3,
	     ARM64_SCR_RW | ARM64_SCR_NS | ARM64_SCR_SIF},
		{ARM64_TTBR0_EL3, 3, &c->sys.ttbr0[3], 0x4000},
		{ARM64_TCR_EL3, 3, &c->sys.tcr[3], TCR_ELX},
		{ARM64_MAIR_EL3, 3, &c->sys.mair[3], MAIR},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
		unsigned el = regs[i].el;

		/* Stage 1 off, so that changing a register changes no fetch. */
		registers(c, el - 1, false);
		c->sys.sctlr[el > 2 ? el - 1 : 1] &= ~ARM64_SCTLR_M;
		c->x[1] = regs[i].value;
		c->sys.esr[el > 1 ? el - 1 : 1] = 0;
		step(m, c, msr(regs[i].encoding, 1));
		taken(c, el > 1 ? el - 1 : 1, el > 1 ? SAME : LOWER, ESR(0, 0), CODE);

		registers(c, el, false);
		c->sys.sctlr[el > 1 ? el : 1] &= ~ARM64_SCTLR_M;
		*regs[i].field = regs[i].value ^ 2;
		c->tlb[7].tag = 1;
		step(m, c, msr(regs[i].encoding, 1));
		if (c->pc != CODE + 4 || *regs[i].field != regs[i].value ||
		    c->tlb[7].tag != 0) {
			fprintf(stderr, "MSR to register %u fails at EL%u\n", i, el);
			test_failures++;
		}
	}

	/* Reset. */
	mmu_on(m, c, 3, true);
	map(m, 0x1000, PAGE(0x2000));
	maps(c, 0x1000, 3, 0x2000);
	map(m, 0x1000, PAGE(0x3000));
	c->cpu.ops->reset(&c->cpu);
	registers(c, 3, true);
	maps(c, 0x1000, 3, 0x3000);
	machine_destroy(m);
}

/*
 * Device memory: instructions run from it unless it is execute-never,
 * and DC by address works there, unlike unaligned accesses. Then
 * SCTLR_EL2.A at EL2, and WnR in the syndrome of an Alignment fault.
 */
static void test_device_and_alignment(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->x[1] = DATA + 1; /* stage 1 off: Device memory */
	step(m, c, DC_CIVAC);
	CHECK_EQ(c->pc, CODE + 4);
	mmu_on(m, c, 3, true);
	map(m, CODE, PAGE(CODE) | ATTR(1));
	step(m, c, NOP);
	CHECK_EQ(c->pc, CODE + 4);

	mmu_on(m, c, 2, false);
	map(m, DATA, PAGE(DATA));
	c->sys.sctlr[2] |= ARM64_SCTLR_A;
	step(m, c, STR);
	taken(c, 2, SAME, ESR(0x25, 0x61), CODE);
	mmu_on(m, c, 2, false);
	step(m, c, LDR);
	taken(c, 2, SAME, ESR(0x25, 0x21), CODE);
	machine_destroy(m);
}

/* What stops the machine, and how it says why. */
static void test_stops(void)
{
	static const Arm64RegValue values[] = {
		{ARM64_ID_AA64MMFR0_EL1, UINT64_C(0xf0100007)},
		{ARM64_DCZID_EL0, 5}, /* 128-byte blocks */
	};
	static const Arm64CpuModel model = {
		.name = "test",
		.values = values,
		.num_values = 2,
	};
	const uint32_t clrex = 0xd503305f;
	char expected[128];
	Arm64Cpu *c;
	Machine *m = machine(&c);

	CHECK_EQ(c->pa_bits, 40);
	CHECK(c->granule_4k && c->granule_64k && !c->granule_16k);

	mmu_on(m, c, 3, true);
	c->sys.ttbr0[3] = 0x200000;
	arm64_tlb_flush(c);
	CHECK_EQ(arm64_test_step(m, c, NOP), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message,
	             "translation table walk: 8-byte read from "
	             "0x0000000000200000 at 0x0000000000080000 failed: no region "
	             "at this address") != NULL);
	machine_destroy(m);

	m = machine(&c);
	mmu_on(m, c, 1, false);
	map(m, CODE, PAGE(0x200000));
	CHECK_EQ(arm64_test_step(m, c, NOP), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message,
	             "instruction fetch from 0x0000000000080000 (physical "
	             "0x0000000000200000) failed") != NULL);
	machine_destroy(m);

	m = machine(&c);
	mmu_on(m, c, 1, false);
	map(m, 0x1000, PAGE(0x300000));
	c->x[1] = 0x1008;
	CHECK_EQ(arm64_test_step(m, c, LDR), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message,
	             "8-byte read from 0x0000000000300008 (virtual "
	             "0x0000000000001008) at 0x0000000000080000 failed") != NULL);
	c->x[1] = 0x1ffe;
	map(m, 0x2000, PAGE(0x2000));
	CHECK_EQ(arm64_test_step(m, c, LDR), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message,
	             "1-byte read from 0x0000000000300ffe (virtual "
	             "0x0000000000001ffe)") != NULL);
	c->x[1] = 0x1047;
	CHECK_EQ(arm64_test_step(m, c, DC_ZVA), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "8-byte write to 0x0000000000300040 (virtual "
	                              "0x0000000000001040)") != NULL);
	/* DC ZVA that fails within its block. */
	CHECK_EQ(bus_map_ram(m->bus, "small", 0x400000, 0x20), BUS_OK);
	map(m, 0x3000, PAGE(0x400000));
	c->x[1] = 0x3008;
	CHECK_EQ(arm64_test_step(m, c, DC_ZVA), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "8-byte write to 0x0000000000400020 (virtual "
	                              "0x0000000000003020)") != NULL);
	/* Walks that fail, for data, for the second page, for DC and DC ZVA. */
	put(m, L1 + 8, TABLE(0x200000));
	put(m, L2 + 511 * 8, BLOCK(0));
	static const struct {
		uint32_t insn;
		uint64_t x1;
	} walks[] = {
		{LDR, 0x40000000},
		{LDR, 0x3ffffffc},
		{DC_CIVAC, 0x40000000},
		{DC_ZVA, 0x40000000},
	};
	for (unsigned i = 0; i < ARRAY_SIZE(walks); i++) {
		c->x[1] = walks[i].x1;
		CHECK_EQ(arm64_test_step(m, c, walks[i].insn), MACHINE_STOP_ERROR);
		CHECK(strstr(m->stop_message, "translation table walk: 8-byte read "
		                              "from 0x0000000000200000") != NULL);
	}
	/* An unaligned access that fails in its second page. */
	map(m, 0x1000, PAGE(0x1000));
	map(m, 0x2000, PAGE(0x300000));
	arm64_tlb_flush(c);
	c->x[1] = 0x1ffe;
	CHECK_EQ(arm64_test_step(m, c, LDR), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message,
	             "1-byte read from 0x0000000000300000 (virtual "
	             "0x0000000000002000)") != NULL);
	c->sys.hcr_el2 |= ARM64_HCR_DC;
	arm64_tlb_flush(c); /* as an MSR of HCR_EL2 does */
	CHECK_EQ(arm64_test_step(m, c, NOP), MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message,
	             "instruction fetch from 0x0000000000080000 is not "
	             "implemented yet (stage 2 translation, for HCR_EL2.DC)") !=
	      NULL);
	/* The next stop does not show that detail. */
	c->sys.hcr_el2 &= ~ARM64_HCR_DC;
	CHECK_EQ(arm64_test_step(m, c, clrex), MACHINE_STOP_ERROR);
	snprintf(expected, sizeof(expected), "is not implemented yet (%s)",
	         arm64_group_name(arm64_group(clrex)));
	CHECK(strstr(m->stop_message, expected) != NULL);
	machine_destroy(m);

	/* A core with the 16KB granule and 48-bit addresses, without 4KB. */
	m = machine_with(&model, &c);
	CHECK_EQ(c->pa_bits, 48);
	CHECK(!c->granule_4k && c->granule_64k && c->granule_16k);
	c->sys.tcr[3] = 25 | 2 << 14;
	c->sys.sctlr[3] |= ARM64_SCTLR_M;
	unimplemented(c, 0, 3, "the 16KB translation granule");
	c->sys.tcr[3] = 25;
	unimplemented(c, 0, 3,
	              "TCR_EL3.TG0 = 0, a granule that the core does not have");
	c->sys.tcr[3] = 25 | 3 << 14;
	unimplemented(c, 0, 3,
	              "TCR_EL3.TG0 = 3, a granule that the core does not have");
	/* In EL1&0, bit 55 picks TCR_EL1.TG1, here with TG0 64KB. */
	c->sys.scr_el3 = ARM64_SCR_RW | ARM64_SCR_NS;
	c->sys.sctlr[1] |= ARM64_SCTLR_M;
	c->sys.tcr[1] = 25 | 1 << 14 | 25 << 16 | UINT64_C(1) << 30;
	unimplemented(c, UINT64_C(0xff80000000000000), 1,
	              "the 16KB translation granule");
	c->sys.tcr[1] &= ~(UINT64_C(3) << 30);
	unimplemented(c, UINT64_C(0xff80000000000000), 1,
	              "TCR_EL1.TG1 = 0, a granule that the core does not have");

	/* DCZID_EL0.BS = 5: DC ZVA zeroes 128 bytes. One 64KB level. */
	c->sys.tcr[3] = 25 | 1 << 14;
	c->sys.ttbr0[3] = L1;
	c->sys.mair[3] = MAIR;
	put(m, L1, BLOCK(0));
	for (unsigned i = 0; i < 0x200; i += 8)
		put(m, DATA + i, UINT64_MAX);
	arm64_tlb_flush(c);
	step(m, c, 0xd53b00e0); /* MRS X0, DCZID_EL0 */
	CHECK_EQ(c->x[0], 5);
	c->x[1] = DATA + 0x88;
	step(m, c, DC_ZVA);
	CHECK_EQ(get(m, DATA + 0x78, 8), UINT64_MAX);
	CHECK_EQ(get(m, DATA + 0x80, 8), 0);
	CHECK_EQ(get(m, DATA + 0xf8, 8), 0);
	CHECK_EQ(get(m, DATA + 0x100, 8), UINT64_MAX);
	machine_destroy(m);
}

int main(void)
{
	RUN_TEST(test_disabled);
	RUN_TEST(test_walk_4k);
	RUN_TEST(test_walk_64k);
	RUN_TEST(test_el1_ranges);
	RUN_TEST(test_permissions);
	RUN_TEST(test_tlb);
	RUN_TEST(test_fetch_page);
	RUN_TEST(test_aborts);
	RUN_TEST(test_branch_addr);
	RUN_TEST(test_at);
	RUN_TEST(test_cache_and_tlb);
	RUN_TEST(test_encodings);
	RUN_TEST(test_walk_edges);
	RUN_TEST(test_fault_status);
	RUN_TEST(test_output_size);
	RUN_TEST(test_features);
	RUN_TEST(test_registers);
	RUN_TEST(test_device_and_alignment);
	RUN_TEST(test_stops);
	return test_summary();
}
