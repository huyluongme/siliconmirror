/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Branches, Exception Generating and System instructions: the
 * instructions of C4.1 with bits 28:26 set to 101. Each one follows its
 * description in C6.2.
 *
 * Implemented so far: the branches, the hints, the DMB, DSB and ISB
 * barriers, the System register moves and PSTATE moves, which are in
 * sysreg.c, and the exception generating instructions and ERET, which
 * are in exception.c. The System instructions and CLREX are not
 * implemented yet. Classes that only hold instructions newer than
 * Armv8.0 are UNDEFINED.
 *
 * A branch target is used as it is. AArch64_BranchAddr() would remove a
 * tag from its top byte when TCR_ELx.TBI says so, but the MMU, which
 * TCR_ELx belongs to, is not implemented yet.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

/* B and BL, C6.2.35 and C6.2.43. */
static Arm64Exec branch_imm(Arm64Cpu *c, uint32_t insn)
{
	uint64_t offset = sign_extend64(field32(insn, 25, 0) << 2, 28);

	if (bit32(insn, 31)) /* BL */
		set_x(c, 30, c->pc + 4);
	c->next_pc = c->pc + offset;
	return ARM64_EXEC_OK;
}

/* CBZ and CBNZ, C6.2.78 and C6.2.77. */
static Arm64Exec compare_branch(Arm64Cpu *c, uint32_t insn)
{
	unsigned datasize = 32 << bit32(insn, 31);
	bool nonzero = bit32(insn, 24);
	uint64_t offset = sign_extend64(field32(insn, 23, 5) << 2, 21);
	uint64_t operand = get_x(c, field32(insn, 4, 0)) & datasize_mask(datasize);

	if ((operand != 0) == nonzero)
		c->next_pc = c->pc + offset;
	return ARM64_EXEC_OK;
}

/* TBZ and TBNZ, C6.2.479 and C6.2.478. */
static Arm64Exec test_branch(Arm64Cpu *c, uint32_t insn)
{
	unsigned bit_pos = bit32(insn, 31) << 5 | field32(insn, 23, 19);
	bool nonzero = bit32(insn, 24);
	uint64_t offset = sign_extend64(field32(insn, 18, 5) << 2, 16);
	uint64_t operand = get_x(c, field32(insn, 4, 0));

	if ((bool)((operand >> bit_pos) & 1) == nonzero)
		c->next_pc = c->pc + offset;
	return ARM64_EXEC_OK;
}

/* B.cond, C6.2.34. */
static Arm64Exec conditional_branch(Arm64Cpu *c, uint32_t insn)
{
	uint64_t offset = sign_extend64(field32(insn, 23, 5) << 2, 21);

	if (bit32(insn, 4)) /* BC.cond: FEAT_HBC */
		return ARM64_EXEC_UNDEFINED;
	if (condition_holds(c, field32(insn, 3, 0)))
		c->next_pc = c->pc + offset;
	return ARM64_EXEC_OK;
}

/* BR, BLR and RET, C6.2.46, C6.2.44 and C6.2.338. */
static Arm64Exec branch_register(Arm64Cpu *c, uint32_t insn)
{
	unsigned opc = field32(insn, 24, 21);
	unsigned n = field32(insn, 9, 5);

	/*
	 * Every Armv8.0 form has op2 = 11111, op3 = 000000 and op4 = 00000.
	 * The other encodings need FEAT_PAuth or are UNALLOCATED.
	 */
	if (field32(insn, 20, 16) != 0x1f || field32(insn, 15, 10) != 0 ||
	    field32(insn, 4, 0) != 0)
		return ARM64_EXEC_UNDEFINED;

	switch (opc) {
	case 0: /* BR */
	case 2: /* RET */
		c->next_pc = get_x(c, n);
		return ARM64_EXEC_OK;
	case 1: { /* BLR: read Xn before writing X30, which may be Xn */
		uint64_t target = get_x(c, n);

		set_x(c, 30, c->pc + 4);
		c->next_pc = target;
		return ARM64_EXEC_OK;
	}
	case 4: /* ERET */
		return n == 31 ? arm64_exec_eret(c) : ARM64_EXEC_UNDEFINED;
	default:
		/* DRPS is UNDEFINED outside Debug state; the rest UNALLOCATED. */
		return ARM64_EXEC_UNDEFINED;
	}
}

/*
 * Hints, C6.2.172. Encodings with no instruction, and instructions of
 * features that this model does not have (PACIASP, BTI, ESB and others),
 * behave as NOP.
 */
static Arm64Exec hint(Arm64Cpu *c, uint32_t insn)
{
	switch (field32(insn, 11, 5)) { /* CRm:op2 */
	case 0x02: {                    /* WFE, C6.2.502: Hint_WFE() */
		Arm64Exec trap;

		if (c->event_register) {
			c->event_register = false;
			return ARM64_EXEC_OK;
		}
		trap = arm64_check_wfx_trap(c, true);
		if (trap != ARM64_EXEC_OK)
			return trap;
		c->wait = ARM64_WAIT_EVENT;
		c->cpu.state = CPU_HALTED;
		return ARM64_EXEC_OK;
	}
	case 0x03: { /* WFI, C6.2.504: Hint_WFI() */
		Arm64Exec trap;

		if (arm64_interrupt_pending(c))
			return ARM64_EXEC_OK;
		trap = arm64_check_wfx_trap(c, false);
		if (trap != ARM64_EXEC_OK)
			return trap;
		c->wait = ARM64_WAIT_INTERRUPT;
		c->cpu.state = CPU_HALTED;
		return ARM64_EXEC_OK;
	}
	case 0x04: /* SEV, C6.2.367 */
		arm64_send_event(c);
		return ARM64_EXEC_OK;
	case 0x05: /* SEVL, C6.2.368 */
		c->event_register = true;
		return ARM64_EXEC_OK;
	default: /* NOP, YIELD, CSDB and the rest */
		return ARM64_EXEC_OK;
	}
}

/*
 * Barriers. DMB, DSB and ISB, C6.2.150, C6.2.152 and C6.2.178, have
 * nothing to wait for here: each instruction completes, memory accesses
 * included, before the next one starts.
 */
static Arm64Exec barrier(uint32_t insn)
{
	if (field32(insn, 4, 0) != 0x1f) /* Rt != 11111: UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;

	switch (field32(insn, 7, 5)) { /* op2 */
	case 2:                        /* CLREX */
		return ARM64_EXEC_UNIMPLEMENTED;
	case 4: /* DSB */
	case 5: /* DMB */
	case 6: /* ISB */
		return ARM64_EXEC_OK;
	default:
		/* SB (FEAT_SB), DSB nXS (FEAT_XS), or UNALLOCATED. */
		return ARM64_EXEC_UNDEFINED;
	}
}

/*
 * The System instruction classes: bits 31:22 = 1101010100. Bit 21 is L,
 * bits 20:19 op0, bits 18:16 op1 and bits 15:12 CRn of the System
 * instruction encoding.
 */
static Arm64Exec system_space(Arm64Cpu *c, uint32_t insn)
{
	unsigned l = bit32(insn, 21);
	unsigned op0 = field32(insn, 20, 19);
	unsigned op1 = field32(insn, 18, 16);
	unsigned crn = field32(insn, 15, 12);

	if (op0 == 1) { /* SYS, SYSL: cache, TLB and address translation */
		if (arm64_tidcp_traps(c, insn))
			return arm64_sys_trap(c, 2, insn);
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	if (op0 >= 2) /* MRS, MSR (register) */
		return arm64_exec_sysreg(c, insn);
	if (l == 0 && crn == 4) /* PSTATE: MSR (immediate) */
		return arm64_exec_msr_imm(c, insn);
	if (l == 0 && op1 == 3 && crn == 2 && field32(insn, 4, 0) == 0x1f)
		return hint(c, insn);
	if (l == 0 && op1 == 3 && crn == 3)
		return barrier(insn);
	/*
	 * The rest is HINTE (FEAT_HINTE), WFET and WFIT (FEAT_WFxT), or
	 * UNALLOCATED.
	 */
	return ARM64_EXEC_UNDEFINED;
}

Arm64Exec arm64_exec_branch_sys(Arm64Cpu *c, uint32_t insn)
{
	unsigned op0 = field32(insn, 31, 29);

	switch (op0 & 3) {
	case 0: /* x00 */
		return branch_imm(c, insn);
	case 1: /* x01 */
		return bit32(insn, 25) ? test_branch(c, insn) : compare_branch(c, insn);
	case 3: /* x11: compare and branch forms of FEAT_CMPBR, or UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;
	default:
		break;
	}

	if (op0 == 2) {
		/*
		 * Bits 25:24 = 01 is Miscellaneous branch (immediate), only
		 * FEAT_PAuth_LR; bit 25 set is UNALLOCATED.
		 */
		if (field32(insn, 25, 24) != 0)
			return ARM64_EXEC_UNDEFINED;
		return conditional_branch(c, insn);
	}

	/* op0 = 110 */
	if (bit32(insn, 25))
		return branch_register(c, insn);
	if (!bit32(insn, 24)) /* SVC, HVC, SMC, BRK, HLT, DCPS */
		return arm64_exec_exception_gen(c, insn);
	/*
	 * Bits 25:22 = 0100 is the System instruction space. 0101 holds the
	 * 128-bit System instructions (FEAT_SYSINSTR128, FEAT_SYSREG128);
	 * 011x is UNALLOCATED.
	 */
	if (field32(insn, 25, 22) != 4)
		return ARM64_EXEC_UNDEFINED;
	return system_space(c, insn);
}
