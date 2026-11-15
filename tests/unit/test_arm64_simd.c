/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the SIMD&FP registers of the AArch64 model: FPCR and
 * FPSR, the traps of CPACR_EL1, CPTR_EL2 and CPTR_EL3, the SIMD&FP loads
 * and stores (load_store.c, simd_ldst.c, mem.c) and the moves of simd.c.
 * Every encoding was produced by the GNU assembler, unless a comment says
 * otherwise; binutils disassembles those built by hand as undefined.
 * Section numbers refer to the Arm Architecture Reference Manual, ARM DDI
 * 0487 M.d.
 */

#include <stdint.h>
#include <string.h>

#include "arm64_test.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/macros.h"

#define DATA 0x1000 /* data area in the test machine's RAM */

#define EC_DATA_ABORT_SAME 0x25
#define EC_SP_ALIGNMENT 0x26
#define DFSC_ALIGNMENT 0x21
#define ESR_FP UINT32_C(0x1fe00000) /* EC 0x07, IL, CV = 1, COND = 1110 */
#define ESR_UNKNOWN UINT32_C(0x02000000)

/* Instructions used by several tests. */
#define LDR_Q0_X1 0x3dc00020u   /* LDR Q0, [X1] */
#define LDR_Q0_SP 0x3dc003e0u   /* LDR Q0, [SP] */
#define FMOV_D0_X1 0x9e670020u  /* FMOV D0, X1 */
#define MRS_X0_FPCR 0xd53b4400u /* MRS X0, FPCR */
#define MSR_FPCR_X0 0xd51b4400u /* MSR FPCR, X0 */
#define MRS_X0_FPSR 0xd53b4420u /* MRS X0, FPSR */
#define MSR_FPSR_X0 0xd51b4420u /* MSR FPSR, X0 */

/* The byte at DATA + i, after fill(). */
static uint8_t pattern(unsigned i)
{
	return (uint8_t)(i * 13 + 1);
}

/* The little-endian value of size bytes (at most 8) of the pattern at i. */
static uint64_t pattern_value(unsigned i, unsigned size)
{
	uint64_t value = 0;

	for (unsigned k = 0; k < size; k++)
		value |= (uint64_t)pattern(i + k) << (8 * k);
	return value;
}

/* Fill 8KB at DATA with the pattern. */
static void fill(Machine *m)
{
	for (unsigned i = 0; i < 0x2000; i++)
		CHECK_EQ(bus_write(m->bus, DATA + i, 1, pattern(i)), BUS_OK);
}

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

/*
 * Stage 1 translation for EL3, data cache on: the 1MB of RAM at 0 mapped
 * as Normal Write-Back memory at the same addresses, except the page at
 * DEVICE, which is Device-nGnRnE.
 */
#define L1 0x10000
#define L2 0x11000
#define L3 0x12000
#define DEVICE 0x81000

static void mmu_on(Machine *m, Arm64Cpu *c)
{
	poke(m, L1, 8, L2 | 3);
	poke(m, L2, 8, L3 | 3);
	for (uint64_t pa = 0; pa < 0x100000; pa += 0x1000) {
		/* AF, Inner Shareable, AttrIndx 0 or, for DEVICE, 1. */
		uint64_t desc = pa | 3 | 1u << 10 | 3u << 8;

		if (pa == DEVICE)
			desc = pa | 3 | 1u << 10 | 1u << 2;
		poke(m, L3 + (pa >> 12) * 8, 8, desc);
	}
	c->sys.tcr[3] = 0x20019; /* T0SZ 25, 4KB granule, 32-bit PA */
	c->sys.ttbr0[3] = L1;
	c->sys.mair[3] = 0x00ff; /* 0: Write-Back, 1: Device-nGnRnE */
	c->sys.sctlr[3] |= ARM64_SCTLR_M | ARM64_SCTLR_C;
	arm64_tlb_flush(c);
}

/*
 * Run insn, which must take a Data Abort with dfsc to the current EL, at
 * EL3 with VBAR_EL3 = 0: to the vector for the current EL with SP_ELx.
 */
static void check_abort(Machine *m, Arm64Cpu *c, uint32_t insn, unsigned dfsc)
{
	unsigned el = c->el;
	uint64_t pc = c->pc;

	c->sys.esr[el] = 0;
	c->sys.elr[el] = 0;
	CHECK_EQ(arm64_test_step(m, c, insn), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->pc, 0x200);
	CHECK_EQ(c->el, el);
	CHECK_EQ(c->sys.esr[el] >> 26, EC_DATA_ABORT_SAME);
	CHECK_EQ(c->sys.esr[el] & 0x3f, dfsc);
	CHECK_EQ(c->sys.elr[el], pc);
	c->pc = pc;
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
	static const uint32_t users[] = {LDR_Q0_X1, FMOV_D0_X1, MRS_X0_FPCR};

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

	/* Every kind of SIMD&FP instruction checks. */
	static const uint32_t kinds[] = {
		0x9c000080, /* LDR Q0, . + 16 */
		0x2d400420, /* LDP S0, S1, [X1] */
		0x3d000c20, /* STR B0, [X1, #3] */
		0x4c407020, /* LD1 {V0.16B}, [X1] */
		0x0d409020, /* LD1 {V0.S}[1], [X1] */
		0x0d40c020, /* LD1R {V0.8B}, [X1] */
		0x4e1f0420, /* DUP V0.16B, V1.B[15] */
		0x4e080c20, /* DUP V0.2D, X1 */
		0x4e071c20, /* INS V0.B[3], W1 */
		0x6e162420, /* INS V0.H[5], V1.H[2] */
		0x0e133c20, /* UMOV W0, V1.B[9] */
		0x4e032c20, /* SMOV X0, V1.B[1] */
		0x5e0b0420, /* DUP B0, V1.B[5] */
		0x4f02e740, /* MOVI V0.16B, #0x5a */
		0x6f00f400, /* FMOV V0.2D, #2.0 */
		0x1e260020, /* FMOV W0, S1 */
		0x9eaf0020, /* FMOV V0.D[1], X1 */
		0x1e204020, /* FMOV S0, S1 */
		0x1e2e1000, /* FMOV S0, #1.0 */
		MSR_FPSR_X0, MRS_X0_FPSR, MSR_FPCR_X0,
	};

	for (unsigned i = 0; i < ARRAY_SIZE(kinds); i++)
		check_trap(kinds[i], 1, 0, false, false, false, false, 1, ESR_FP);

	/*
	 * An UNDEFINED encoding is UNDEFINED, not trapped: decoding comes
	 * first. 0x7dc00020: LDR with size = 01 and opc = 11.
	 */
	check_trap(0x7dc00020, 1, 0, false, false, false, false, 1, ESR_UNKNOWN);
	check_trap(0x0e080420, 1, 0, false, false, false, false, 1, ESR_UNKNOWN);

	/* The trap comes before the SP alignment check, and changes nothing. */
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->sys.scr_el3 = ARM64_SCR_RW | ARM64_SCR_NS;
	c->sys.hcr_el2 = ARM64_HCR_RW;
	c->sys.sctlr[1] = ARM64_SCTLR_SA;
	c->el = 1;
	c->sp_el[1] = DATA + 8;
	CHECK_EQ(arm64_test_step(m, c, LDR_Q0_SP), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->sys.esr[1], ESR_FP);
	CHECK_V(c, 0, 0xeeeeeeeeeeeeee00, 0xdddddddddddddd00);
	c->el = 1;
	c->pc = RESET_PC;
	c->x[1] = DATA;
	CHECK_EQ(arm64_test_step(m, c, 0x3cdf0420), /* LDR Q0, [X1], #-16 */
	         MACHINE_STOP_LIMIT);
	CHECK_EQ(c->sys.esr[1], ESR_FP);
	CHECK_EQ(c->x[1], DATA);
	CHECK_V(c, 0, 0xeeeeeeeeeeeeee00, 0xdddddddddddddd00);
	/* With FPEN = 11, the SP alignment fault. */
	c->el = 1;
	c->pc = RESET_PC;
	c->sys.cpacr_el1 = 3u << 20;
	CHECK_EQ(arm64_test_step(m, c, LDR_Q0_SP), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->sys.esr[1] >> 26, EC_SP_ALIGNMENT);
	machine_destroy(m);
}

/* Check that Vn holds the 16 bytes of the pattern at DATA + i. */
#define CHECK_Q(c, n, i)                                                       \
	CHECK_V(c, n, pattern_value(i, 8), pattern_value((i) + 8, 8))

static void test_register_forms(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->x[1] = DATA;
	/* Loads write the whole register, clearing the bits above. */
	arm64_test_exec(m, c, 0x3d400c20); /* LDR B0, [X1, #3] */
	CHECK_V(c, 0, pattern_value(3, 1), 0);
	arm64_test_exec(m, c, 0x7d400c21); /* LDR H1, [X1, #6] */
	CHECK_V(c, 1, pattern_value(6, 2), 0);
	arm64_test_exec(m, c, 0xbd400822); /* LDR S2, [X1, #8] */
	CHECK_V(c, 2, pattern_value(8, 4), 0);
	arm64_test_exec(m, c, 0xfd400823); /* LDR D3, [X1, #16] */
	CHECK_V(c, 3, pattern_value(16, 8), 0);
	arm64_test_exec(m, c, 0x3dc00824); /* LDR Q4, [X1, #32]: scaled by 16 */
	CHECK_Q(c, 4, 32);
	CHECK_EQ(c->x[1], DATA);

	/* Stores write their size only. */
	set_v(c, 0, 0x8877665544332211, 0x00ffeeddccbbaa99);
	arm64_test_exec(m, c, 0x3d000c20); /* STR B0, [X1, #3] */
	CHECK_EQ(peek(m, DATA, 8),
	         (pattern_value(0, 8) & ~UINT64_C(0xff000000)) | 0x11000000);
	arm64_test_exec(m, c, 0x7d000c20); /* STR H0, [X1, #6] */
	CHECK_EQ(peek(m, DATA + 6, 2), 0x2211);
	CHECK_EQ(peek(m, DATA + 8, 1), pattern(8));
	arm64_test_exec(m, c, 0xbd000820); /* STR S0, [X1, #8] */
	CHECK_EQ(peek(m, DATA + 8, 8),
	         (pattern_value(8, 8) & ~UINT64_C(0xffffffff)) | 0x44332211);
	arm64_test_exec(m, c, 0xfd000820); /* STR D0, [X1, #16] */
	CHECK_EQ(peek(m, DATA + 16, 8), 0x8877665544332211);
	CHECK_EQ(peek(m, DATA + 24, 8), pattern_value(24, 8));
	arm64_test_exec(m, c, 0x3d800820); /* STR Q0, [X1, #32] */
	CHECK_EQ(peek(m, DATA + 32, 8), 0x8877665544332211);
	CHECK_EQ(peek(m, DATA + 40, 8), 0x00ffeeddccbbaa99);
	CHECK_EQ(peek(m, DATA + 48, 8), pattern_value(48, 8));
	arm64_test_exec(m, c, 0x3d80003f); /* STR Q31, [X1] */
	CHECK_EQ(peek(m, DATA, 8), 0xeeeeeeeeeeeeee1f);
	CHECK_EQ(peek(m, DATA + 8, 8), 0xdddddddddddddd1f);
	fill(m);

	/* Pre-index, post-index and unscaled forms. */
	arm64_test_exec(m, c, 0x3cc10c20); /* LDR Q0, [X1, #16]! */
	CHECK_Q(c, 0, 16);
	CHECK_EQ(c->x[1], DATA + 16);
	arm64_test_exec(m, c, 0x3cdf0425); /* LDR Q5, [X1], #-16 */
	CHECK_Q(c, 5, 16);
	CHECK_EQ(c->x[1], DATA);
	set_v(c, 0, 0x0123456789abcdef, 0xfedcba9876543210);
	arm64_test_exec(m, c, 0xfc008c20); /* STR D0, [X1, #8]! */
	CHECK_EQ(c->x[1], DATA + 8);
	CHECK_EQ(peek(m, DATA + 8, 8), 0x0123456789abcdef);
	arm64_test_exec(m, c, 0xbc004420); /* STR S0, [X1], #4 */
	CHECK_EQ(c->x[1], DATA + 12);
	CHECK_EQ(peek(m, DATA + 8, 8), 0x0123456789abcdef);
	fill(m);
	c->x[1] = DATA + 16;
	arm64_test_exec(m, c, 0x3cdf0026); /* LDUR Q6, [X1, #-16] (built) */
	CHECK_Q(c, 6, 0);
	c->x[1] = DATA;

	/* Register offsets: the shift is the size, 4 for Q. */
	c->x[2] = 2;
	arm64_test_exec(m, c, 0x3ce27820); /* LDR Q0, [X1, X2, LSL #4] */
	CHECK_Q(c, 0, 32);
	c->x[1] = DATA + 64;
	c->x[2] = 0xfffffffffffffffe;      /* W2 = -2 as SXTW */
	arm64_test_exec(m, c, 0xfc62d820); /* LDR D0, [X1, W2, SXTW #3] */
	CHECK_V(c, 0, pattern_value(48, 8), 0);
	c->x[1] = DATA;
	c->x[2] = 5;
	arm64_test_exec(m, c, 0x3c626820); /* LDR B0, [X1, X2] */
	CHECK_V(c, 0, pattern(5), 0);
	arm64_test_exec(m, c, 0x3c627820); /* LDR B0, [X1, X2, LSL #0] */
	CHECK_V(c, 0, pattern(5), 0);
	set_v(c, 0, 0xbeef, 0);
	arm64_test_exec(m, c, 0x7c227820); /* STR H0, [X1, X2, LSL #1] */
	CHECK_EQ(peek(m, DATA + 10, 2), 0xbeef);
	fill(m);

	/* Literals: 4, 8 or 16 bytes after the PC. */
	poke(m, RESET_PC + 8, 8, 0x1122334455667788);
	poke(m, RESET_PC + 16, 8, 0x99aabbccddeeff00);
	poke(m, RESET_PC + 24, 8, 0x0f1e2d3c4b5a6978);
	c->pc = RESET_PC;
	arm64_test_exec(m, c, 0x1c000040); /* LDR S0, . + 8 */
	CHECK_V(c, 0, 0x55667788, 0);
	c->pc = RESET_PC;
	arm64_test_exec(m, c, 0x5c000040); /* LDR D0, . + 8 */
	CHECK_V(c, 0, 0x1122334455667788, 0);
	c->pc = RESET_PC;
	arm64_test_exec(m, c, 0x9c000080); /* LDR Q0, . + 16 */
	CHECK_V(c, 0, 0x99aabbccddeeff00, 0x0f1e2d3c4b5a6978);
	machine_destroy(m);
}

static void test_pairs(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->x[1] = DATA;
	arm64_test_exec(m, c, 0x2d400420); /* LDP S0, S1, [X1] */
	CHECK_V(c, 0, pattern_value(0, 4), 0);
	CHECK_V(c, 1, pattern_value(4, 4), 0);
	arm64_test_exec(m, c, 0x6d410420); /* LDP D0, D1, [X1, #16] */
	CHECK_V(c, 0, pattern_value(16, 8), 0);
	CHECK_V(c, 1, pattern_value(24, 8), 0);
	c->x[1] = DATA + 64;
	arm64_test_exec(m, c, 0xadff0420); /* LDP Q0, Q1, [X1, #-32]! */
	CHECK_Q(c, 0, 32);
	CHECK_Q(c, 1, 48);
	CHECK_EQ(c->x[1], DATA + 32);
	arm64_test_exec(m, c, 0xac400c22); /* LDNP Q2, Q3, [X1] */
	CHECK_Q(c, 2, 32);
	CHECK_Q(c, 3, 48);
	/* Rt == Rt2: the second value, one of the UNKNOWN values allowed. */
	arm64_test_exec(m, c, 0xad400020); /* LDP Q0, Q0, [X1] (built) */
	CHECK_Q(c, 0, 48);

	set_v(c, 0, 0x1111111111111111, 0x2222222222222222);
	set_v(c, 1, 0x3333333333333333, 0x4444444444444444);
	set_v(c, 2, 0x5555555566666666, 0x7777777777777777);
	set_v(c, 3, 0x88888888aaaaaaaa, 0x9999999999999999);
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0xac810420); /* STP Q0, Q1, [X1], #32 */
	CHECK_EQ(c->x[1], DATA + 32);
	CHECK_EQ(peek(m, DATA, 8), 0x1111111111111111);
	CHECK_EQ(peek(m, DATA + 8, 8), 0x2222222222222222);
	CHECK_EQ(peek(m, DATA + 16, 8), 0x3333333333333333);
	CHECK_EQ(peek(m, DATA + 24, 8), 0x4444444444444444);
	arm64_test_exec(m, c, 0x6c3f8c22); /* STNP D2, D3, [X1, #-8] */
	CHECK_EQ(peek(m, DATA + 24, 8), 0x5555555566666666);
	CHECK_EQ(peek(m, DATA + 32, 8), 0x88888888aaaaaaaa);
	CHECK_EQ(peek(m, DATA + 40, 8), pattern_value(40, 8));
	arm64_test_exec(m, c, 0x2d008c22); /* STP S2, S3, [X1, #4] */
	CHECK_EQ(peek(m, DATA + 32, 8), 0x66666666aaaaaaaa);
	CHECK_EQ(peek(m, DATA + 40, 8),
	         (pattern_value(40, 8) & ~UINT64_C(0xffffffff)) | 0xaaaaaaaa);
	machine_destroy(m);
}

/*
 * Alignment, Mem{} of J1.2: each register of a pair, or each element,
 * is aligned on its own size; 16 bytes at an address aligned to 8 are two
 * halves, aligned only if the whole is, and 32 bytes four aligned words.
 * With the MMU off, data is Device memory, which requires alignment.
 */
static void test_alignment(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->x[1] = DATA + 8;
	check_abort(m, c, LDR_Q0_X1, DFSC_ALIGNMENT);
	CHECK_V(c, 0, 0xeeeeeeeeeeeeee00, 0xdddddddddddddd00);
	CHECK_EQ(c->sys.far[3], DATA + 8);
	check_abort(m, c, 0x3d800020, DFSC_ALIGNMENT); /* STR Q0, [X1] */
	CHECK_EQ(peek(m, DATA + 8, 8), pattern_value(8, 8));
	arm64_test_exec(m, c, 0xad400420); /* LDP Q0, Q1, [X1]: four words */
	CHECK_Q(c, 0, 8);
	CHECK_Q(c, 1, 24);
	c->x[1] = DATA + 4;
	check_abort(m, c, 0x6d400420, DFSC_ALIGNMENT); /* LDP D0, D1, [X1] */
	c->pc = RESET_PC;
	arm64_test_exec(m, c, 0x2d400420); /* LDP S0, S1, [X1] */
	CHECK_V(c, 1, pattern_value(8, 4), 0);
	c->x[1] = DATA + 1;
	c->pc = RESET_PC;
	arm64_test_exec(m, c, 0x4c407020); /* LD1 {V0.16B}, [X1]: bytes */
	CHECK_Q(c, 0, 1);
	c->pc = RESET_PC;
	c->x[1] = DATA + 2;
	check_abort(m, c, 0x4c407820, DFSC_ALIGNMENT); /* LD1 {V0.4S}, [X1] */

	/* Normal memory needs no alignment, unless SCTLR_ELx.A asks. */
	mmu_on(m, c);
	c->x[1] = DATA + 8;
	arm64_test_exec(m, c, LDR_Q0_X1);
	CHECK_Q(c, 0, 8);
	c->pc = RESET_PC;
	c->x[1] = DATA + 0xff9; /* across a page */
	arm64_test_exec(m, c, LDR_Q0_X1);
	CHECK_V(c, 0, pattern_value(0xff9, 8), pattern_value(0x1001, 8));
	c->pc = RESET_PC;
	set_v(c, 0, 0x0706050403020100, 0x0f0e0d0c0b0a0908);
	arm64_test_exec(m, c, 0x3c801020); /* STUR Q0, [X1, #1] */
	CHECK_EQ(peek(m, DATA + 0xffa, 8), 0x0706050403020100);
	CHECK_EQ(peek(m, DATA + 0x1002, 8), 0x0f0e0d0c0b0a0908);
	CHECK_EQ(peek(m, DATA + 0xff9, 1), pattern(0xff9));
	CHECK_EQ(peek(m, DATA + 0x100a, 1), pattern(0x100a));
	c->pc = RESET_PC;
	c->x[1] = DATA + 3;
	arm64_test_exec(m, c, 0xad400420); /* LDP Q0, Q1, [X1]: unaligned */
	CHECK_Q(c, 0, 3);
	CHECK_Q(c, 1, 19);

	c->sys.sctlr[3] |= ARM64_SCTLR_A;
	c->pc = RESET_PC;
	c->x[1] = DATA + 8;
	check_abort(m, c, LDR_Q0_X1, DFSC_ALIGNMENT);
	c->x[1] = DATA + 16;               /* each register of the pair aligned */
	arm64_test_exec(m, c, 0xad400420); /* LDP Q0, Q1, [X1] */
	CHECK_Q(c, 1, 32);
	c->pc = RESET_PC;
	c->x[1] = DATA + 8;
	check_abort(m, c, 0xad400420, DFSC_ALIGNMENT); /* LDP Q0, Q1: on 8 */
	c->sys.sctlr[3] &= ~(uint64_t)ARM64_SCTLR_A;

	/* A Device page of the tables. */
	c->x[1] = DEVICE + 8;
	check_abort(m, c, LDR_Q0_X1, DFSC_ALIGNMENT);
	arm64_test_exec(m, c, 0xad400420); /* LDP Q0, Q1, [X1] */
	c->pc = RESET_PC;
	c->x[1] = DEVICE + 0xff8; /* the second half in a Normal page */
	check_abort(m, c, LDR_Q0_X1, DFSC_ALIGNMENT);
	c->x[1] = DEVICE - 8; /* the second half faults, in the Device page */
	check_abort(m, c, LDR_Q0_X1, DFSC_ALIGNMENT);
	CHECK_EQ(c->sys.far[3], DEVICE);
	c->x[1] = DEVICE - 4; /* unaligned: the Device page faults */
	check_abort(m, c, LDR_Q0_X1, DFSC_ALIGNMENT);
	CHECK_EQ(c->sys.far[3], DEVICE);
	machine_destroy(m);
}

/*
 * Check the registers of an LDn of multiple structures from DATA + base:
 * selem registers from t, elements of size bytes, datasize 8 or 16.
 * Element e of register t + s is structure element s of structure e.
 */
static void check_structures(Arm64Cpu *c, unsigned base, unsigned t,
                             unsigned selem, unsigned size, unsigned datasize)
{
	for (unsigned s = 0; s < selem; s++) {
		unsigned r = (t + s) % 32;

		for (unsigned e = 0; e < 16 / size; e++) {
			uint64_t want =
				e < datasize / size
					? pattern_value(base + (e * selem + s) * size, size)
					: 0;

			if (get_elem(c, r, e, size) != want) {
				fprintf(stderr,
				        "V%u element %u: 0x%" PRIx64 ", not 0x%" PRIx64 "\n", r,
				        e, get_elem(c, r, e, size), want);
				test_failures++;
			}
		}
	}
}

static void test_multiple_structures(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->x[1] = DATA;
	arm64_test_exec(m, c, 0x4c407020); /* LD1 {V0.16B}, [X1] */
	CHECK_Q(c, 0, 0);
	arm64_test_exec(m, c, 0x0c407020); /* LD1 {V0.8B}, [X1] */
	CHECK_V(c, 0, pattern_value(0, 8), 0);
	arm64_test_exec(m, c, 0x4c40ac20); /* LD1 {V0.2D, V1.2D}, [X1] */
	CHECK_Q(c, 0, 0);
	CHECK_Q(c, 1, 16);
	garbage(c);
	arm64_test_exec(m, c, 0x0c406c20); /* LD1 {V0.1D-V2.1D}, [X1] */
	CHECK_V(c, 0, pattern_value(0, 8), 0);
	CHECK_V(c, 1, pattern_value(8, 8), 0);
	CHECK_V(c, 2, pattern_value(16, 8), 0);
	CHECK_V(c, 3, 0xeeeeeeeeeeeeee03, 0xdddddddddddddd03);
	arm64_test_exec(m, c, 0x4c40283e); /* LD1 {V30.4S-V1.4S}, [X1] */
	CHECK_Q(c, 30, 0);
	CHECK_Q(c, 31, 16);
	CHECK_Q(c, 0, 32);
	CHECK_Q(c, 1, 48);
	CHECK_EQ(c->x[1], DATA);

	arm64_test_exec(m, c, 0x4c408420); /* LD2 {V0.8H, V1.8H}, [X1] */
	check_structures(c, 0, 0, 2, 2, 16);
	arm64_test_exec(m, c, 0x4c404820); /* LD3 {V0.4S-V2.4S}, [X1] */
	check_structures(c, 0, 0, 3, 4, 16);
	arm64_test_exec(m, c, 0x4c400020); /* LD4 {V0.16B-V3.16B}, [X1] */
	check_structures(c, 0, 0, 4, 1, 16);
	garbage(c);
	arm64_test_exec(m, c, 0x0c400020); /* LD4 {V0.8B-V3.8B}, [X1] */
	check_structures(c, 0, 0, 4, 1, 8);

	/* Stores interleave: a load of the same form gives the values back. */
	for (unsigned i = 0; i < 4; i++)
		set_v(c, i, 0x0706050403020100 + i * 0x1010101010101010,
		      0x0f0e0d0c0b0a0908 + i * 0x1010101010101010);
	c->x[1] = DATA + 0x100;
	arm64_test_exec(m, c, 0x4c008820); /* ST2 {V0.4S, V1.4S}, [X1] */
	CHECK_EQ(peek(m, DATA + 0x100, 8), 0x1312111003020100);
	CHECK_EQ(peek(m, DATA + 0x118, 8), 0x1f1e1d1c0f0e0d0c);
	CHECK_EQ(peek(m, DATA + 0x120, 1), pattern(0x120));
	arm64_test_exec(m, c, 0x0c004020); /* ST3 {V0.8B-V2.8B}, [X1] */
	CHECK_EQ(peek(m, DATA + 0x100, 8), 0x1202211101201000);
	CHECK_EQ(peek(m, DATA + 0x110, 8), 0x2717072616062515);
	CHECK_EQ(peek(m, DATA + 0x118, 1), 0x0c); /* from ST2 */
	arm64_test_exec(m, c, 0x4c000c20);        /* ST4 {V0.2D-V3.2D}, [X1] */
	CHECK_EQ(peek(m, DATA + 0x108, 8), 0x1716151413121110);
	CHECK_EQ(peek(m, DATA + 0x138, 8), 0x3f3e3d3c3b3a3938);
	set_v(c, 4, 0x8899aabbccddeeff, 0);
	arm64_test_exec(m, c, 0x0c007024); /* ST1 {V4.8B}, [X1] */
	CHECK_EQ(peek(m, DATA + 0x100, 8), 0x8899aabbccddeeff);
	CHECK_EQ(peek(m, DATA + 0x108, 8), 0x1716151413121110);
	arm64_test_exec(m, c, 0x4c007020); /* ST1 {V0.16B}, [X1] */
	CHECK_EQ(peek(m, DATA + 0x108, 8), 0x0f0e0d0c0b0a0908);

	/* Post-indexing by the size or by a register; SP as the base. */
	fill(m);
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0x4cdf7020); /* LD1 {V0.16B}, [X1], #16 */
	CHECK_EQ(c->x[1], DATA + 16);
	CHECK_Q(c, 0, 0);
	c->x[2] = (uint64_t)-8;
	arm64_test_exec(m, c, 0x4cc27020); /* LD1 {V0.16B}, [X1], X2 */
	CHECK_EQ(c->x[1], DATA + 8);
	CHECK_Q(c, 0, 16);
	arm64_test_exec(m, c, 0x4cdf0820); /* LD4 {V0.4S-V3.4S}, [X1], #64 */
	CHECK_EQ(c->x[1], DATA + 72);
	check_structures(c, 8, 0, 4, 4, 16);
	c->sp_el[3] = DATA + 0x200;
	set_v(c, 0, 1, 2);
	set_v(c, 1, 3, 4);
	arm64_test_exec(m, c, 0x4c9fafe0); /* ST1 {V0.2D, V1.2D}, [SP], #32 */
	CHECK_EQ(c->sp_el[3], DATA + 0x220);
	CHECK_EQ(peek(m, DATA + 0x200, 8), 1);
	CHECK_EQ(peek(m, DATA + 0x218, 8), 4);
	c->sp_el[3] = DATA + 0x208;
	c->sys.sctlr[3] |= ARM64_SCTLR_SA;
	CHECK_EQ(arm64_test_step(m, c, 0x4c9fafe0), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->sys.esr[3] >> 26, EC_SP_ALIGNMENT);
	machine_destroy(m);

	/*
	 * Elements load in order: when one fails, those before it are loaded.
	 * Here the second register is past the end of RAM.
	 */
	m = machine(&c);
	c->x[1] = 0x100000 - 8;
	CHECK_EQ(arm64_test_step(m, c, 0x0c40a020), /* LD1 {V0.8B, V1.8B} */
	         MACHINE_STOP_ERROR);
	CHECK_V(c, 0, peek(m, 0x100000 - 8, 8), 0);
	CHECK_V(c, 1, 0xeeeeeeeeeeeeee01, 0xdddddddddddddd01);
	machine_destroy(m);
}

static void test_single_structure(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* One element: the rest of the register stays. */
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0x0d409020); /* LD1 {V0.S}[1], [X1] */
	CHECK_V(c, 0, pattern_value(0, 4) << 32 | 0xeeeeee00, 0xdddddddddddddd00);
	arm64_test_exec(m, c, 0x4d401c20); /* LD1 {V0.B}[15], [X1] */
	CHECK_EQ(c->v[0].d[1], (uint64_t)pattern(0) << 56 | 0x00dddddddddddd00);
	arm64_test_exec(m, c, 0x4d405820); /* LD1 {V0.H}[7], [X1] */
	CHECK_EQ(c->v[0].d[1] >> 48, pattern_value(0, 2));
	arm64_test_exec(m, c, 0x4d408420); /* LD1 {V0.D}[1], [X1] */
	CHECK_V(c, 0, pattern_value(0, 4) << 32 | 0xeeeeee00, pattern_value(0, 8));
	arm64_test_exec(m, c, 0x0d605820); /* LD2 {V0.H, V1.H}[3], [X1] */
	CHECK_EQ(get_elem(c, 0, 3, 2), pattern_value(0, 2));
	CHECK_EQ(get_elem(c, 1, 3, 2), pattern_value(2, 2));
	CHECK_EQ(get_elem(c, 1, 2, 2), 0xeeee);
	arm64_test_exec(m, c, 0x4d40a020); /* LD3 {V0.S-V2.S}[2], [X1] */
	CHECK_EQ(get_elem(c, 0, 2, 4), pattern_value(0, 4));
	CHECK_EQ(get_elem(c, 1, 2, 4), pattern_value(4, 4));
	CHECK_EQ(get_elem(c, 2, 2, 4), pattern_value(8, 4));
	arm64_test_exec(m, c, 0x0d60a420); /* LD4 {V0.D-V3.D}[0], [X1] */
	for (unsigned i = 0; i < 4; i++)
		CHECK_EQ(c->v[i].d[0], pattern_value(8 * i, 8));
	CHECK_EQ(c->v[3].d[1], 0xdddddddddddddd03);

	set_v(c, 0, 0x1111111111111111, 0x0706050403020100);
	set_v(c, 1, 0, 0x1716151413121110);
	set_v(c, 2, 0, 0x2726252423222120);
	set_v(c, 3, 0, 0x3736353433323130);
	c->x[1] = DATA + 0x100;
	arm64_test_exec(m, c, 0x4d008420); /* ST1 {V0.D}[1], [X1] */
	CHECK_EQ(peek(m, DATA + 0x100, 8), 0x0706050403020100);
	CHECK_EQ(peek(m, DATA + 0x108, 1), pattern(0x108));
	arm64_test_exec(m, c, 0x4d202420); /* ST4 {V0.B-V3.B}[9], [X1] */
	CHECK_EQ(peek(m, DATA + 0x100, 8), 0x0706050431211101);

	/* Post-indexing, and V31 followed by V0. */
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0x0ddfb020); /* LD3 {V0.S-V2.S}[1], [X1], #12 */
	CHECK_EQ(c->x[1], DATA + 12);
	CHECK_EQ(get_elem(c, 2, 1, 4), pattern_value(8, 4));
	set_v(c, 31, 0, 0xaaaaaaaaaaaaaaaa);
	set_v(c, 0, 0, 0xbbbbbbbbbbbbbbbb);
	c->x[1] = DATA + 0x100;
	c->x[2] = 0x40;
	arm64_test_exec(m, c, 0x4da2843f); /* ST2 {V31.D, V0.D}[1], [X1], X2 */
	CHECK_EQ(c->x[1], DATA + 0x140);
	CHECK_EQ(peek(m, DATA + 0x100, 8), 0xaaaaaaaaaaaaaaaa);
	CHECK_EQ(peek(m, DATA + 0x108, 8), 0xbbbbbbbbbbbbbbbb);
	machine_destroy(m);
}

static void test_replicate(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	uint64_t b = pattern(0) * UINT64_C(0x0101010101010101);
	uint64_t h0 = pattern_value(0, 2) * UINT64_C(0x0001000100010001);
	uint64_t h1 = pattern_value(2, 2) * UINT64_C(0x0001000100010001);

	c->x[1] = DATA;
	arm64_test_exec(m, c, 0x0d40c020); /* LD1R {V0.8B}, [X1] */
	CHECK_V(c, 0, b, 0);
	arm64_test_exec(m, c, 0x4d40cc20); /* LD1R {V0.2D}, [X1] */
	CHECK_V(c, 0, pattern_value(0, 8), pattern_value(0, 8));
	arm64_test_exec(m, c, 0x0d60c420); /* LD2R {V0.4H, V1.4H}, [X1] */
	CHECK_V(c, 0, h0, 0);
	CHECK_V(c, 1, h1, 0);
	arm64_test_exec(m, c, 0x4d40e820); /* LD3R {V0.4S-V2.4S}, [X1] */
	for (unsigned i = 0; i < 3; i++) {
		uint64_t s = pattern_value(4 * i, 4) * UINT64_C(0x100000001);

		CHECK_V(c, i, s, s);
	}
	garbage(c);
	arm64_test_exec(m, c, 0x0d60ec20); /* LD4R {V0.1D-V3.1D}, [X1] */
	for (unsigned i = 0; i < 4; i++)
		CHECK_V(c, i, pattern_value(8 * i, 8), 0);
	CHECK_V(c, 4, 0xeeeeeeeeeeeeee04, 0xdddddddddddddd04);
	arm64_test_exec(m, c, 0x4ddfc020); /* LD1R {V0.16B}, [X1], #1 */
	CHECK_EQ(c->x[1], DATA + 1);
	CHECK_V(c, 0, b, b);
	c->x[1] = DATA + 4;
	c->x[2] = 0x20;
	arm64_test_exec(m, c, 0x4dc2c820); /* LD1R {V0.4S}, [X1], X2 */
	CHECK_EQ(c->x[1], DATA + 0x24);
	CHECK_V(c, 0, pattern_value(4, 4) * UINT64_C(0x100000001),
	        pattern_value(4, 4) * UINT64_C(0x100000001));
	machine_destroy(m);
}

static void test_structure_unallocated(void)
{
	static const uint32_t insns[] = {
		0x0c408c20, /* LD2 {V0.1D, V1.1D}: .1D is for LD1 and ST1 only */
		0x4c401020, /* multiple structures, opcode 0001 */
		0x4c405020, /* opcode 0101 */
		0x4c40c020, /* opcode 11xx */
		0x4c417020, /* LD1 without post-index, bit 16 set */
		0x4ce07020, /* post-indexed multiple structures, bit 21 set */
		0x0d419020, /* LD1 single without post-index, bit 16 set */
		0x0d40d020, /* LD1R with S = 1 */
		0x0d00c020, /* a store that replicates */
		0x0d404420, /* LD1 {V0.H}[], size<0> = 1 */
		0x0d408820, /* LD1 single, opcode 100, size = 10 */
		0x0d409420, /* LD1 {V0.D}[] with S = 1 */
		0x8c407020, /* op0 = 1x00 with V = 1 */
		0x7dc00020, /* LDR with size = 01, opc = 11 */
		0xbcc00020, /* LDUR with size = 10, opc = 11 */
		0x7d800020, /* STR with size = 01, opc = 10 */
		0x3c400820, /* LDTR has no SIMD&FP form */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

/* Run insn with Vd = V0 garbage, V1 = the pattern's first 16 bytes. */
static void move(Machine *m, Arm64Cpu *c, uint32_t insn)
{
	garbage(c);
	set_v(c, 1, pattern_value(0, 8), pattern_value(8, 8));
	arm64_test_exec(m, c, insn);
}

/* Registers other than V0, V1 and X1, each field with high and low bits. */
static void test_register_numbers(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	c->x[22] = DATA;
	c->x[27] = 2;
	arm64_test_exec(m, c, 0x3dc006d1); /* LDR Q17, [X22, #16] */
	CHECK_Q(c, 17, 16);
	arm64_test_exec(m, c, 0xfc7b7ad1); /* LDR D17, [X22, X27, LSL #3] */
	CHECK_V(c, 17, pattern_value(16, 8), 0);
	arm64_test_exec(m, c, 0xad5076d1); /* LDP Q17, Q29, [X22, #512] */
	CHECK_Q(c, 17, 512);
	CHECK_Q(c, 29, 528);
	arm64_test_exec(m, c, 0x4c4072d1); /* LD1 {V17.16B}, [X22] */
	CHECK_Q(c, 17, 0);
	arm64_test_exec(m, c, 0x4d4092d1); /* LD1 {V17.S}[3], [X22] */
	CHECK_EQ(c->v[17].d[1] >> 32, pattern_value(0, 4));
	CHECK_EQ(c->v[17].d[0], pattern_value(0, 8));
	c->x[27] = 0x20;
	arm64_test_exec(m, c, 0x4cdb8edd); /* LD2 {V29.2D, V30.2D}, [X22], X27 */
	CHECK_EQ(c->x[22], DATA + 0x20);
	CHECK_V(c, 29, pattern_value(0, 8), pattern_value(16, 8));
	CHECK_V(c, 30, pattern_value(8, 8), pattern_value(24, 8));
	arm64_test_exec(m, c, 0x4ddbcad1); /* LD1R {V17.4S}, [X22], X27 */
	CHECK_EQ(c->x[22], DATA + 0x40);
	CHECK_EQ(c->v[17].d[1], pattern_value(0x20, 4) * UINT64_C(0x100000001));
	arm64_test_exec(m, c, 0x6cff46dd); /* LDP D29, D17, [X22], #-16 */
	CHECK_EQ(c->x[22], DATA + 0x30);
	CHECK_V(c, 29, pattern_value(0x40, 8), 0);
	CHECK_V(c, 17, pattern_value(0x48, 8), 0);
	c->pc = RESET_PC;
	poke(m, RESET_PC + 16, 8, 0x0123456789abcdef);
	poke(m, RESET_PC + 24, 8, 0xfedcba9876543210);
	arm64_test_exec(m, c, 0x9c00009d); /* LDR Q29, . + 16 */
	CHECK_V(c, 29, 0x0123456789abcdef, 0xfedcba9876543210);

	c->x[22] = DATA + 0x100;
	arm64_test_exec(m, c, 0x3c8106dd); /* STR Q29, [X22], #16 */
	CHECK_EQ(c->x[22], DATA + 0x110);
	CHECK_EQ(peek(m, DATA + 0x108, 8), 0xfedcba9876543210);
	set_v(c, 17, 0x11111111, 0);
	set_v(c, 29, 0x22222222, 0);
	arm64_test_exec(m, c, 0x2d1076d1); /* STP S17, S29, [X22, #128] */
	CHECK_EQ(peek(m, DATA + 0x190, 8), 0x2222222211111111);
	set_v(c, 17, 1, 0x5555555566666666);
	arm64_test_exec(m, c, 0x4d9f86d1); /* ST1 {V17.D}[1], [X22], #8 */
	CHECK_EQ(c->x[22], DATA + 0x118);
	CHECK_EQ(peek(m, DATA + 0x110, 8), 0x5555555566666666);

	/* Moves */
	set_v(c, 22, 0x7766554433221100, 0xffeeddccbbaa9988);
	c->x[27] = 0x0123456789abcdef;
	arm64_test_exec(m, c, 0x4e0c06d1); /* DUP V17.4S, V22.S[1] */
	CHECK_V(c, 17, 0x7766554477665544, 0x7766554477665544);
	arm64_test_exec(m, c, 0x4e080f7d); /* DUP V29.2D, X27 */
	CHECK_V(c, 29, 0x0123456789abcdef, 0x0123456789abcdef);
	arm64_test_exec(m, c, 0x4e141f71); /* INS V17.S[2], W27 */
	CHECK_V(c, 17, 0x7766554477665544, 0x7766554489abcdef);
	arm64_test_exec(m, c, 0x6e0666d1); /* INS V17.H[1], V22.H[6] */
	CHECK_V(c, 17, 0x77665544ddcc5544, 0x7766554489abcdef);
	arm64_test_exec(m, c, 0x0e1c3edb); /* UMOV W27, V22.S[3] */
	CHECK_EQ(c->x[27], 0xffeeddcc);
	arm64_test_exec(m, c, 0x4e062edb); /* SMOV X27, V22.H[1] */
	CHECK_EQ(c->x[27], 0x3322);
	arm64_test_exec(m, c, 0x5e1306d1); /* MOV B17, V22.B[9] */
	CHECK_V(c, 17, 0x99, 0);
	c->x[27] = 0x0123456789abcdef;
	arm64_test_exec(m, c, 0x1e270371); /* FMOV S17, W27 */
	CHECK_V(c, 17, 0x89abcdef, 0);
	arm64_test_exec(m, c, 0x9eaf0371); /* FMOV V17.D[1], X27 */
	CHECK_V(c, 17, 0x89abcdef, 0x0123456789abcdef);
	arm64_test_exec(m, c, 0x9e6602db); /* FMOV X27, D22 */
	CHECK_EQ(c->x[27], 0x7766554433221100);
	arm64_test_exec(m, c, 0x9eae02db); /* FMOV X27, V22.D[1] */
	CHECK_EQ(c->x[27], 0xffeeddccbbaa9988);
	arm64_test_exec(m, c, 0x1e2042d1); /* FMOV S17, S22 */
	CHECK_V(c, 17, 0x33221100, 0);
	arm64_test_exec(m, c, 0x1e6e101d); /* FMOV D29, #1.0 */
	CHECK_V(c, 29, 0x3ff0000000000000, 0);
	arm64_test_exec(m, c, 0x6f00e431); /* MOVI V17.2D, #0xff */
	CHECK_V(c, 17, 0xff, 0xff);
	arm64_test_exec(m, c, 0x6f01d69d); /* MVNI V29.4S, #0x34, MSL #16 */
	CHECK_V(c, 29, 0xffcb0000ffcb0000, 0xffcb0000ffcb0000);
	arm64_test_exec(m, c, 0x4f013451); /* ORR V17.4S, #0x22, LSL #8 */
	CHECK_V(c, 17, 0x00002200000022ff, 0x00002200000022ff);
	machine_destroy(m);
}

/* More immediates: VFPExpandImm() with b = 0 and b = 1, h = 1. */
static void test_fp_immediates(void)
{
	static const struct {
		uint32_t insn;
		uint64_t lo, hi;
	} cases[] = {
		{0x1e201000, 0x40000000, 0},         /* FMOV S0, #2.0 */
		{0x1e2e3000, 0x3f880000, 0},         /* FMOV S0, #1.0625 */
		{0x1e311000, 0xc0400000, 0},         /* FMOV S0, #-3.0 */
		{0x1e6e1000, 0x3ff0000000000000, 0}, /* FMOV D0, #1.0 */
		{0x1e691000, 0x3fc8000000000000, 0}, /* FMOV D0, #0.1875 */
		{0x4f00f400, 0x4000000040000000, 0x4000000040000000},
		/* FMOV V0.4S, #2.0 */
		{0x6f07f620, 0xbff1000000000000, 0xbff1000000000000},
		/* FMOV V0.2D, #-1.0625 */
	};
	Arm64Cpu *c;
	Machine *m = machine(&c);

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		move(m, c, cases[i].insn);
		CHECK_V(c, 0, cases[i].lo, cases[i].hi);
	}
	machine_destroy(m);
}

/* Faults and their order, writebacks and the base SP. */
static void test_faults(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* With the MMU off, data is Device memory: no unaligned access. */
	c->x[1] = DATA + 8;
	check_abort(m, c, 0x3c810420, DFSC_ALIGNMENT); /* STR Q0, [X1], #16 */
	CHECK_EQ(c->x[1], DATA + 8);
	c->x[1] = DATA + 4;
	check_abort(m, c, 0xac810420, DFSC_ALIGNMENT); /* STP Q0, Q1, [X1], #32 */
	CHECK_EQ(c->x[1], DATA + 4);
	CHECK_EQ(peek(m, DATA + 8, 8), pattern_value(8, 8));
	c->x[1] = DATA + 2;
	check_abort(m, c, 0x2d400420, DFSC_ALIGNMENT); /* LDP S0, S1, [X1] */
	c->x[1] = DATA + 1;
	check_abort(m, c, 0xad400420, DFSC_ALIGNMENT); /* LDP Q0, Q1, [X1] */
	c->x[1] = DATA + 2;
	check_abort(m, c, 0x4d40c820, DFSC_ALIGNMENT); /* LD1R {V0.4S}, [X1] */
	check_abort(m, c, 0x0d409020, DFSC_ALIGNMENT); /* LD1 {V0.S}[1], [X1] */
	CHECK_V(c, 0, 0xeeeeeeeeeeeeee00, 0xdddddddddddddd00);
	/* A literal 8 bytes after the PC, not 16-byte aligned. */
	c->pc = RESET_PC;
	check_abort(m, c, 0x9c000040, DFSC_ALIGNMENT); /* LDR Q0, . + 8 */
	CHECK_V(c, 0, 0xeeeeeeeeeeeeee00, 0xdddddddddddddd00);

	/* LD1 of four registers writes four. */
	c->x[1] = DATA;
	c->x[0] = 0x40;                    /* not a post-index register: X1 stays */
	arm64_test_exec(m, c, 0x4c40283e); /* LD1 {V30.4S-V1.4S}, [X1] */
	CHECK_V(c, 2, 0xeeeeeeeeeeeeee02, 0xdddddddddddddd02);
	CHECK_EQ(c->x[1], DATA);
	arm64_test_exec(m, c, 0x0d409020); /* LD1 {V0.S}[1], [X1] */
	CHECK_EQ(c->x[1], DATA);

	/* SP as the base checks its alignment; other registers do not. */
	c->sys.sctlr[3] |= ARM64_SCTLR_SA;
	c->sp_el[3] = DATA + 8;
	c->x[1] = DATA + 16;
	arm64_test_exec(m, c, 0xad400420); /* LDP Q0, Q1, [X1] */
	arm64_test_exec(m, c, 0x0d409020); /* LD1 {V0.S}[1], [X1] */
	c->sys.esr[3] = 0;
	CHECK_EQ(arm64_test_step(m, c, 0xad4007e0), /* LDP Q0, Q1, [SP] */
	         MACHINE_STOP_LIMIT);
	CHECK_EQ(c->sys.esr[3] >> 26, EC_SP_ALIGNMENT);
	c->pc = RESET_PC;
	c->sys.esr[3] = 0;
	CHECK_EQ(arm64_test_step(m, c, 0x0d4093e0), /* LD1 {V0.S}[1], [SP] */
	         MACHINE_STOP_LIMIT);
	CHECK_EQ(c->sys.esr[3] >> 26, EC_SP_ALIGNMENT);
	c->pc = RESET_PC;
	c->sp_el[3] = DATA + 16;
	arm64_test_exec(m, c, 0xad4007e0);
	arm64_test_exec(m, c, 0x0d4093e0);
	CHECK_EQ(get_elem(c, 0, 1, 4), pattern_value(16, 4));
	machine_destroy(m);

	/* An unaligned word of Normal memory loads four bytes. */
	m = machine(&c);
	mmu_on(m, c);
	c->x[1] = DATA;
	arm64_test_exec(m, c, 0xbc401020); /* LDUR S0, [X1, #1] */
	CHECK_V(c, 0, pattern_value(1, 4), 0);
	machine_destroy(m);
}

static void test_copy(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	uint64_t ones = UINT64_C(0x0101010101010101);

	move(m, c, 0x4e1f0420); /* DUP V0.16B, V1.B[15] */
	CHECK_V(c, 0, pattern(15) * ones, pattern(15) * ones);
	move(m, c, 0x0e1e0420); /* DUP V0.4H, V1.H[7] */
	CHECK_V(c, 0, pattern_value(14, 2) * UINT64_C(0x0001000100010001), 0);
	move(m, c, 0x0e1c0420); /* DUP V0.2S, V1.S[3] */
	CHECK_V(c, 0, pattern_value(12, 4) * UINT64_C(0x100000001), 0);
	move(m, c, 0x4e180420); /* DUP V0.2D, V1.D[1] */
	CHECK_V(c, 0, pattern_value(8, 8), pattern_value(8, 8));

	c->x[1] = 0x123456789abcdef0;
	move(m, c, 0x0e010c20); /* DUP V0.8B, W1 */
	CHECK_V(c, 0, 0xf0 * ones, 0);
	move(m, c, 0x4e020c20); /* DUP V0.8H, W1 */
	CHECK_V(c, 0, 0xdef0def0def0def0, 0xdef0def0def0def0);
	move(m, c, 0x4e040c20); /* DUP V0.4S, W1 */
	CHECK_V(c, 0, 0x9abcdef09abcdef0, 0x9abcdef09abcdef0);
	move(m, c, 0x4e080c20); /* DUP V0.2D, X1 */
	CHECK_V(c, 0, 0x123456789abcdef0, 0x123456789abcdef0);

	/* INS writes one element and keeps the others. */
	move(m, c, 0x4e071c20); /* INS V0.B[3], W1 */
	CHECK_V(c, 0, 0xeeeeeeeef0eeee00, 0xdddddddddddddd00);
	move(m, c, 0x4e181c20); /* INS V0.D[1], X1 */
	CHECK_V(c, 0, 0xeeeeeeeeeeeeee00, 0x123456789abcdef0);
	move(m, c, 0x6e162420); /* INS V0.H[5], V1.H[2] */
	CHECK_V(c, 0, 0xeeeeeeeeeeeeee00,
	        0xdddddddd0000dd00 | pattern_value(4, 2) << 16);
	move(m, c, 0x6e046420); /* INS V0.S[0], V1.S[3] */
	CHECK_V(c, 0, 0xeeeeeeee00000000 | pattern_value(12, 4),
	        0xdddddddddddddd00);
	move(m, c, 0x6e084420); /* INS V0.D[0], V1.D[1] */
	CHECK_V(c, 0, pattern_value(8, 8), 0xdddddddddddddd00);

	/* UMOV and SMOV to W clear the upper half of X. */
	set_v(c, 1, 0x8899aabbccddeeff, 0x8070605040302010);
	c->x[0] = UINT64_MAX;
	arm64_test_exec(m, c, 0x0e133c20); /* UMOV W0, V1.B[9] */
	CHECK_EQ(c->x[0], 0x20);
	arm64_test_exec(m, c, 0x0e0e3c20); /* UMOV W0, V1.H[3] */
	CHECK_EQ(c->x[0], 0x8899);
	arm64_test_exec(m, c, 0x0e143c20); /* UMOV W0, V1.S[2] */
	CHECK_EQ(c->x[0], 0x40302010);
	arm64_test_exec(m, c, 0x4e183c20); /* UMOV X0, V1.D[1] */
	CHECK_EQ(c->x[0], 0x8070605040302010);
	arm64_test_exec(m, c, 0x0e032c20); /* SMOV W0, V1.B[1] */
	CHECK_EQ(c->x[0], 0xffffffee);
	arm64_test_exec(m, c, 0x4e032c20); /* SMOV X0, V1.B[1] */
	CHECK_EQ(c->x[0], 0xffffffffffffffee);
	arm64_test_exec(m, c, 0x4e122c20); /* SMOV X0, V1.H[4] */
	CHECK_EQ(c->x[0], 0x2010);
	arm64_test_exec(m, c, 0x4e1c2c20); /* SMOV X0, V1.S[3] */
	CHECK_EQ(c->x[0], 0xffffffff80706050);
	arm64_test_exec(m, c, 0x0e022c20); /* SMOV W0, V1.H[0] */
	CHECK_EQ(c->x[0], 0xffffeeff);

	/* DUP (element), scalar: MOV Bd, Vn.B[i] and the others. */
	move(m, c, 0x5e0b0420); /* MOV B0, V1.B[5] */
	CHECK_V(c, 0, pattern(5), 0);
	move(m, c, 0x5e0e0420); /* MOV H0, V1.H[3] */
	CHECK_V(c, 0, pattern_value(6, 2), 0);
	move(m, c, 0x5e0c0420); /* MOV S0, V1.S[1] */
	CHECK_V(c, 0, pattern_value(4, 4), 0);
	move(m, c, 0x5e180420); /* MOV D0, V1.D[1] */
	CHECK_V(c, 0, pattern_value(8, 8), 0);
	machine_destroy(m);
}

static void test_fmov(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* FMOV (general): the bits move unchanged. */
	c->x[1] = 0xffffffff12345678;
	move(m, c, 0x1e270020); /* FMOV S0, W1 */
	CHECK_V(c, 0, 0x12345678, 0);
	move(m, c, 0x9e670020); /* FMOV D0, X1 */
	CHECK_V(c, 0, 0xffffffff12345678, 0);
	move(m, c, 0x9eaf0020); /* FMOV V0.D[1], X1 */
	CHECK_V(c, 0, 0xeeeeeeeeeeeeee00, 0xffffffff12345678);
	c->x[0] = UINT64_MAX;
	set_v(c, 1, 0x8899aabbccddeeff, 0x0011223344556677);
	arm64_test_exec(m, c, 0x1e260020); /* FMOV W0, S1 */
	CHECK_EQ(c->x[0], 0xccddeeff);
	arm64_test_exec(m, c, 0x9e660020); /* FMOV X0, D1 */
	CHECK_EQ(c->x[0], 0x8899aabbccddeeff);
	arm64_test_exec(m, c, 0x9eae0020); /* FMOV X0, V1.D[1] */
	CHECK_EQ(c->x[0], 0x0011223344556677);

	/* FMOV (register) */
	move(m, c, 0x1e204020); /* FMOV S0, S1 */
	CHECK_V(c, 0, pattern_value(0, 4), 0);
	move(m, c, 0x1e604020); /* FMOV D0, D1 */
	CHECK_V(c, 0, pattern_value(0, 8), 0);

	/* FMOV (scalar, immediate): VFPExpandImm(). */
	move(m, c, 0x1e2e1000); /* FMOV S0, #1.0 */
	CHECK_V(c, 0, 0x3f800000, 0);
	move(m, c, 0x1e701000); /* FMOV D0, #-2.0 */
	CHECK_V(c, 0, 0xc000000000000000, 0);
	move(m, c, 0x1e281000); /* FMOV S0, #0.125 */
	CHECK_V(c, 0, 0x3e000000, 0);
	move(m, c, 0x1e67f000); /* FMOV D0, #31.0 */
	CHECK_V(c, 0, 0x403f000000000000, 0);
	machine_destroy(m);
}

static void test_modified_immediate(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	move(m, c, 0x4f056560); /* MOVI V0.4S, #0xab, LSL #24 */
	CHECK_V(c, 0, 0xab000000ab000000, 0xab000000ab000000);
	move(m, c, 0x0f050560); /* MOVI V0.2S, #0xab */
	CHECK_V(c, 0, 0x000000ab000000ab, 0);
	move(m, c, 0x4f06a5a0); /* MOVI V0.8H, #0xcd, LSL #8 */
	CHECK_V(c, 0, 0xcd00cd00cd00cd00, 0xcd00cd00cd00cd00);
	move(m, c, 0x0f0685a0); /* MOVI V0.4H, #0xcd */
	CHECK_V(c, 0, 0x00cd00cd00cd00cd, 0);
	move(m, c, 0x4f00c640); /* MOVI V0.4S, #0x12, MSL #8 */
	CHECK_V(c, 0, 0x000012ff000012ff, 0x000012ff000012ff);
	move(m, c, 0x0f00d640); /* MOVI V0.2S, #0x12, MSL #16 */
	CHECK_V(c, 0, 0x0012ffff0012ffff, 0);
	move(m, c, 0x4f02e740); /* MOVI V0.16B, #0x5a */
	CHECK_V(c, 0, 0x5a5a5a5a5a5a5a5a, 0x5a5a5a5a5a5a5a5a);
	move(m, c, 0x0f02e740); /* MOVI V0.8B, #0x5a */
	CHECK_V(c, 0, 0x5a5a5a5a5a5a5a5a, 0);
	move(m, c, 0x2f05e540); /* MOVI D0, #0xff00ff00ff00ff00 */
	CHECK_V(c, 0, 0xff00ff00ff00ff00, 0);
	move(m, c, 0x6f02e6a0); /* MOVI V0.2D, #0x00ff00ff00ff00ff */
	CHECK_V(c, 0, 0x00ff00ff00ff00ff, 0x00ff00ff00ff00ff);

	move(m, c, 0x6f054560); /* MVNI V0.4S, #0xab, LSL #16 */
	CHECK_V(c, 0, 0xff54ffffff54ffff, 0xff54ffffff54ffff);
	move(m, c, 0x6f008640); /* MVNI V0.8H, #0x12 */
	CHECK_V(c, 0, 0xffedffedffedffed, 0xffedffedffedffed);
	move(m, c, 0x2f01c680); /* MVNI V0.2S, #0x34, MSL #8 */
	CHECK_V(c, 0, 0xffffcb00ffffcb00, 0);

	/* ORR and BIC change Vd; the 64-bit forms clear its upper half. */
	move(m, c, 0x4f003620); /* ORR V0.4S, #0x11, LSL #8 */
	CHECK_V(c, 0, 0xeeeeffeeeeeeff00, 0xdddddddddddddd00);
	move(m, c, 0x0f01b440); /* ORR V0.4H, #0x22, LSL #8 */
	CHECK_V(c, 0, 0xeeeeeeeeeeeeee00, 0);
	move(m, c, 0x6f0797e0); /* BIC V0.8H, #0xff */
	CHECK_V(c, 0, 0xee00ee00ee00ee00, 0xdd00dd00dd00dd00);
	move(m, c, 0x2f047400); /* BIC V0.2S, #0x80, LSL #24 */
	CHECK_V(c, 0, 0x6eeeeeee6eeeee00, 0);

	move(m, c, 0x4f03f600); /* FMOV V0.4S, #1.0 */
	CHECK_V(c, 0, 0x3f8000003f800000, 0x3f8000003f800000);
	move(m, c, 0x0f07f400); /* FMOV V0.2S, #-0.5 */
	CHECK_V(c, 0, 0xbf000000bf000000, 0);
	move(m, c, 0x6f00f400); /* FMOV V0.2D, #2.0 */
	CHECK_V(c, 0, 0x4000000000000000, 0x4000000000000000);
	machine_destroy(m);
}

static void test_move_unallocated(void)
{
	static const uint32_t insns[] = {
		0x4e100420, /* DUP (element) with imm5 = 10000 */
		0x0e080420, /* DUP V0.1D */
		0x2e162420, /* INS (element) with Q = 0 */
		0x4e013c20, /* UMOV X0 from a byte */
		0x0e042c20, /* SMOV W0 from a word */
		0x4e011420, /* copy, Q = 1, imm4 = 0010 */
		0x4e014420, /* copy, imm4 = 1000 */
		0x0e012420, /* copy, Q = 0, imm4 = 0100 */
		0x7e010420, /* scalar copy with op = 1 */
		0x5e010c20, /* scalar copy with imm4 = 0001 */
		0x5e100420, /* scalar DUP with imm5 = 10000 */
		0x4f000c00, /* modified immediate, o2 = 1 */
		0x6f000c00, /* modified immediate, op = 1, o2 = 1 */
		0x2f00f400, /* FMOV (vector, immediate) of D with Q = 0 */
		0x9e260020, /* FMOV X0, S1 */
		0x1e660020, /* FMOV W0, D1 */
		0x9ea60020, /* FMOV (general) with ftype = 10, rmode = 00 */
		0x1eae0020, /* FMOV W0, V1.D[1] */
		0x3e260020, /* FMOV (general) with S = 1 */
		0x1ea04020, /* FMOV (register) with ftype = 10 */
		0x9e204020, /* FMOV (register) with M = 1 */
		0x3e204020, /* FMOV (register) with S = 1 */
		0x1e2e1020, /* FMOV (scalar, immediate) with imm5 != 0 */
		0x1eae1000, /* FMOV (scalar, immediate) with ftype = 10 */
		0x9e2e1000, /* FMOV (scalar, immediate) with M = 1 */
		0xee000000, /* op0 = 1110 */
		0x8e000000, /* op0 = 10x0 */
		0xde000000, /* op0 = 11x1 */
		0xde010420, /* op0 = 11x1, as a scalar copy would be */
		0x4e000420, /* DUP (element) with imm5 = 00000 */
		0x5e000420, /* scalar DUP with imm5 = 00000 */
		0x6e000420, /* INS (element) with imm5 = 00000 */
		0x0e071c20, /* INS (general) with Q = 0 */
		0x4e101c20, /* INS (general) with imm5 = 10000 */
		0x4e082c20, /* SMOV X0 from a doubleword */
		0x0e083c20, /* UMOV W0 from a doubleword */
		0x0e103c20, /* UMOV W0 with imm5 = 10000 */
		0x1e360020, /* FMOV-like, rmode = 10 (FEAT_FPRCVT) */
		0x9ebe0020, /* FMOV X0, V1.D[1] with rmode = 11 */
		0x1ea20020, /* SCVTF with ftype = 10 */
		0x3e02f420, /* SCVTF (fixed-point) with S = 1 */
		0x1e82f420, /* SCVTF (fixed-point) with ftype = 10 */
		0x9e622820, /* FADD with M = 1 */
		0x3e622820, /* FADD with S = 1 */
		0x1ea22820, /* FADD with ftype = 10 */
		0x1e208020, /* scalar FP, op3 = 100000 */
		0x1ea12000, /* FCMP with ftype = 10 */
		0x1ea20c20, /* FCSEL with ftype = 10 */
		0x1f820c20, /* FMADD with ftype = 10 */
		0x1ea10400, /* FCCMP with ftype = 10 */
		0x1ea0c020, /* FABS with ftype = 10 */
		0x1ee34020, /* FP 1 source, half precision, opcode 000110 */
		0x1e304020, /* FP 1 source, opcode 100000 */
		0x5e410420, /* scalar, op2 = 1000, as a scalar copy would be */
		0x5f800420, /* scalar, op1 = 11, op3[0] = 1 */
		0x4f800420, /* vector, op1 = 11, op3[0] = 1 */
		0x5ee2b820, /* scalar two-register, op3[7] = 1 */
		0x4ea2b820, /* vector two-register, op3[7] = 1 */
		0x5e380820, /* scalar, op2 = 0111 */
		0x4e380820, /* vector, op2 = 0111 */
		0x4e028020, /* vector, op2 = x0xx, op3 = xxx1xxxx0 */
		0x5e828420, /* scalar three same extra with U = 0 */
		0x4c607020, /* LD1 (multiple) without post-index, bit 21 set */
		0x0d509020, /* LD1 (single) without post-index, bit 20 set */
		0x1e2e1200, /* FMOV (scalar, immediate) with imm5 = 10000 */
		0x4ea4b820, /* vector two-register, op3[8] = 1 */
		0x5ee4b820, /* scalar two-register, op3[8] = 1 */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

/* What this core lacks is UNDEFINED: crypto, FEAT_FP16 and newer. */
static void test_features_undefined(void)
{
	static const uint32_t insns[] = {
		0x4e284820, /* AESE V0.16B, V1.16B */
		0x5e020020, /* SHA1C Q0, S1, V2.4S */
		0x5e280820, /* SHA1H S0, S1 */
		0x5e282820, /* SHA256SU0 V0.4S, V1.4S */
		0x1ee22820, /* FADD H0, H1, H2 */
		0x4e421420, /* FADD V0.8H, V1.8H, V2.8H */
		0x4ef8f820, /* FABS V0.8H, V1.8H */
		0x1ee70020, /* FMOV H0, W1 */
		0x1eee1000, /* FMOV H0, #1.0 */
		0x4f03fe00, /* FMOV V0.8H, #1.0 */
		0x6e828420, /* SQRDMLAH V0.4S, V1.4S, V2.4S (FEAT_RDM) */
		0x7e828420, /* SQRDMLAH S0, S1, S2 */
		0x4e829420, /* SDOT V0.4S, V1.16B, V2.16B (FEAT_DotProd) */
		0x1e7e0020, /* FJCVTZS W0, D1 (FEAT_JSCVT) */
		0xce020c20, /* EOR3 V0.16B, V1.16B, V2.16B, V3.16B (FEAT_SHA3) */
		0xce820c20, /* XAR V0.2D, V1.2D, V2.2D, #3 */
		0xce628020, /* SHA512H Q0, Q1, V2.2D (FEAT_SHA512) */
		0xce428020, /* SM3TT1A V0.4S, V1.4S, V2.S[0] (FEAT_SM3) */
		0x1ee0c020, /* FABS H0, H1 */
		0x4e420420, /* FMAXNM V0.8H, V1.8H, V2.8H */
		0x4e481420, /* FADD V0.8H, V1.8H, V8.8H */
		0x1ee20020, /* SCVTF H0, W1 */
		0x1ec2f420, /* SCVTF H0, W1, #3 */
		0x5e0a0020, /* SHA1C Q0, S1, V10.4S */
		0x7ec81420, /* FABD H0, H1, H8 */
		0x5e422420, /* FCMEQ H0, H1, H2 */
		0x7ec21420, /* FABD H0, H1, H2 */
	};

	arm64_test_check_undefined(insns, ARRAY_SIZE(insns));
}

/* The classes not implemented yet stop the machine and are named. */
static void test_not_implemented_yet(void)
{
	static const struct {
		uint32_t insn;
		const char *name;
	} cases[] = {
		{0x1e622820, "Floating-point data-processing (2 source)"},
		{0x4e22d420, "Advanced SIMD three same"}, /* FADD */
		{0x1e620020, "Conversion between floating-point and integer"},
		/* SCVTF D0, W1 */
		{0x6e30f820, "Advanced SIMD across lanes"}, /* FMAXV */
		{0x4ea0f820, "Advanced SIMD two-register miscellaneous"},
		/* FABS V0.4S, V1.4S */
		{0x4f3de420, "Advanced SIMD shift by immediate"},
		/* SCVTF V0.4S, V1.4S, #3 */
		{0x4fa21020, "Advanced SIMD vector x indexed element"}, /* FMLA */
		{0x7ea2d420, "Advanced SIMD scalar three same"},        /* FABD */
		{0x7e30d820, "Advanced SIMD scalar pairwise"},          /* FADDP */
		{0x5ea1d820, "Advanced SIMD scalar two-register miscellaneous"},
		/* FRECPE S0, S1 */
		{0x5f3de420, "Advanced SIMD scalar shift by immediate"},
		/* SCVTF S0, S1, #3 */
		{0x5fa21020, "Advanced SIMD scalar x indexed element"}, /* FMLA */
		{0x1e212000, "Floating-point compare"},
		{0x1e210400, "Floating-point conditional compare"},
		{0x1e220c20, "Floating-point conditional select"},
		{0x1f020c20, "Floating-point data-processing (3 source)"},
		{0x1e220020, "Conversion between floating-point and integer"},
		{0x1e02f420, "Conversion between floating-point and fixed-point"},
		{0x1e20c020, "Floating-point data-processing (1 source)"},
		{0x1ee24020, "Floating-point data-processing (1 source)"},
		/* FCVT S0, H1 */
		{0x1ee2c020, "Floating-point data-processing (1 source)"},
		/* FCVT D0, H1 */
		{0x1e218400, "Floating-point conditional compare"}, /* HI */
		{0x1e214400, "Floating-point conditional compare"}, /* MI */
		{0x1e211400, "Floating-point conditional compare"}, /* NE */
		{0x1e212400, "Floating-point conditional compare"}, /* CS */
		{0x1e227c20, "Floating-point conditional select"},  /* VC */
		{0x1f024020, "Floating-point data-processing (3 source)"},
		/* FMADD S0, S1, S2, S16 */
		{0x1f021020, "Floating-point data-processing (3 source)"},
		/* FMADD S0, S1, S2, S4 */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Arm64Cpu *c;
		Machine *m = machine(&c);
		char expected[128];

		snprintf(expected, sizeof(expected), "is not implemented yet (%s)",
		         cases[i].name);
		CHECK_EQ(arm64_test_step(m, c, cases[i].insn), MACHINE_STOP_ERROR);
		if (strstr(m->stop_message, expected) == NULL) {
			fprintf(stderr, "0x%08" PRIx32 ": %s\n", cases[i].insn,
			        m->stop_message);
			test_failures++;
		}
		machine_destroy(m);
	}
}

int main(void)
{
	RUN_TEST(test_reset_and_registers);
	RUN_TEST(test_traps);
	RUN_TEST(test_register_forms);
	RUN_TEST(test_pairs);
	RUN_TEST(test_alignment);
	RUN_TEST(test_multiple_structures);
	RUN_TEST(test_single_structure);
	RUN_TEST(test_replicate);
	RUN_TEST(test_structure_unallocated);
	RUN_TEST(test_copy);
	RUN_TEST(test_fmov);
	RUN_TEST(test_modified_immediate);
	RUN_TEST(test_move_unallocated);
	RUN_TEST(test_features_undefined);
	RUN_TEST(test_not_implemented_yet);
	RUN_TEST(test_register_numbers);
	RUN_TEST(test_fp_immediates);
	RUN_TEST(test_faults);
	return test_summary();
}
