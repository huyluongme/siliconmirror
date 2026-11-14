/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for src/cpu/arm64/exclusive.c and the exclusive monitors of
 * cpu.c: the Load-Exclusive and Store-Exclusive instructions, LDAR and
 * STLR, CLREX, the monitors of two PEs, and WFE woken by a write; and
 * for LDTR and STTR, in load_store.c.
 */

#include <stdint.h>
#include <string.h>

#include "core/machine.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "test.h"
#include "util/macros.h"

/* Translation tables for 0 to 2MB: L1[0] -> L2, L2[0] -> L3 (4KB pages). */
#define L1 0x10000
#define L2 0x11000
#define L3 0x12000

#define WB 0x40000       /* Normal Write-Back, Inner Shareable */
#define NC 0x80000       /* Normal Non-cacheable */
#define DEV 0x81000      /* Device-nGnRnE */
#define EL1_ONLY 0x90000 /* EL0 has no access */
#define EL0_RW 0x91000
#define ALL_RO 0x92000 /* read-only for EL1 and EL0 */
#define HOLE 0x93000   /* Write-Back, to 0x300000, where no RAM is */

#define AF (UINT64_C(1) << 10)
#define PAGE(pa) ((uint64_t)(pa) | 3 | AF)
#define TABLE(pa) ((uint64_t)(pa) | 3)
#define ATTR(n) ((uint64_t)(n) << 2)
#define AP_EL0 (UINT64_C(1) << 6)
#define AP_RO (UINT64_C(1) << 7)
#define SH_INNER (UINT64_C(3) << 8)

/* MAIR: 0 Normal Write-Back, 1 Device-nGnRnE, 2 Normal Non-cacheable. */
#define MAIR UINT64_C(0x4400ff)
#define TCR_EL1 UINT64_C(0x280190019)
#define TCR_ELX UINT64_C(0x20019)

#define EC_DATA_ABORT_SAME 0x25
#define DFSC_ALIGNMENT 0x21

/* An instruction of the Load/store exclusive class. */
static uint32_t ex(unsigned size, unsigned o2, unsigned l, unsigned o1,
                   unsigned rs, unsigned o0, unsigned rt2, unsigned rn,
                   unsigned rt)
{
	return (uint32_t)size << 30 | 0x08u << 24 | o2 << 23 | l << 22 | o1 << 21 |
	       rs << 16 | o0 << 15 | rt2 << 10 | rn << 5 | rt;
}

/* LDXR, STXR, LDXP, STXP, LDAR and STLR, size 0 to 3. */
static uint32_t ldxr(unsigned size, unsigned rt, unsigned rn)
{
	return ex(size, 0, 1, 0, 31, 0, 31, rn, rt);
}

static uint32_t stxr(unsigned size, unsigned rs, unsigned rt, unsigned rn)
{
	return ex(size, 0, 0, 0, rs, 0, 31, rn, rt);
}

static uint32_t ldxp(unsigned size, unsigned rt, unsigned rt2, unsigned rn)
{
	return ex(size, 0, 1, 1, 31, 0, rt2, rn, rt);
}

static uint32_t stxp(unsigned size, unsigned rs, unsigned rt, unsigned rt2,
                     unsigned rn)
{
	return ex(size, 0, 0, 1, rs, 0, rt2, rn, rt);
}

static uint32_t ldar(unsigned size, unsigned rt, unsigned rn)
{
	return ex(size, 1, 1, 0, 31, 1, 31, rn, rt);
}

static uint32_t stlr(unsigned size, unsigned rt, unsigned rn)
{
	return ex(size, 1, 0, 0, 31, 1, 31, rn, rt);
}

#define CLREX 0xd503305fu
#define WFE 0xd503205fu
#define ERET 0xd69f03e0u
#define STR_X0_X1 0xf9000020u /* STR X0, [X1] */
#define LDR_X0_X1 0xf9400020u /* LDR X0, [X1] */
#define LDTR_X0_X1 0xf8400820u
#define STTR_X0_X1 0xf8000820u
#define LDTRSB_X0_X1 0x38800820u
#define DC_ZVA_X1 0xd50b7421u
#define STRH_W0_X1 0x79000020u
#define STRB_W0_X1 0x39000020u
#define LDUR_X0_X1 0xf8400020u
#define LDR_POST 0xf8408420u /* LDR X0, [X1], #8 */
#define LDR_PRE 0xf8408c20u  /* LDR X0, [X1, #8]! */
#define LDR_REG 0xf8626820u  /* LDR X0, [X1, X2] */

static void put(Machine *m, uint64_t pa, uint64_t value)
{
	CHECK_EQ(bus_write(m->bus, pa, 8, value), BUS_OK);
}

static uint64_t get(Machine *m, uint64_t pa)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(m->bus, pa, 8, &value), BUS_OK);
	return value;
}

static Arm64Cpu *add_a53(Machine *m, uint64_t mpidr)
{
	Arm64Config config = {
		.reset_pc = 0,
		.highest_el = 3,
		.model = &arm64_cortex_a53_bcm2837,
		.mpidr = mpidr,
	};
	Cpu *base = arm64_cpu_create(&config);

	CHECK(base != NULL && machine_add_cpu(m, base));
	return arm64_cpu_of(base);
}

/*
 * Stage 1 translation on for the regime of el, data cache on, through
 * tables that map the 1MB of RAM at 0 with the pages above.
 */
static void mmu_on(Machine *m, Arm64Cpu *c, unsigned el)
{
	unsigned r = el > 1 ? el : 1;

	put(m, L1, TABLE(L2));
	put(m, L2, TABLE(L3));
	for (uint64_t pa = 0; pa < 0x100000; pa += 0x1000) {
		uint64_t desc = PAGE(pa) | ATTR(0) | SH_INNER | AP_EL0;

		if (pa == NC)
			desc = PAGE(pa) | ATTR(2) | AP_EL0;
		else if (pa == DEV)
			desc = PAGE(pa) | ATTR(1);
		else if (pa == EL1_ONLY)
			desc = PAGE(pa) | ATTR(0) | SH_INNER;
		else if (pa == ALL_RO)
			desc = PAGE(pa) | ATTR(0) | SH_INNER | AP_EL0 | AP_RO;
		else if (pa == HOLE)
			desc = PAGE(0x300000) | ATTR(0) | SH_INNER | AP_EL0;
		put(m, L3 + (pa >> 12) * 8, desc);
	}
	c->sys.scr_el3 = ARM64_SCR_RW | ARM64_SCR_NS;
	c->sys.hcr_el2 = ARM64_HCR_RW;
	c->sys.tcr[r] = r == 1 ? TCR_EL1 : TCR_ELX;
	c->sys.ttbr0[r] = L1;
	c->sys.mair[r] = MAIR;
	c->sys.sctlr[r] |= ARM64_SCTLR_M | ARM64_SCTLR_C;
	c->el = el;
	arm64_tlb_flush(c);
}

/* A machine with 1MB of RAM and two Cortex-A53s at EL3, the MMU on. */
static Machine *two_pes(Arm64Cpu **a, Arm64Cpu **b)
{
	Machine *m = machine_create();

	CHECK(m != NULL);
	CHECK_EQ(bus_map_ram(m->bus, "ram", 0, 0x100000), BUS_OK);
	*a = add_a53(m, 0x80000000);
	*b = add_a53(m, 0x80000001);
	mmu_on(m, *a, 3);
	mmu_on(m, *b, 3);
	return m;
}

static void run(Arm64Cpu *c, uint32_t insn)
{
	CHECK_EQ(arm64_execute(c, insn), ARM64_EXEC_OK);
}

/*
 * Run insn, which must take a Data Abort with dfsc to the current level:
 * its ISS.WnR, whether it was a write.
 */
static bool aborts(Arm64Cpu *c, uint32_t insn, unsigned dfsc)
{
	unsigned el = c->el;
	uint64_t esr;

	CHECK_EQ(arm64_execute(c, insn), ARM64_EXEC_EXCEPTION);
	arm64_take_exception(c);
	esr = c->sys.esr[c->el];
	CHECK_EQ(esr >> 26, EC_DATA_ABORT_SAME);
	CHECK_EQ(esr & 0x3f, dfsc);
	c->el = el;
	return esr & (1u << 6);
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

/* LDXR and STXR of each size, and the acquire and release forms. */
static void test_load_store_exclusive(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	for (unsigned size = 0; size < 4; size++) {
		for (unsigned o0 = 0; o0 <= 1; o0++) {
			uint64_t mask = size == 3 ? ~0ull : (1ull << (8 << size)) - 1;

			put(m, WB, 0x1122334455667788);
			a->x[1] = WB;
			a->x[3] = 0xa1a2a3a4a5a6a7a8;
			run(a, ex(size, 0, 1, 0, 31, o0, 31, 1, 0)); /* LD(A)XR */
			CHECK_EQ(a->x[0], 0x1122334455667788 & mask);
			CHECK(a->monitor.exclusive);
			run(a, ex(size, 0, 0, 0, 2, o0, 31, 1, 3)); /* ST(L)XR W2 */
			CHECK_EQ(a->x[2], 0);
			CHECK(!a->monitor.exclusive);
			CHECK_EQ(get(m, WB), (0x1122334455667788 & ~mask) |
			                         (0xa1a2a3a4a5a6a7a8 & mask));
			/* Again, with the monitor open: no store. */
			a->x[3] = 0;
			run(a, ex(size, 0, 0, 0, 2, o0, 31, 1, 3));
			CHECK_EQ(a->x[2], 1);
			CHECK_EQ(get(m, WB) & mask, 0xa1a2a3a4a5a6a7a8 & mask);
		}
	}

	/* The pairs: 2 × 32 and 2 × 64 bits; Rt == Rt2 gets the second. */
	put(m, WB, 0x1111111122222222);
	put(m, WB + 8, 0x3333333344444444);
	a->x[1] = WB;
	run(a, ldxp(3, 0, 4, 1));
	CHECK_EQ(a->x[0], 0x1111111122222222);
	CHECK_EQ(a->x[4], 0x3333333344444444);
	a->x[3] = 5;
	a->x[5] = 6;
	run(a, stxp(3, 2, 3, 5, 1));
	CHECK_EQ(a->x[2], 0);
	CHECK_EQ(get(m, WB), 5);
	CHECK_EQ(get(m, WB + 8), 6);
	run(a, ldxp(2, 0, 4, 1));
	CHECK_EQ(a->x[0], 5);
	CHECK_EQ(a->x[4], 0);
	a->x[3] = 0xffffffff00000007;
	a->x[5] = 0xffffffff00000008;
	run(a, ex(2, 0, 0, 1, 2, 1, 5, 1, 3)); /* STLXP W2, W3, W5 */
	CHECK_EQ(a->x[2], 0);
	CHECK_EQ(get(m, WB), 0x0000000800000007);
	run(a, ldxp(3, 4, 4, 1));
	CHECK_EQ(a->x[4], 6);
	run(a, CLREX);

	/* The status register is WZR, or one of the others (UNKNOWN: kept). */
	run(a, ldxr(3, 0, 1));
	a->x[3] = 9;
	run(a, stxr(3, 31, 3, 1));
	CHECK_EQ(get(m, WB), 9);
	run(a, ldxr(3, 0, 1));
	a->x[3] = 10;
	run(a, stxr(3, 3, 3, 1)); /* Rs == Rt: Rt's value is stored */
	CHECK_EQ(get(m, WB), 10);
	CHECK_EQ(a->x[3], 0);
	run(a, ldxr(3, 0, 1));
	a->x[0] = 11;
	run(a, stxr(3, 1, 0, 1)); /* Rs == Rn: the address from before */
	CHECK_EQ(get(m, WB), 11);
	CHECK_EQ(a->x[1], 0);
	machine_destroy(m);
}

/* What the monitor holds, and what clears it. */
static void test_monitor(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	a->x[1] = WB;
	/* A Store-Exclusive must match its Load-Exclusive. */
	run(a, ldxr(3, 0, 1));
	a->x[4] = WB + 8; /* same block, another address */
	run(a, stxr(3, 2, 3, 4));
	CHECK_EQ(a->x[2], 1);
	CHECK(!a->monitor.exclusive);
	run(a, ldxr(2, 0, 1)); /* 4 bytes, then 8 */
	run(a, stxr(3, 2, 3, 1));
	CHECK_EQ(a->x[2], 1);
	run(a, ldxp(3, 0, 5, 1)); /* two registers, then one */
	run(a, stxr(3, 2, 3, 1));
	CHECK_EQ(a->x[2], 1);
	run(a, ldxr(3, 0, 1)); /* one, then two */
	run(a, stxp(3, 2, 3, 5, 1));
	CHECK_EQ(a->x[2], 1);

	/* CLREX and an exception return clear it; the PE's own store not. */
	run(a, ldxr(3, 0, 1));
	run(a, CLREX);
	CHECK(!a->monitor.exclusive);
	run(a, ldxr(3, 0, 1));
	a->sys.spsr[3] = 0xd; /* EL3h */
	a->sys.elr[3] = 0x1000;
	run(a, ERET);
	CHECK(!a->monitor.exclusive);
	run(a, ldxr(3, 0, 1));
	a->x[0] = 1;
	run(a, STR_X0_X1);
	CHECK(a->monitor.exclusive);
	run(a, stxr(3, 2, 3, 1));
	CHECK_EQ(a->x[2], 0);

	/* A reset clears it. */
	run(a, ldxr(3, 0, 1));
	a->cpu.ops->reset(&a->cpu);
	CHECK(!a->monitor.exclusive);
	machine_destroy(m);
}

/* Another PE's write to the block clears the mark, and wakes WFE. */
static void test_two_pes(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	a->x[1] = WB;
	b->x[1] = WB + 0x38; /* the same 64-byte block */
	run(a, ldxr(3, 0, 1));
	b->x[0] = 1;
	run(b, STR_X0_X1);
	CHECK(!a->monitor.exclusive);
	run(a, stxr(3, 2, 3, 1));
	CHECK_EQ(a->x[2], 1);

	/* A write to the next block leaves it. */
	run(a, ldxr(3, 0, 1));
	b->x[1] = WB + 0x40;
	run(b, STR_X0_X1);
	CHECK(a->monitor.exclusive);

	/* B's own Load-Exclusive and Store-Exclusive clear A's too. */
	b->x[1] = WB + 8;
	run(b, ldxr(3, 0, 1));
	CHECK(a->monitor.exclusive);
	run(b, stxr(3, 2, 3, 1));
	CHECK_EQ(b->x[2], 0);
	CHECK(!a->monitor.exclusive);

	/* A waits in WFE after its Load-Exclusive; B's write wakes it. */
	run(a, ldxr(3, 0, 1));
	a->event_register = false;
	run(a, WFE);
	CHECK_EQ(a->cpu.state, CPU_HALTED);
	b->x[1] = WB + 0x100; /* elsewhere: A sleeps on */
	run(b, STR_X0_X1);
	CHECK_EQ(a->cpu.state, CPU_HALTED);
	b->x[1] = WB;
	run(b, STR_X0_X1);
	CHECK_EQ(a->cpu.state, CPU_RUNNING);
	CHECK(!a->event_register);

	/* Not waiting: the event stays for the next WFE. DC ZVA writes too. */
	run(a, ldxr(3, 0, 1));
	run(b, DC_ZVA_X1);
	CHECK(!a->monitor.exclusive);
	CHECK(a->event_register);
	CHECK_EQ(get(m, WB), 0);

	/* A store that crosses into the block, from the one before. */
	run(a, ldxr(3, 0, 1));
	b->x[1] = WB - 4;
	run(b, STR_X0_X1);
	CHECK(!a->monitor.exclusive);
	machine_destroy(m);
}

/* Exclusive and ordered accesses must be aligned to their whole size. */
static void test_alignment(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	a->x[1] = WB + 4;
	CHECK(!aborts(a, ldxr(3, 0, 1), DFSC_ALIGNMENT));
	CHECK(aborts(a, stxr(3, 2, 3, 1), DFSC_ALIGNMENT)); /* even with no mark */
	CHECK(!aborts(a, ldar(3, 0, 1), DFSC_ALIGNMENT));
	CHECK(aborts(a, stlr(3, 0, 1), DFSC_ALIGNMENT));
	a->x[1] = WB + 8;
	CHECK(!aborts(a, ldxp(3, 0, 4, 1), DFSC_ALIGNMENT)); /* 16 bytes */
	CHECK(aborts(a, stxp(3, 2, 3, 4, 1), DFSC_ALIGNMENT));
	run(a, ldxp(2, 0, 4, 1)); /* 8 bytes */
	a->x[1] = WB + 1;
	run(a, ldar(0, 0, 1)); /* a byte */
	aborts(a, ldar(1, 0, 1), DFSC_ALIGNMENT);
	CHECK(!(a->sys.sctlr[3] & ARM64_SCTLR_A)); /* whatever SCTLR.A says */
	machine_destroy(m);
}

/* LDAR and STLR are loads and stores, on any memory. */
static void test_ordered(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	put(m, WB, 0x8877665544332211);
	a->x[1] = WB;
	run(a, ldar(1, 0, 1));
	CHECK_EQ(a->x[0], 0x2211);
	run(a, ldar(3, 0, 1));
	CHECK_EQ(a->x[0], 0x8877665544332211);
	a->x[0] = 0xabcd;
	run(a, stlr(0, 0, 1));
	CHECK_EQ(get(m, WB), 0x88776655443322cd);
	/* An ordered store clears another PE's mark, as any write does. */
	run(b, ldxr(3, 0, 1));
	b->x[1] = WB;
	run(b, ldxr(3, 0, 1));
	run(a, stlr(2, 0, 1));
	CHECK(!b->monitor.exclusive);
	a->x[1] = NC;
	run(a, ldar(3, 0, 1));
	machine_destroy(m);
}

/* Only Normal Write-Back memory with the cache on: the A53's monitor. */
static void test_memory_types(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	a->x[1] = NC;
	stops(a, ldxr(3, 0, 1), "exclusive access to memory of attributes 0x44");
	a->x[1] = DEV;
	stops(a, ldxr(3, 0, 1), "attributes 0x00");
	/* A Store-Exclusive with no mark does not look. */
	run(a, stxr(3, 2, 3, 1));
	CHECK_EQ(a->x[2], 1);
	a->x[1] = WB;
	run(a, ldxr(3, 0, 1));
	a->x[1] = NC;
	stops(a, stxr(3, 2, 3, 1), "attributes 0x44");
	a->sys.sctlr[3] &= ~ARM64_SCTLR_C; /* the data cache off */
	a->x[1] = WB;
	stops(a, ldxr(3, 0, 1), "attributes 0xff");
	a->sys.sctlr[3] &= ~ARM64_SCTLR_M; /* no translation: Device */
	stops(a, ldxr(3, 0, 1), "attributes 0x00");

	/* A Store-Exclusive whose translation faults keeps the mark. */
	a->sys.sctlr[3] |= ARM64_SCTLR_M | ARM64_SCTLR_C;
	run(a, ldxr(3, 0, 1));
	a->x[1] = 0x200000; /* not mapped */
	CHECK_EQ(arm64_execute(a, stxr(3, 2, 3, 1)), ARM64_EXEC_EXCEPTION);
	CHECK(a->monitor.exclusive);
	machine_destroy(m);
}

/* The encodings of the class that are newer than Armv8.0. */
static void test_undefined(void)
{
	const uint32_t insns[] = {
		ex(3, 1, 1, 1, 2, 1, 31, 1, 0),  /* CASAL */
		ex(3, 1, 0, 1, 2, 0, 31, 1, 0),  /* CAS */
		ex(3, 1, 1, 0, 31, 0, 31, 1, 0), /* LDLAR */
		ex(3, 1, 0, 0, 31, 0, 31, 1, 0), /* STLLR */
		ex(0, 0, 1, 1, 2, 0, 31, 1, 0),  /* CASPA */
		ex(1, 0, 0, 1, 2, 1, 31, 1, 0),  /* CASPL */
	};
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	a->x[1] = WB;
	for (size_t i = 0; i < ARRAY_SIZE(insns); i++)
		CHECK_EQ(arm64_execute(a, insns[i]), ARM64_EXEC_UNDEFINED);
	machine_destroy(m);
}

/* LDTR and STTR check EL0's permissions at EL1. */
static void test_unprivileged(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	mmu_on(m, a, 1);
	put(m, EL1_ONLY, 0x80);
	put(m, EL0_RW, 0x81);
	a->x[1] = EL1_ONLY;
	run(a, LDR_X0_X1);
	CHECK_EQ(a->x[0], 0x80);
	aborts(a, LDTR_X0_X1, 0x0f); /* permission fault, level 3 */
	run(a, LDR_X0_X1);           /* EL1's page entry is still there */
	run(a, LDUR_X0_X1);
	a->x[2] = 0;
	run(a, LDR_REG);
	run(a, LDR_POST);
	a->x[1] = EL1_ONLY;
	run(a, LDR_PRE);
	a->x[1] = EL1_ONLY;
	a->x[1] = EL0_RW;
	run(a, LDTR_X0_X1);
	CHECK_EQ(a->x[0], 0x81);
	run(a, LDTRSB_X0_X1);
	CHECK_EQ(a->x[0], 0xffffffffffffff81);
	put(m, EL0_RW + 8, 0x82);
	run(a, 0xf8408820); /* LDTR X0, [X1, #8]: no writeback */
	CHECK_EQ(a->x[0], 0x82);
	CHECK_EQ(a->x[1], EL0_RW);
	a->x[0] = 0x55;
	run(a, STTR_X0_X1);
	CHECK_EQ(get(m, EL0_RW), 0x55);
	a->x[1] = ALL_RO;
	run(a, LDTR_X0_X1);
	aborts(a, STTR_X0_X1, 0x0f);

	/* At EL0 and at EL3, they are LDR and STR. */
	a->el = 0;
	a->x[1] = EL0_RW;
	run(a, LDTR_X0_X1);
	CHECK_EQ(a->x[0], 0x55);
	mmu_on(m, b, 3);
	b->x[1] = EL1_ONLY;
	run(b, LDTR_X0_X1);
	CHECK_EQ(b->x[0], 0x80);

	/* No prefetch and no SIMD&FP register in the class. */
	CHECK_EQ(arm64_execute(a, 0xf8800820), ARM64_EXEC_UNDEFINED); /* opc 2 */
	CHECK_EQ(arm64_execute(a, 0x3c400820), ARM64_EXEC_UNDEFINED); /* V */
	machine_destroy(m);
}

/*
 * First, with no monitor marked before: the count of marked monitors
 * that spares the search must follow every mark and clear.
 */
static void test_count(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	a->x[1] = WB;
	b->x[1] = WB;
	run(a, ldxr(3, 0, 1));
	run(b, CLREX); /* B's monitor was open: A's mark still counts */
	run(b, STR_X0_X1);
	CHECK(!a->monitor.exclusive);
	run(a, ldxr(3, 0, 1));
	run(a, ldxr(3, 0, 1)); /* marked twice, counted once */
	run(a, CLREX);
	run(b, ldxr(3, 0, 1));
	run(a, STR_X0_X1);
	CHECK(!b->monitor.exclusive);
	machine_destroy(m);
}

/* What a write must cover to clear a mark, through every path. */
static void test_write_paths(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	/* 8 bytes that end where the block starts leave it. */
	a->x[1] = WB + 0x40;
	run(a, ldxr(3, 0, 1));
	b->x[1] = WB + 0x38;
	run(b, STR_X0_X1);
	CHECK(a->monitor.exclusive);
	/* An unaligned halfword with one byte in the block clears it. */
	b->x[1] = WB + 0x3f;
	run(b, STRH_W0_X1);
	CHECK(!a->monitor.exclusive);

	/* A byte marks the block, not the byte: the next block is apart. */
	a->x[1] = WB + 1;
	run(a, ex(0, 0, 1, 0, 31, 0, 31, 1, 0)); /* LDXRB */
	run(a, ex(0, 0, 0, 0, 2, 0, 31, 1, 3));  /* STXRB W2, W3 */
	CHECK_EQ(a->x[2], 0);
	run(a, ex(0, 0, 1, 0, 31, 0, 31, 1, 0));
	b->x[1] = WB + 0x40;
	run(b, STRB_W0_X1);
	CHECK(a->monitor.exclusive);
	b->x[1] = WB + 0x3f;
	run(b, STRB_W0_X1);
	CHECK(!a->monitor.exclusive);

	/* A store across two pages clears the blocks on either side. */
	b->x[1] = WB + 0xffc;
	a->x[1] = WB + 0x1000;
	run(a, ldxr(3, 0, 1));
	run(b, STR_X0_X1);
	CHECK(!a->monitor.exclusive);
	a->x[1] = WB + 0xfc0;
	run(a, ldxr(3, 0, 1));
	run(b, STR_X0_X1);
	CHECK(!a->monitor.exclusive);
	/* Blocks past the bytes written keep their mark. */
	a->x[1] = WB + 0x1040;
	run(a, ldxr(3, 0, 1));
	run(b, STR_X0_X1);
	CHECK(a->monitor.exclusive);
	a->x[1] = WB + 0xfc0;
	run(a, ldxr(3, 0, 1));
	/* A load across them does not. */
	run(a, ldxr(3, 0, 1));
	run(b, LDR_X0_X1);
	CHECK(a->monitor.exclusive);
	run(a, CLREX);

	/* A write through a page entry that an earlier write filled. */
	b->x[1] = WB + 0x2801;
	run(b, STRB_W0_X1);
	a->x[1] = WB + 0x2040;
	run(a, ldxr(3, 0, 1));
	b->x[1] = WB + 0x2038; /* the block before: no more */
	run(b, STR_X0_X1);
	CHECK(a->monitor.exclusive);
	b->x[1] = WB + 0x2040;
	run(b, STR_X0_X1);
	CHECK(!a->monitor.exclusive);
	machine_destroy(m);
}

/* Each kind of Normal memory, in each Exception level's regime. */
static void test_attributes(void)
{
	static const struct {
		uint8_t attr;
		bool write_back;
	} attrs[] = {
		{0xff, true},  {0x55, true},  {0x66, true},  {0x77, true},
		{0xf5, true},  {0x5f, true},  {0xcc, true},  {0x44, false},
		{0xaa, false}, {0x22, false}, {0xbb, false}, {0xf4, false},
		{0x4f, false}, {0xfa, false}, {0xaf, false}, {0x00, false},
	};
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	a->x[1] = WB;
	for (size_t i = 0; i < ARRAY_SIZE(attrs); i++) {
		a->sys.mair[3] = attrs[i].attr;
		arm64_tlb_flush(a);
		if (attrs[i].write_back) {
			run(a, ldxr(3, 0, 1));
			run(a, CLREX);
		} else {
			stops(a, ldxr(3, 0, 1), "exclusive access to memory");
		}
	}

	/* The data cache of the regime: SCTLR_EL1 for EL0 and EL1. */
	for (unsigned el = 0; el <= 2; el++) {
		unsigned r = el > 1 ? el : 1;

		mmu_on(m, a, el > 1 ? el : 1);
		a->el = el;
		for (unsigned other = 1; other <= 3; other++)
			if (other != r)
				a->sys.sctlr[other] &= ~ARM64_SCTLR_C;
		run(a, ldxr(3, 0, 1));
		run(a, CLREX);
		a->sys.sctlr[r] &= ~ARM64_SCTLR_C;
		stops(a, ldxr(3, 0, 1), "exclusive access to memory");
		for (unsigned other = 1; other <= 3; other++)
			if (other != r)
				a->sys.sctlr[other] |= ARM64_SCTLR_C;
		stops(a, ldxr(3, 0, 1), "exclusive access to memory");
	}
	machine_destroy(m);
}

/* SP as the base, X16 to X30, and what goes wrong on the way. */
static void test_registers_and_errors(void)
{
	Arm64Cpu *a, *b;
	Machine *m = two_pes(&a, &b);

	/* SP: checked for alignment when SCTLR_EL3.SA is set. */
	a->sp_sel = true;
	a->sp_el[3] = WB;
	put(m, WB, 77);
	run(a, ldxr(3, 0, 31));
	CHECK_EQ(a->x[0], 77);
	a->x[3] = 78;
	run(a, stxr(3, 2, 3, 31));
	CHECK_EQ(a->x[2], 0);
	run(a, ldar(3, 0, 31));
	CHECK_EQ(a->x[0], 78);
	a->sys.sctlr[3] |= ARM64_SCTLR_SA;
	a->sp_el[3] = WB + 8;
	CHECK_EQ(arm64_execute(a, ldxr(3, 0, 31)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(arm64_execute(a, stxr(3, 2, 3, 31)), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(arm64_execute(a, stlr(3, 0, 31)), ARM64_EXEC_EXCEPTION);
	a->sys.sctlr[3] &= ~ARM64_SCTLR_SA;

	/* High registers in every field. */
	a->x[21] = WB;
	put(m, WB, 0x10);
	put(m, WB + 8, 0x20);
	run(a, ldxr(3, 20, 21));
	CHECK_EQ(a->x[20], 0x10);
	a->x[22] = 0x11;
	a->x[17] = 0xdead;
	a->x[1] = 0xdead;
	run(a, stxr(3, 17, 22, 21));
	CHECK_EQ(a->x[1], 0xdead);
	CHECK_EQ(a->x[17], 0);
	CHECK_EQ(get(m, WB), 0x11);
	run(a, ldxp(3, 23, 24, 21));
	CHECK_EQ(a->x[24], 0x20);
	a->x[25] = 0x12;
	a->x[26] = 0x22;
	a->x[18] = 0xdead;
	run(a, stxp(3, 18, 25, 26, 21));
	CHECK_EQ(a->x[18], 0);
	CHECK_EQ(get(m, WB + 8), 0x22);

	/* A walk that reads where no RAM is stops; no mark is left. */
	a->sys.ttbr0[3] = 0x300000;
	arm64_tlb_flush(a);
	a->x[1] = WB;
	CHECK_EQ(arm64_execute(a, ldxr(3, 0, 1)), ARM64_EXEC_DATA_ABORT);
	CHECK(!a->monitor.exclusive);
	a->sys.ttbr0[3] = L1;
	arm64_tlb_flush(a);
	/* So does a load from such a page, with no mark either. */
	a->x[1] = HOLE;
	CHECK_EQ(arm64_execute(a, ldxr(3, 0, 1)), ARM64_EXEC_DATA_ABORT);
	CHECK(!a->monitor.exclusive);

	/* A Store-Exclusive that may not write faults, and keeps the mark. */
	mmu_on(m, a, 1);
	a->x[1] = ALL_RO;
	run(a, ldxr(3, 0, 1));
	CHECK_EQ(aborts(a, stxr(3, 2, 3, 1), 0x0f), true);
	CHECK(a->monitor.exclusive);

	/* Bit 24 set: another class, not implemented. */
	CHECK_EQ(arm64_execute(a, 0xc9000020), ARM64_EXEC_UNIMPLEMENTED);
	machine_destroy(m);
}

int main(void)
{
	RUN_TEST(test_count); /* first: see there */
	RUN_TEST(test_load_store_exclusive);
	RUN_TEST(test_monitor);
	RUN_TEST(test_two_pes);
	RUN_TEST(test_alignment);
	RUN_TEST(test_ordered);
	RUN_TEST(test_memory_types);
	RUN_TEST(test_undefined);
	RUN_TEST(test_unprivileged);
	RUN_TEST(test_write_paths);
	RUN_TEST(test_attributes);
	RUN_TEST(test_registers_and_errors);
	return test_summary();
}
