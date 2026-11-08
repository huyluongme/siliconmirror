/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * System registers: MRS and MSR (register), C6.2.287 and C6.2.289, and
 * MSR (immediate), C6.2.288.
 *
 * The table below lists the AArch64 registers of Armv8.0 that the model
 * implements, with the Armv8.0 part of the "Accessing" pseudocode of each
 * one (C5.2 and D24): the lowest Exception level that may use it, and the
 * controls that trap its use at a lower level to a higher one. The CPU
 * model gives the values of the ID registers, the reset values and the
 * IMPLEMENTATION DEFINED registers.
 *
 * A core has registers that this model does not implement yet: debug,
 * performance monitors, Generic Timer, floating-point, and the System
 * registers of AArch32. The CPU model lists them, and using one stops
 * the machine as "not implemented yet". Other encodings are UNDEFINED.
 *
 * Writing SCTLR_ELx to turn on the MMU or big-endian data for the current
 * Exception level also stops the machine: neither is implemented yet.
 */

#include "cpu/arm64/sysreg.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/arm64/internal.h"
#include "util/bits.h"

/* Where the value of a register comes from. */
typedef enum Kind {
	KIND_FIELD,     /* a field of Arm64Cpu, at offset */
	KIND_ID,        /* a constant from the CPU model */
	KIND_NZCV,      /* PSTATE.{N,Z,C,V} */
	KIND_DAIF,      /* PSTATE.{D,A,I,F} */
	KIND_CURRENTEL, /* PSTATE.EL */
	KIND_SPSEL,     /* PSTATE.SP */
	KIND_SP_EL0,    /* UNDEFINED while it is the current stack pointer */
	KIND_SP_EL1,
	KIND_SP_EL2,
	KIND_MIDR,   /* VPIDR_EL2 when read at EL1 with EL2 enabled */
	KIND_MPIDR,  /* VMPIDR_EL2 likewise */
	KIND_CCSIDR, /* the cache that CSSELR_EL1 selects */
	KIND_ISR,    /* pending interrupts: none can be pending yet */
	KIND_RVBAR,  /* the reset address */
	KIND_ZERO,   /* reads as 0 */
} Kind;

/* Access rules. */
enum {
	RO = 1 << 0,             /* MSR is UNDEFINED */
	EL0_RO = 1 << 1,         /* MSR is UNDEFINED at EL0 */
	EL0_UMA = 1 << 2,        /* EL0 traps unless SCTLR_EL1.UMA */
	EL0_UCT = 1 << 3,        /* EL0 traps unless SCTLR_EL1.UCT */
	TRAP_TID1 = 1 << 4,      /* EL1 traps to EL2 on HCR_EL2.TID1 */
	TRAP_TID2 = 1 << 5,      /* EL0 and EL1 trap to EL2 on HCR_EL2.TID2 */
	TRAP_TID3 = 1 << 6,      /* EL1 traps to EL2 on HCR_EL2.TID3 */
	TRAP_TACR = 1 << 7,      /* EL1 traps to EL2 on HCR_EL2.TACR */
	TRAP_TVM = 1 << 8,       /* EL1 traps on HCR_EL2.TRVM (MRS), TVM (MSR) */
	TRAP_CPACR = 1 << 9,     /* CPTR_EL2.TCPAC at EL1, CPTR_EL3.TCPAC below */
	TRAP_CPTR_EL2 = 1 << 10, /* EL2 traps to EL3 on CPTR_EL3.TCPAC */
	TRAP_MDCR_EL2 = 1 << 11, /* EL2 traps to EL3 on MDCR_EL3.TDA */
	SCTLR = 1 << 12,         /* check the bits that are not implemented */
	RMR = 1 << 13,           /* likewise */
};

typedef struct SysReg {
	uint16_t encoding;
	const char *name;
	uint8_t min_el; /* UNDEFINED below this Exception level */
	uint16_t flags;
	Kind kind;
	size_t offset; /* for KIND_FIELD */
} SysReg;

#define FIELD(enc, name, el, flags, field)                                     \
	{enc, name, el, flags, KIND_FIELD, offsetof(Arm64Cpu, field)}
#define SPECIAL(enc, name, el, flags, kind) {enc, name, el, flags, kind, 0}

/* Sorted by encoding, for the binary search in find_sysreg(). */
static const SysReg sysregs[] = {
	SPECIAL(ARM64_MIDR_EL1, "MIDR_EL1", 1, RO, KIND_MIDR),
	SPECIAL(ARM64_MPIDR_EL1, "MPIDR_EL1", 1, RO, KIND_MPIDR),
	SPECIAL(ARM64_REVIDR_EL1, "REVIDR_EL1", 1, RO | TRAP_TID1, KIND_ID),
	SPECIAL(ARM64_ID_PFR0_EL1, "ID_PFR0_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_PFR1_EL1, "ID_PFR1_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_DFR0_EL1, "ID_DFR0_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_AFR0_EL1, "ID_AFR0_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_MMFR0_EL1, "ID_MMFR0_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_MMFR1_EL1, "ID_MMFR1_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_MMFR2_EL1, "ID_MMFR2_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_MMFR3_EL1, "ID_MMFR3_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_ISAR0_EL1, "ID_ISAR0_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_ISAR1_EL1, "ID_ISAR1_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_ISAR2_EL1, "ID_ISAR2_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_ISAR3_EL1, "ID_ISAR3_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_ISAR4_EL1, "ID_ISAR4_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_ISAR5_EL1, "ID_ISAR5_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_MVFR0_EL1, "MVFR0_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_MVFR1_EL1, "MVFR1_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_MVFR2_EL1, "MVFR2_EL1", 1, RO | TRAP_TID3, KIND_ID),
	SPECIAL(ARM64_ID_AA64PFR0_EL1, "ID_AA64PFR0_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	SPECIAL(ARM64_ID_AA64PFR1_EL1, "ID_AA64PFR1_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	SPECIAL(ARM64_ID_AA64DFR0_EL1, "ID_AA64DFR0_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	SPECIAL(ARM64_ID_AA64DFR1_EL1, "ID_AA64DFR1_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	SPECIAL(ARM64_ID_AA64AFR0_EL1, "ID_AA64AFR0_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	SPECIAL(ARM64_ID_AA64AFR1_EL1, "ID_AA64AFR1_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	SPECIAL(ARM64_ID_AA64ISAR0_EL1, "ID_AA64ISAR0_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	SPECIAL(ARM64_ID_AA64ISAR1_EL1, "ID_AA64ISAR1_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	SPECIAL(ARM64_ID_AA64MMFR0_EL1, "ID_AA64MMFR0_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	SPECIAL(ARM64_ID_AA64MMFR1_EL1, "ID_AA64MMFR1_EL1", 1, RO | TRAP_TID3,
            KIND_ID),
	FIELD(ARM64_SCTLR_EL1, "SCTLR_EL1", 1, SCTLR | TRAP_TVM, sys.sctlr[1]),
	FIELD(ARM64_ACTLR_EL1, "ACTLR_EL1", 1, TRAP_TACR, sys.actlr[1]),
	FIELD(ARM64_CPACR_EL1, "CPACR_EL1", 1, TRAP_CPACR, sys.cpacr_el1),
	FIELD(ARM64_TTBR0_EL1, "TTBR0_EL1", 1, TRAP_TVM, sys.ttbr0[1]),
	FIELD(ARM64_TTBR1_EL1, "TTBR1_EL1", 1, TRAP_TVM, sys.ttbr1_el1),
	FIELD(ARM64_TCR_EL1, "TCR_EL1", 1, TRAP_TVM, sys.tcr[1]),
	FIELD(ARM64_SPSR_EL1, "SPSR_EL1", 1, 0, sys.spsr[1]),
	FIELD(ARM64_ELR_EL1, "ELR_EL1", 1, 0, sys.elr[1]),
	SPECIAL(ARM64_SP_EL0, "SP_EL0", 1, 0, KIND_SP_EL0),
	SPECIAL(ARM64_SPSEL, "SPSel", 1, 0, KIND_SPSEL),
	SPECIAL(ARM64_CURRENTEL, "CurrentEL", 1, RO, KIND_CURRENTEL),
	FIELD(ARM64_AFSR0_EL1, "AFSR0_EL1", 1, TRAP_TVM, sys.afsr0[1]),
	FIELD(ARM64_AFSR1_EL1, "AFSR1_EL1", 1, TRAP_TVM, sys.afsr1[1]),
	FIELD(ARM64_ESR_EL1, "ESR_EL1", 1, TRAP_TVM, sys.esr[1]),
	FIELD(ARM64_FAR_EL1, "FAR_EL1", 1, TRAP_TVM, sys.far[1]),
	FIELD(ARM64_PAR_EL1, "PAR_EL1", 1, 0, sys.par_el1),
	FIELD(ARM64_MAIR_EL1, "MAIR_EL1", 1, TRAP_TVM, sys.mair[1]),
	FIELD(ARM64_AMAIR_EL1, "AMAIR_EL1", 1, TRAP_TVM, sys.amair[1]),
	FIELD(ARM64_VBAR_EL1, "VBAR_EL1", 1, 0, sys.vbar[1]),
	SPECIAL(ARM64_ISR_EL1, "ISR_EL1", 1, RO, KIND_ISR),
	FIELD(ARM64_CONTEXTIDR_EL1, "CONTEXTIDR_EL1", 1, TRAP_TVM,
          sys.contextidr_el1),
	FIELD(ARM64_TPIDR_EL1, "TPIDR_EL1", 1, 0, sys.tpidr[1]),
	SPECIAL(ARM64_CCSIDR_EL1, "CCSIDR_EL1", 1, RO | TRAP_TID2, KIND_CCSIDR),
	SPECIAL(ARM64_CLIDR_EL1, "CLIDR_EL1", 1, RO | TRAP_TID2, KIND_ID),
	SPECIAL(ARM64_AIDR_EL1, "AIDR_EL1", 1, RO | TRAP_TID1, KIND_ID),
	FIELD(ARM64_CSSELR_EL1, "CSSELR_EL1", 1, TRAP_TID2, sys.csselr_el1),
	SPECIAL(ARM64_CTR_EL0, "CTR_EL0", 0, RO | EL0_UCT | TRAP_TID2, KIND_ID),
	SPECIAL(ARM64_DCZID_EL0, "DCZID_EL0", 0, RO, KIND_ID),
	SPECIAL(ARM64_NZCV, "NZCV", 0, 0, KIND_NZCV),
	SPECIAL(ARM64_DAIF, "DAIF", 0, EL0_UMA, KIND_DAIF),
	FIELD(ARM64_TPIDR_EL0, "TPIDR_EL0", 0, 0, sys.tpidr[0]),
	FIELD(ARM64_TPIDRRO_EL0, "TPIDRRO_EL0", 0, EL0_RO, sys.tpidrro_el0),
	FIELD(ARM64_VPIDR_EL2, "VPIDR_EL2", 2, 0, sys.vpidr_el2),
	FIELD(ARM64_VMPIDR_EL2, "VMPIDR_EL2", 2, 0, sys.vmpidr_el2),
	FIELD(ARM64_SCTLR_EL2, "SCTLR_EL2", 2, SCTLR, sys.sctlr[2]),
	FIELD(ARM64_ACTLR_EL2, "ACTLR_EL2", 2, 0, sys.actlr[2]),
	FIELD(ARM64_HCR_EL2, "HCR_EL2", 2, 0, sys.hcr_el2),
	FIELD(ARM64_MDCR_EL2, "MDCR_EL2", 2, TRAP_MDCR_EL2, sys.mdcr_el2),
	FIELD(ARM64_CPTR_EL2, "CPTR_EL2", 2, TRAP_CPTR_EL2, sys.cptr_el2),
	FIELD(ARM64_HSTR_EL2, "HSTR_EL2", 2, 0, sys.hstr_el2),
	FIELD(ARM64_HACR_EL2, "HACR_EL2", 2, 0, sys.hacr_el2),
	FIELD(ARM64_TTBR0_EL2, "TTBR0_EL2", 2, 0, sys.ttbr0[2]),
	FIELD(ARM64_TCR_EL2, "TCR_EL2", 2, 0, sys.tcr[2]),
	FIELD(ARM64_VTTBR_EL2, "VTTBR_EL2", 2, 0, sys.vttbr_el2),
	FIELD(ARM64_VTCR_EL2, "VTCR_EL2", 2, 0, sys.vtcr_el2),
	FIELD(ARM64_SPSR_EL2, "SPSR_EL2", 2, 0, sys.spsr[2]),
	FIELD(ARM64_ELR_EL2, "ELR_EL2", 2, 0, sys.elr[2]),
	SPECIAL(ARM64_SP_EL1, "SP_EL1", 2, 0, KIND_SP_EL1),
	FIELD(ARM64_AFSR0_EL2, "AFSR0_EL2", 2, 0, sys.afsr0[2]),
	FIELD(ARM64_AFSR1_EL2, "AFSR1_EL2", 2, 0, sys.afsr1[2]),
	FIELD(ARM64_ESR_EL2, "ESR_EL2", 2, 0, sys.esr[2]),
	FIELD(ARM64_FAR_EL2, "FAR_EL2", 2, 0, sys.far[2]),
	FIELD(ARM64_HPFAR_EL2, "HPFAR_EL2", 2, 0, sys.hpfar_el2),
	FIELD(ARM64_MAIR_EL2, "MAIR_EL2", 2, 0, sys.mair[2]),
	FIELD(ARM64_AMAIR_EL2, "AMAIR_EL2", 2, 0, sys.amair[2]),
	FIELD(ARM64_VBAR_EL2, "VBAR_EL2", 2, 0, sys.vbar[2]),
	FIELD(ARM64_TPIDR_EL2, "TPIDR_EL2", 2, 0, sys.tpidr[2]),
	FIELD(ARM64_SCTLR_EL3, "SCTLR_EL3", 3, SCTLR, sys.sctlr[3]),
	FIELD(ARM64_ACTLR_EL3, "ACTLR_EL3", 3, 0, sys.actlr[3]),
	FIELD(ARM64_SCR_EL3, "SCR_EL3", 3, 0, sys.scr_el3),
	FIELD(ARM64_SDER32_EL3, "SDER32_EL3", 3, 0, sys.sder32_el3),
	FIELD(ARM64_CPTR_EL3, "CPTR_EL3", 3, 0, sys.cptr_el3),
	FIELD(ARM64_MDCR_EL3, "MDCR_EL3", 3, 0, sys.mdcr_el3),
	FIELD(ARM64_TTBR0_EL3, "TTBR0_EL3", 3, 0, sys.ttbr0[3]),
	FIELD(ARM64_TCR_EL3, "TCR_EL3", 3, 0, sys.tcr[3]),
	FIELD(ARM64_SPSR_EL3, "SPSR_EL3", 3, 0, sys.spsr[3]),
	FIELD(ARM64_ELR_EL3, "ELR_EL3", 3, 0, sys.elr[3]),
	SPECIAL(ARM64_SP_EL2, "SP_EL2", 3, 0, KIND_SP_EL2),
	FIELD(ARM64_AFSR0_EL3, "AFSR0_EL3", 3, 0, sys.afsr0[3]),
	FIELD(ARM64_AFSR1_EL3, "AFSR1_EL3", 3, 0, sys.afsr1[3]),
	FIELD(ARM64_ESR_EL3, "ESR_EL3", 3, 0, sys.esr[3]),
	FIELD(ARM64_FAR_EL3, "FAR_EL3", 3, 0, sys.far[3]),
	FIELD(ARM64_MAIR_EL3, "MAIR_EL3", 3, 0, sys.mair[3]),
	FIELD(ARM64_AMAIR_EL3, "AMAIR_EL3", 3, 0, sys.amair[3]),
	FIELD(ARM64_VBAR_EL3, "VBAR_EL3", 3, 0, sys.vbar[3]),
	SPECIAL(ARM64_RVBAR_EL3, "RVBAR_EL3", 3, RO, KIND_RVBAR),
	FIELD(ARM64_RMR_EL3, "RMR_EL3", 3, RMR, sys.rmr_el3),
	FIELD(ARM64_TPIDR_EL3, "TPIDR_EL3", 3, 0, sys.tpidr[3]),
};

static const SysReg *find_sysreg(uint16_t encoding)
{
	size_t lo = 0, hi = sizeof(sysregs) / sizeof(sysregs[0]);

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (sysregs[mid].encoding == encoding)
			return &sysregs[mid];
		if (sysregs[mid].encoding < encoding)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/* An IMPLEMENTATION DEFINED register of the CPU model, or -1. */
static int find_impdef(const Arm64Cpu *c, uint16_t encoding)
{
	const Arm64CpuModel *model = c->config.model;

	for (unsigned i = 0; model != NULL && i < model->num_impdef; i++)
		if (model->impdef[i].encoding == encoding)
			return (int)i;
	return -1;
}

/*
 * Where the IMPLEMENTATION DEFINED register at index is stored: in the
 * cluster for a cluster register, unless the core has no cluster.
 */
static uint64_t *impdef_value(Arm64Cpu *c, int index)
{
	if (c->config.model->impdef[index].cluster && c->config.cluster != NULL)
		return &c->config.cluster->impdef[index];
	return &c->sys.impdef[index];
}

/* The value the CPU model gives for a register, or 0. */
static uint64_t model_value(const Arm64Cpu *c, uint16_t encoding)
{
	const Arm64CpuModel *model = c->config.model;

	for (unsigned i = 0; model != NULL && i < model->num_values; i++)
		if (model->values[i].encoding == encoding)
			return model->values[i].value;
	return 0;
}

static uint64_t *field_of(Arm64Cpu *c, const SysReg *r)
{
	return (uint64_t *)((char *)c + r->offset);
}

/*
 * EL2Enabled(): EL2 is implemented and the lower Exception levels are
 * Non-secure, the only state that has EL2 in Armv8.0.
 */
static bool el2_enabled(const Arm64Cpu *c)
{
	if (c->config.highest_el < 2)
		return false;
	return c->config.highest_el == 2 || (c->sys.scr_el3 & ARM64_SCR_NS);
}

/*
 * Raise an exception for a trapped MRS, MSR or System instruction: EC
 * 0x18, with the fields of the instruction as the ISS, D24.2.44.
 */
static Arm64Exec trap(Arm64Cpu *c, unsigned target_el, uint32_t insn)
{
	uint32_t iss = field32(insn, 20, 19) << 20 | /* Op0 */
	               field32(insn, 7, 5) << 17 |   /* Op2 */
	               field32(insn, 18, 16) << 14 | /* Op1 */
	               field32(insn, 15, 12) << 10 | /* CRn */
	               field32(insn, 4, 0) << 5 |    /* Rt */
	               field32(insn, 11, 8) << 1 |   /* CRm */
	               bit32(insn, 21);              /* Direction: 1 for a read */

	c->exception.target_el = target_el;
	c->exception.esr = 0x18u << 26 | 1u << 25 | iss; /* EC, IL, ISS */
	return ARM64_EXEC_EXCEPTION;
}

/*
 * AArch64_SystemAccessTraptoEL1orEL2(): an EL0 trap goes to EL1, or to
 * EL2 when HCR_EL2.TGE routes EL0 exceptions there.
 */
static unsigned el0_trap_target(const Arm64Cpu *c)
{
	return el2_enabled(c) && (c->sys.hcr_el2 & ARM64_HCR_TGE) ? 2 : 1;
}

/* The Exception level that an access traps to, or 0 if it does not trap. */
static unsigned trap_target(const Arm64Cpu *c, const SysReg *r, bool read)
{
	uint64_t hcr = el2_enabled(c) ? c->sys.hcr_el2 : 0;
	bool el3 = c->config.highest_el == 3;

	switch (c->el) {
	case 0:
		if ((r->flags & EL0_UMA) && !(c->sys.sctlr[1] & ARM64_SCTLR_UMA))
			return el0_trap_target(c);
		if ((r->flags & EL0_UCT) && !(c->sys.sctlr[1] & ARM64_SCTLR_UCT))
			return el0_trap_target(c);
		if ((r->flags & TRAP_TID2) && (hcr & ARM64_HCR_TID2))
			return 2;
		return 0;
	case 1:
		if ((r->flags & TRAP_TID1) && (hcr & ARM64_HCR_TID1))
			return 2;
		if ((r->flags & TRAP_TID2) && (hcr & ARM64_HCR_TID2))
			return 2;
		if ((r->flags & TRAP_TID3) && (hcr & ARM64_HCR_TID3))
			return 2;
		if ((r->flags & TRAP_TACR) && (hcr & ARM64_HCR_TACR))
			return 2;
		if ((r->flags & TRAP_TVM) &&
		    (hcr & (read ? ARM64_HCR_TRVM : ARM64_HCR_TVM)))
			return 2;
		if ((r->flags & TRAP_CPACR) && el2_enabled(c) &&
		    (c->sys.cptr_el2 & ARM64_CPTR_TCPAC))
			return 2;
		if ((r->flags & TRAP_CPACR) && el3 &&
		    (c->sys.cptr_el3 & ARM64_CPTR_TCPAC))
			return 3;
		return 0;
	case 2:
		if ((r->flags & (TRAP_CPACR | TRAP_CPTR_EL2)) && el3 &&
		    (c->sys.cptr_el3 & ARM64_CPTR_TCPAC))
			return 3;
		if ((r->flags & TRAP_MDCR_EL2) && el3 &&
		    (c->sys.mdcr_el3 & ARM64_MDCR_TDA))
			return 3;
		return 0;
	default:
		return 0;
	}
}

/* Read a register; false if the read is UNDEFINED. */
static bool read_sysreg(Arm64Cpu *c, const SysReg *r, uint64_t *value)
{
	bool virtual_id = c->el == 1 && el2_enabled(c);

	switch (r->kind) {
	case KIND_FIELD:
		*value = *field_of(c, r);
		return true;
	case KIND_ID:
		*value = model_value(c, r->encoding);
		return true;
	case KIND_NZCV:
		*value = c->nzcv;
		return true;
	case KIND_DAIF:
		*value = c->daif;
		return true;
	case KIND_CURRENTEL:
		*value = (uint64_t)c->el << 2;
		return true;
	case KIND_SPSEL:
		*value = c->sp_sel;
		return true;
	case KIND_SP_EL0:
		*value = c->sp_el[0];
		return c->sp_sel;
	case KIND_SP_EL1:
		*value = c->sp_el[1];
		return true;
	case KIND_SP_EL2:
		*value = c->sp_el[2];
		return true;
	case KIND_MIDR:
		*value = virtual_id ? c->sys.vpidr_el2 : model_value(c, ARM64_MIDR_EL1);
		return true;
	case KIND_MPIDR:
		*value = virtual_id ? c->sys.vmpidr_el2 : c->config.mpidr;
		return true;
	case KIND_CCSIDR: {
		const Arm64CpuModel *model = c->config.model;
		uint64_t index = c->sys.csselr_el1 & 0xf;

		*value = model != NULL && index < model->num_ccsidr
		             ? model->ccsidr[index]
		             : 0;
		return true;
	}
	case KIND_ISR:
		*value = 0;
		return true;
	case KIND_RVBAR:
		*value = c->config.reset_pc;
		return true;
	case KIND_ZERO:
		*value = 0;
		return true;
	}
	return false;
}

/*
 * The SCTLR_ELx that controls the current Exception level, to which
 * writes can turn on what is not implemented yet.
 */
static bool controls_current_el(const Arm64Cpu *c, uint16_t encoding)
{
	unsigned el = c->el > 1 ? c->el : 1;

	return encoding == (el == 3   ? ARM64_SCTLR_EL3
	                    : el == 2 ? ARM64_SCTLR_EL2
	                              : ARM64_SCTLR_EL1);
}

/* Write a register. */
static Arm64Exec write_sysreg(Arm64Cpu *c, const SysReg *r, uint64_t value)
{
	switch (r->kind) {
	case KIND_FIELD:
		break;
	case KIND_NZCV:
		c->nzcv = (uint32_t)value & 0xf0000000;
		return ARM64_EXEC_OK;
	case KIND_DAIF:
		c->daif = (uint32_t)value & 0x3c0;
		return ARM64_EXEC_OK;
	case KIND_SPSEL:
		c->sp_sel = value & 1;
		return ARM64_EXEC_OK;
	case KIND_SP_EL0:
		if (!c->sp_sel)
			return ARM64_EXEC_UNDEFINED;
		c->sp_el[0] = value;
		return ARM64_EXEC_OK;
	case KIND_SP_EL1:
		c->sp_el[1] = value;
		return ARM64_EXEC_OK;
	case KIND_SP_EL2:
		c->sp_el[2] = value;
		return ARM64_EXEC_OK;
	default: /* the read-only kinds never get here */
		return ARM64_EXEC_UNDEFINED;
	}

	if ((r->flags & SCTLR) && controls_current_el(c, r->encoding)) {
		if (value & ARM64_SCTLR_M) {
			snprintf(c->detail, sizeof(c->detail), "turning on the MMU with %s",
			         r->name);
			return ARM64_EXEC_UNIMPLEMENTED;
		}
		if (value & ARM64_SCTLR_EE) {
			snprintf(c->detail, sizeof(c->detail),
			         "big-endian data, SCTLR_ELx.EE");
			return ARM64_EXEC_UNIMPLEMENTED;
		}
	}
	/* RMR_EL3: AA64 = 0 would reset into AArch32, RR = 1 asks for one. */
	if ((r->flags & RMR) && (!(value & 1) || (value & 2))) {
		snprintf(c->detail, sizeof(c->detail), "reset through RMR_EL3");
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	if (r->encoding == ARM64_CSSELR_EL1)
		value &= 0xf; /* Level and InD; the rest is RES0 */
	*field_of(c, r) = value;
	return ARM64_EXEC_OK;
}

static Arm64Exec access_sysreg(Arm64Cpu *c, const SysReg *r, uint32_t insn)
{
	bool read = bit32(insn, 21);
	unsigned t = field32(insn, 4, 0);

	if (c->el < r->min_el)
		return ARM64_EXEC_UNDEFINED;
	if (!read && ((r->flags & RO) || (c->el == 0 && (r->flags & EL0_RO))))
		return ARM64_EXEC_UNDEFINED;

	unsigned target = trap_target(c, r, read);

	if (target != 0)
		return trap(c, target, insn);
	if (!read)
		return write_sysreg(c, r, get_x(c, t));

	uint64_t value;

	if (!read_sysreg(c, r, &value))
		return ARM64_EXEC_UNDEFINED;
	set_x(c, t, value);
	return ARM64_EXEC_OK;
}

static Arm64Exec access_impdef(Arm64Cpu *c, int index, uint32_t insn)
{
	const Arm64ImpDefReg *d = &c->config.model->impdef[index];
	unsigned t = field32(insn, 4, 0);

	if (c->el < 1)
		return ARM64_EXEC_UNDEFINED;
	if (bit32(insn, 21)) {
		set_x(c, t, *impdef_value(c, index));
		return ARM64_EXEC_OK;
	}
	if (d->read_only)
		return ARM64_EXEC_UNDEFINED;
	if (d->actlr_bit >= 0 && c->el < 3) {
		uint64_t bit = (uint64_t)1 << d->actlr_bit;

		if (c->config.highest_el == 3 && !(c->sys.actlr[3] & bit))
			return trap(c, 3, insn);
		if (c->el == 1 && el2_enabled(c) && !(c->sys.actlr[2] & bit))
			return trap(c, 2, insn);
	}
	*impdef_value(c, index) = d->fixed_write ? d->write_value : get_x(c, t);
	return ARM64_EXEC_OK;
}

/* A register of the core that is not implemented yet, or NULL. */
static const char *missing_name(const Arm64Cpu *c, uint16_t encoding)
{
	const Arm64CpuModel *model = c->config.model;

	for (unsigned i = 0; model != NULL && i < model->num_missing; i++)
		if (model->missing[i].encoding == encoding)
			return model->missing[i].name;
	return NULL;
}

Arm64Exec arm64_exec_sysreg(Arm64Cpu *c, uint32_t insn)
{
	uint16_t encoding = (uint16_t)field32(insn, 20, 5);
	const SysReg *r = find_sysreg(encoding);
	const char *missing;
	int index;

	if (r != NULL)
		return access_sysreg(c, r, insn);
	index = find_impdef(c, encoding);
	if (index >= 0)
		return access_impdef(c, index, insn);
	/*
	 * D23.3: an unused encoding of the ID register space with op1 == 0
	 * and CRm == 2 to 7 reads as 0 from EL1 up. Without FEAT_FGT, it is
	 * IMPLEMENTATION DEFINED whether HCR_EL2.TID3 traps it; this model
	 * traps it like the ID registers. This covers the ID registers of
	 * newer features, such as ID_AA64MMFR2_EL1: all their fields read as
	 * 0, "not implemented".
	 */
	if (encoding >> 7 == ARM64_SYSREG(3, 0, 0, 0, 0) >> 7 &&
	    ((encoding >> 3) & 0xf) >= 2) {
		static const SysReg reserved_id =
			SPECIAL(0, "reserved ID register", 1, RO | TRAP_TID3, KIND_ZERO);

		return access_sysreg(c, &reserved_id, insn);
	}
	missing = missing_name(c, encoding);
	if (missing != NULL) {
		snprintf(c->detail, sizeof(c->detail), "System register %s", missing);
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	return ARM64_EXEC_UNDEFINED;
}

Arm64Exec arm64_exec_msr_imm(Arm64Cpu *c, uint32_t insn)
{
	unsigned op1 = field32(insn, 18, 16);
	unsigned op2 = field32(insn, 7, 5);
	uint32_t bits = field32(insn, 11, 8) << 6; /* CRm, as in DAIF */

	if (field32(insn, 4, 0) != 0x1f) /* Rt != 11111: UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;

	switch (op1 << 3 | op2) {
	case 0x05: /* SPSel, from EL1 */
		if (c->el < 1)
			return ARM64_EXEC_UNDEFINED;
		c->sp_sel = bit32(insn, 8);
		return ARM64_EXEC_OK;
	case 0x1e: /* DAIFSet */
	case 0x1f: /* DAIFClr */
		/* AArch64_CheckDAIFAccess(): EL0 needs SCTLR_EL1.UMA. */
		if (c->el == 0 && !(c->sys.sctlr[1] & ARM64_SCTLR_UMA))
			return trap(c, el0_trap_target(c), insn);
		if (op2 == 6)
			c->daif |= bits;
		else
			c->daif &= ~bits;
		return ARM64_EXEC_OK;
	default:
		/* Other fields need newer features, or are RESERVED. */
		return ARM64_EXEC_UNDEFINED;
	}
}

void arm64_sysreg_reset(Arm64Cpu *c)
{
	const Arm64CpuModel *model = c->config.model;

	memset(&c->sys, 0, sizeof(c->sys));
	for (size_t i = 0; i < sizeof(sysregs) / sizeof(sysregs[0]); i++)
		if (sysregs[i].kind == KIND_FIELD)
			*field_of(c, &sysregs[i]) = model_value(c, sysregs[i].encoding);
	c->sys.vmpidr_el2 = c->config.mpidr;
	for (unsigned i = 0; model != NULL && i < model->num_impdef; i++)
		c->sys.impdef[i] = model->impdef[i].reset;
}

Arm64Cluster *arm64_cluster_create(const Arm64CpuModel *model)
{
	if (model == NULL || model->num_impdef > ARM64_MAX_IMPDEF)
		return NULL;

	Arm64Cluster *cluster = calloc(1, sizeof(*cluster));

	if (cluster == NULL)
		return NULL;
	cluster->model = model;
	arm64_cluster_reset(cluster);
	return cluster;
}

void arm64_cluster_reset(Arm64Cluster *cluster)
{
	const Arm64CpuModel *model = cluster->model;

	for (unsigned i = 0; i < model->num_impdef; i++)
		cluster->impdef[i] =
			model->impdef[i].cluster ? model->impdef[i].reset : 0;
}

void arm64_cluster_destroy(Arm64Cluster *cluster)
{
	free(cluster);
}

bool arm64_cpu_read_sysreg(Arm64Cpu *c, uint16_t encoding, uint64_t *value)
{
	const SysReg *r = find_sysreg(encoding);
	int index;

	if (r != NULL) {
		/* SP_EL0 can be read even while it is the current SP. */
		if (r->kind == KIND_SP_EL0) {
			*value = c->sp_el[0];
			return true;
		}
		return read_sysreg(c, r, value);
	}
	index = find_impdef(c, encoding);
	if (index < 0)
		return false;
	*value = *impdef_value(c, index);
	return true;
}

bool arm64_cpu_write_sysreg(Arm64Cpu *c, uint16_t encoding, uint64_t value)
{
	const SysReg *r = find_sysreg(encoding);
	int index;

	if (r != NULL) {
		switch (r->kind) {
		case KIND_FIELD:
			*field_of(c, r) = value;
			return true;
		case KIND_SP_EL0:
			c->sp_el[0] = value;
			return true;
		case KIND_NZCV:
		case KIND_DAIF:
		case KIND_SPSEL:
		case KIND_SP_EL1:
		case KIND_SP_EL2:
			return write_sysreg(c, r, value) == ARM64_EXEC_OK;
		default:
			return false;
		}
	}
	index = find_impdef(c, encoding);
	if (index < 0)
		return false;

	const Arm64ImpDefReg *d = &c->config.model->impdef[index];

	if (d->read_only)
		return false;
	*impdef_value(c, index) = d->fixed_write ? d->write_value : value;
	return true;
}

const char *arm64_sysreg_name(const Arm64Cpu *c, uint16_t encoding)
{
	const SysReg *r = find_sysreg(encoding);
	int index;

	if (r != NULL)
		return r->name;
	index = find_impdef(c, encoding);
	return index >= 0 ? c->config.model->impdef[index].name : NULL;
}

bool arm64_sysreg_table_is_sorted(void)
{
	for (size_t i = 1; i < sizeof(sysregs) / sizeof(sysregs[0]); i++)
		if (sysregs[i - 1].encoding >= sysregs[i].encoding)
			return false;
	return true;
}
