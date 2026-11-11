/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Exceptions of the AArch64 model: taking an exception (D1.4.2,
 * AArch64_TakeException() in J1.2.2.38), synchronous or an interrupt that
 * interrupt.c decided to take, the exception return (D1.4.4,
 * AArch64_ExceptionReturn() in J1.2.3.36), the instructions that generate
 * exceptions, and the faults and traps that the CPU checks itself.
 *
 * The model runs in AArch64 state only, so exceptions come from AArch64
 * state and go to it. An exception return to AArch32 state that the
 * architecture allows stops the machine: AArch32 is not implemented.
 * Software step needs MDSCR_EL1, which is not implemented, so PSTATE.SS
 * is always 0.
 */

#include <stdio.h>

#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/bits.h"

Arm64Exec arm64_raise(Arm64Cpu *c, unsigned target_el, unsigned ec,
                      uint32_t iss)
{
	c->exception.target_el = target_el;
	c->exception.esr = arm64_esr(ec, iss);
	c->exception.return_address = c->pc;
	c->exception.far_valid = false;
	return ARM64_EXEC_EXCEPTION;
}

/*
 * The level for an exception that goes to EL1 unless the PE is above it:
 * from EL2 or EL3 to the current level, and to EL2 instead of EL1 when
 * HCR_EL2.TGE routes it there. TGE routes the exceptions of
 * AArch64_Undefined() and of the calls from EL0 only, and the aborts of
 * AArch64_DataAbort() and the alignment faults from EL1 too.
 */
static unsigned route(const Arm64Cpu *c, bool tge_at_el1)
{
	if (c->el > 1)
		return c->el;
	if ((c->el == 0 || tge_at_el1) && arm64_el2_enabled(c) &&
	    (c->sys.hcr_el2 & ARM64_HCR_TGE))
		return 2;
	return 1;
}

unsigned arm64_el1_target(const Arm64Cpu *c)
{
	return route(c, false);
}

/* HaveAArch32EL(): whether ID_AA64PFR0_EL1 says that el has AArch32. */
static bool have_aarch32_el(Arm64Cpu *c, unsigned el)
{
	uint64_t pfr0 = 0;

	if (el > c->config.highest_el)
		return false;
	arm64_cpu_read_sysreg(c, ARM64_ID_AA64PFR0_EL1, &pfr0);
	return ((pfr0 >> (4 * el)) & 0xf) == 2; /* AArch64 and AArch32 */
}

/*
 * ELUsingAArch32K(), from ELStateUsingAArch32K() in J1.4.3.376: whether
 * el uses AArch32, as the RW bits of SCR_EL3 and HCR_EL2 make it. Returns
 * false when it is not known: for EL0, seen from above while EL1 uses
 * AArch64.
 */
static bool el_using_aarch32_k(Arm64Cpu *c, unsigned el, bool *aarch32)
{
	bool secure = c->config.highest_el == 3 && !(c->sys.scr_el3 & ARM64_SCR_NS);

	*aarch32 = false;
	if (!have_aarch32_el(c, el) || el == 3)
		return true;
	if (c->config.highest_el == 3 && !(c->sys.scr_el3 & ARM64_SCR_RW)) {
		*aarch32 = true; /* everything below EL3 */
		return true;
	}
	if (el == 2)
		return true;
	if (c->config.highest_el >= 2 && !(c->sys.hcr_el2 & ARM64_HCR_RW) &&
	    !secure) {
		*aarch32 = true; /* everything below EL2 */
		return true;
	}
	if (el == 1)
		return true;
	return c->el == 0; /* at EL0, PSTATE.nRW: AArch64 */
}

/* ELUsingAArch32(), for an Exception level whose state is known. */
static bool el_using_aarch32(Arm64Cpu *c, unsigned el)
{
	bool aarch32;

	return el_using_aarch32_k(c, el, &aarch32) && aarch32;
}

/* GetPSRFromPSTATE() for AArch64 state, J1.4.3.452: SPSR_ELx. */
static uint64_t pstate_to_spsr(const Arm64Cpu *c)
{
	return (uint64_t)c->nzcv | (c->il ? UINT32_C(1) << 20 : 0) | c->daif |
	       (uint64_t)c->el << 2 | (c->sp_sel ? 1 : 0);
}

/*
 * AArch64_TakeException() from AArch64 state to target, with the
 * offset of the exception type in the vector table (D1.4.1.6) and the
 * preferred return address. An IRQ or FIQ leaves ESR_ELx and FAR_ELx
 * alone: AArch64_ReportException() is not called for them.
 */
static void take(Arm64Cpu *c, unsigned target, uint64_t offset,
                 uint64_t return_address, bool report)
{
	if (target > c->el) {
		bool lower_32;

		if (target == 3)
			lower_32 = el_using_aarch32(c, arm64_el2_enabled(c) ? 2 : 1);
		else
			lower_32 = el_using_aarch32(c, target - 1);
		offset += lower_32 ? 0x600 : 0x400;
	} else if (c->sp_sel) {
		offset += 0x200;
	}

	/*
	 * AArch64_ReportException(), J1.2.2.28. FAR_ELx is ARBITRARY for the
	 * exceptions without an address: it keeps its value.
	 */
	if (report) {
		c->sys.esr[target] = c->exception.esr;
		if (c->exception.far_valid)
			c->sys.far[target] = c->exception.far;
	}
	c->sys.spsr[target] = pstate_to_spsr(c);
	c->sys.elr[target] = return_address;

	c->el = target;
	c->sp_sel = true;
	c->daif = ARM64_DAIF_D | ARM64_DAIF_A | ARM64_DAIF_I | ARM64_DAIF_F;
	c->il = false;
	c->pc = (c->sys.vbar[target] & ~(uint64_t)0x7ff) + offset;
}

void arm64_take_exception(Arm64Cpu *c)
{
	take(c, c->exception.target_el, 0, c->exception.return_address, true);
}

void arm64_take_interrupt(Arm64Cpu *c, unsigned target_el, bool fiq)
{
	take(c, target_el, fiq ? 0x100 : 0x80, c->pc, false);
}

/*
 * SVC, HVC and SMC (C6.2.466, C6.2.175, C6.2.372) return to the next
 * instruction: AArch64_CallSupervisor(), AArch64_CallHypervisor() and
 * AArch64_CallSecureMonitor(), J1.2.2.35 to 37.
 */
static Arm64Exec call(Arm64Cpu *c, unsigned target_el, unsigned ec,
                      uint32_t imm16)
{
	arm64_raise(c, target_el, ec, imm16);
	c->exception.return_address = c->pc + 4;
	return ARM64_EXEC_EXCEPTION;
}

static Arm64Exec hvc(Arm64Cpu *c, uint32_t imm16)
{
	unsigned highest = c->config.highest_el;

	if (highest < 2 || c->el == 0 || (c->el == 1 && !arm64_el2_enabled(c)))
		return ARM64_EXEC_UNDEFINED;
	if (highest == 2 && (c->sys.hcr_el2 & ARM64_HCR_HCD))
		return ARM64_EXEC_UNDEFINED;
	if (highest == 3 && !(c->sys.scr_el3 & ARM64_SCR_HCE))
		return ARM64_EXEC_UNDEFINED;
	return call(c, c->el == 3 ? 3 : 2, ARM64_EC_HVC, imm16);
}

/*
 * SMC: AArch64_CheckForSMCUndefOrTrap(), J1.2.2.48. Without EL3, whether
 * HCR_EL2.TSC traps SMC from EL1 is IMPLEMENTATION DEFINED; this model
 * makes SMC UNDEFINED there instead.
 */
static Arm64Exec smc(Arm64Cpu *c, uint32_t imm16)
{
	bool tsc =
		c->el == 1 && arm64_el2_enabled(c) && (c->sys.hcr_el2 & ARM64_HCR_TSC);

	if (c->el == 0 || c->config.highest_el < 3)
		return ARM64_EXEC_UNDEFINED;
	if (tsc) /* trapped: returns to the SMC */
		return arm64_raise(c, 2, ARM64_EC_SMC, imm16);
	if (c->sys.scr_el3 & ARM64_SCR_SMD)
		return ARM64_EXEC_UNDEFINED;
	return call(c, 3, ARM64_EC_SMC, imm16);
}

/*
 * BRK, C6.2.49: AArch64_SoftwareBreakpoint(), J1.2.2.23. Returns to the
 * BRK. HCR_EL2.TGE and MDCR_EL2.TDE send it to EL2 from EL0 and EL1.
 */
static Arm64Exec brk(Arm64Cpu *c, uint32_t imm16)
{
	unsigned target = c->el > 1 ? c->el : 1;

	if (c->el <= 1 && arm64_el2_enabled(c) &&
	    ((c->sys.hcr_el2 & ARM64_HCR_TGE) ||
	     (c->sys.mdcr_el2 & ARM64_MDCR_TDE)))
		target = 2;
	return arm64_raise(c, target, ARM64_EC_BRK, imm16);
}

/* The Exception generation class of C4.1: bits 31:24 = 11010100. */
Arm64Exec arm64_exec_exception_gen(Arm64Cpu *c, uint32_t insn)
{
	unsigned opc = field32(insn, 23, 21);
	unsigned ll = field32(insn, 1, 0);
	uint32_t imm16 = field32(insn, 20, 5);

	if (field32(insn, 4, 2) != 0) /* op2 */
		return ARM64_EXEC_UNDEFINED;
	switch (opc << 2 | ll) {
	case 0x01:
		return call(c, arm64_el1_target(c), ARM64_EC_SVC, imm16);
	case 0x02:
		return hvc(c, imm16);
	case 0x03:
		return smc(c, imm16);
	case 0x04:
		return brk(c, imm16);
	default:
		/*
		 * HLT (C6.2.174) needs halting enabled by an external debugger,
		 * and DCPS1 to DCPS3 (C6.2.146 to 148) need Debug state: neither
		 * exists here, so they are UNDEFINED, like the UNALLOCATED
		 * encodings.
		 */
		return ARM64_EXEC_UNDEFINED;
	}
}

/* ELFromSPSR(), J1.4.3.373, with ELFromM32() and BadMode() for AArch32. */
static bool el_from_spsr(Arm64Cpu *c, uint64_t spsr, unsigned *el)
{
	bool valid;

	if (!(spsr & 0x10)) { /* AArch64 */
		*el = (spsr >> 2) & 3;
		valid = *el <= c->config.highest_el && !(spsr & 2) &&
		        !(*el == 0 && (spsr & 1));
	} else {
		switch (spsr & 0x1f) {
		case 0x16: /* Monitor */
			*el = 3;
			break;
		case 0x1a: /* Hyp */
			*el = 2;
			break;
		case 0x11: /* FIQ */
		case 0x12: /* IRQ */
		case 0x13: /* Supervisor */
		case 0x17: /* Abort */
		case 0x1b: /* Undefined */
		case 0x1f: /* System: EL1 modes, as EL3 uses AArch64 */
			*el = 1;
			break;
		case 0x10: /* User */
			*el = 0;
			break;
		default:
			return false;
		}
		valid = have_aarch32_el(c, *el);
	}
	/* There is no Secure EL2. */
	if (*el == 2 && c->config.highest_el == 3 &&
	    !(c->sys.scr_el3 & ARM64_SCR_NS))
		valid = false;
	return valid;
}

/* IllegalExceptionReturn(), J1.4.3.469. */
static bool illegal_return(Arm64Cpu *c, uint64_t spsr)
{
	bool spsr_aarch32 = spsr & 0x10;
	bool known, target_aarch32;
	unsigned target;

	if (!el_from_spsr(c, spsr, &target) || target > c->el)
		return true;
	known = el_using_aarch32_k(c, target, &target_aarch32);
	if (known && spsr_aarch32 != target_aarch32)
		return true;
	return target == 1 && arm64_el2_enabled(c) &&
	       (c->sys.hcr_el2 & ARM64_HCR_TGE);
}

/*
 * ERET, C6.2.157: AArch64_ExceptionReturn(ELR_ELx, SPSR_ELx). An illegal
 * return (D1.4.4.2) keeps the Exception level and stack pointer and sets
 * PSTATE.IL, so that the next instruction takes an Illegal Execution
 * state exception. It still restores NZCV and DAIF, and branches to
 * ELR_ELx, whose bits 63:32 and 1:0 are UNKNOWN when the SPSR is an
 * AArch32 one: this model keeps them.
 *
 * The return address is used as it is: AArch64_BranchAddr() would remove
 * a tag from its top byte when TCR_ELx.TBI says so, and the MMU, which
 * TCR_ELx belongs to, is not implemented yet. ClearExclusiveLocal() has
 * nothing to clear: the exclusive monitors are not implemented yet.
 */
Arm64Exec arm64_exec_eret(Arm64Cpu *c)
{
	if (c->el == 0)
		return ARM64_EXEC_UNDEFINED;

	uint64_t spsr = c->sys.spsr[c->el];
	uint64_t elr = c->sys.elr[c->el];

	if (illegal_return(c, spsr)) {
		c->il = true;
	} else if (spsr & 0x10) {
		snprintf(c->detail, sizeof(c->detail),
		         "exception return to AArch32 state");
		return ARM64_EXEC_UNIMPLEMENTED;
	} else {
		c->il = spsr & (UINT32_C(1) << 20);
		c->el = (spsr >> 2) & 3;
		c->sp_sel = spsr & 1;
	}
	c->nzcv = (uint32_t)spsr & 0xf0000000;
	c->daif = (uint32_t)spsr & 0x3c0;
	c->event_register = true; /* SendEventLocal() */
	c->next_pc = elr;
	return ARM64_EXEC_OK;
}

/*
 * The traps of WFI and WFE: AArch64_CheckForWFxTrap() in J1.2.2.50, then
 * AArch64_WFxTrap() in J1.2.2.58, which sends a trap to EL1 to EL2 when
 * HCR_EL2.TGE is set. WFE checks them only when it would wait, WFI only
 * when no interrupt is pending (Hint_WFE() and Hint_WFI() in J1.4.3.463
 * and 465). Returns ARM64_EXEC_OK when nothing traps.
 */
Arm64Exec arm64_check_wfx_trap(Arm64Cpu *c, bool wfe)
{
	unsigned target = 0;

	if (c->el == 0 &&
	    !(c->sys.sctlr[1] & (wfe ? ARM64_SCTLR_NTWE : ARM64_SCTLR_NTWI)))
		target = arm64_el1_target(c);
	else if (c->el <= 1 && arm64_el2_enabled(c) &&
	         (c->sys.hcr_el2 & (wfe ? ARM64_HCR_TWE : ARM64_HCR_TWI)))
		target = 2;
	else if (c->config.highest_el == 3 && c->el < 3 &&
	         (c->sys.scr_el3 & (wfe ? ARM64_SCR_TWE : ARM64_SCR_TWI)))
		target = 3;
	if (target == 0)
		return ARM64_EXEC_OK;
	/* ConditionSyndrome() in AArch64: CV = 1, COND = 1110. TI: 1 for WFE. */
	return arm64_raise(c, target, ARM64_EC_WFX,
	                   UINT32_C(0x1e) << 20 | (wfe ? 1 : 0));
}

/*
 * CheckSPAlignment(), J1.2.3.113, for a load or store whose base is SP:
 * SCTLR_ELx.SA, or SCTLR_EL1.SA0 at EL0, asks for 16-byte alignment.
 * AArch64_SPAlignmentFault(), J1.2.2.11.
 */
Arm64Exec arm64_check_sp_alignment(Arm64Cpu *c)
{
	uint64_t check = c->el == 0 ? c->sys.sctlr[1] & ARM64_SCTLR_SA0
	                            : c->sys.sctlr[c->el] & ARM64_SCTLR_SA;

	if (!check || (*current_sp(c) & 0xf) == 0)
		return ARM64_EXEC_OK;
	return arm64_raise(c, route(c, true), ARM64_EC_SP_ALIGNMENT, 0);
}

/*
 * An Alignment fault on a data access: AArch64_DataAbort(), J1.2.2.4,
 * with the syndrome of AArch64_FaultSyndrome(), J1.2.3.1. ISV is 0, as
 * for every fault of a stage 1 translation, so the ISS holds WnR and the
 * fault status code. FAR_ELx holds the address of the access.
 */
Arm64Exec arm64_alignment_fault(Arm64Cpu *c, uint64_t address, bool write)
{
	unsigned target = route(c, true);
	unsigned ec =
		target == c->el ? ARM64_EC_DATA_ABORT_SAME : ARM64_EC_DATA_ABORT_LOWER;

	arm64_raise(c, target, ec, (write ? 1u << 6 : 0) | ARM64_DFSC_ALIGNMENT);
	c->exception.far_valid = true;
	c->exception.far = address;
	return ARM64_EXEC_EXCEPTION;
}

/*
 * D1.4.10.1: an instruction at a misaligned PC takes a PC alignment
 * fault. AArch64_PCAlignmentFault(), J1.2.2.7: FAR_ELx holds the PC.
 */
void arm64_pc_alignment_fault(Arm64Cpu *c)
{
	arm64_raise(c, route(c, true), ARM64_EC_PC_ALIGNMENT, 0);
	c->exception.far_valid = true;
	c->exception.far = c->pc;
}

/*
 * AArch64_CheckIllegalState(), J1.2.2.51: an instruction run with
 * PSTATE.IL set.
 */
void arm64_illegal_state(Arm64Cpu *c)
{
	arm64_raise(c, arm64_el1_target(c), ARM64_EC_ILLEGAL_STATE, 0);
}
