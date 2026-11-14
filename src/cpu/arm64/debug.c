/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Self-hosted debug of an Armv8.0 PE: the System registers of D24.3 that
 * software uses with no external debugger, with the Armv8.0 part of their
 * "Accessing" pseudocode. ID_AA64DFR0_EL1 gives how many breakpoints and
 * watchpoints there are.
 *
 * - DBGBVR<n>_EL1, DBGBCR<n>_EL1, DBGWVR<n>_EL1 and DBGWCR<n>_EL1: one
 *   past the last breakpoint or watchpoint is UNDEFINED.
 * - MDSCR_EL1: MDE, KDE, TDCC and SS hold a value. Its other fields show
 *   EDSCR, which only an external debugger changes, and are writable for
 *   a save and restore while the OS Lock is locked (RXfull, TXfull, RXO,
 *   TXU, INTdis, TDA, HDE and ERR); the rest is RES0.
 * - OSLAR_EL1 locks and unlocks the OS Lock, which OSLSR_EL1 shows; it is
 *   locked from a Cold reset, as the Cortex-A53's OSLSR_EL1 resets to 0xA.
 * - OSDLR_EL1.DLK, the OS Double Lock.
 *
 * EL0 has none of them. MDCR_EL2.TDE or TDA, and for the OS Lock
 * registers TDE or TDOSA, trap EL1 to EL2; MDCR_EL3.TDA or TDOSA trap EL1
 * and EL2 to EL3.
 *
 * Debug exceptions and halting are not emulated yet: the machine stops
 * when software could cause one. That is software step (MDSCR_EL1.SS), an
 * enabled breakpoint or watchpoint with MDSCR_EL1.MDE, and with the OS
 * Lock unlocked, EDSCR.HDE (HLT would halt, see exception.c) or EDSCR.TDA
 * (an access to a breakpoint or watchpoint register would halt). The
 * checks leave out what else could keep the exception from happening,
 * such as PSTATE.D or the OS Lock: they err on the side of stopping.
 * Linux clears these registers when it boots.
 */

#include <stdio.h>
#include <string.h>

#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/bits.h"

/* MDSCR_EL1. */
#define MDSCR_SS (1u << 0)
#define MDSCR_ERR (1u << 6)
#define MDSCR_TDCC (1u << 12)
#define MDSCR_KDE (1u << 13)
#define MDSCR_HDE (1u << 14)
#define MDSCR_MDE (1u << 15)
#define MDSCR_TDA (1u << 21)
#define MDSCR_INTDIS (3u << 22)
#define MDSCR_TXU (1u << 26)
#define MDSCR_RXO (1u << 27)
#define MDSCR_TXFULL (1u << 29)
#define MDSCR_RXFULL (1u << 30)
#define MDSCR_RW (MDSCR_SS | MDSCR_TDCC | MDSCR_KDE | MDSCR_MDE)
#define MDSCR_EDSCR                                                            \
	(MDSCR_ERR | MDSCR_HDE | MDSCR_TDA | MDSCR_INTDIS | MDSCR_TXU |            \
	 MDSCR_RXO | MDSCR_TXFULL | MDSCR_RXFULL)

/* MDCR_EL2 and MDCR_EL3. */
#define MDCR_TDA (1u << 9)
#define MDCR_TDOSA (1u << 10)

/* OSLSR_EL1: OSLM is 0b10, the OS Lock of Armv8. */
#define OSLSR_OSLM 0x8u
#define OSLSR_OSLK_SHIFT 1

/*
 * The fields of DBGBCR<n>_EL1 (BT, LBN, SSC, HMC, BAS, PMC, E) and of
 * DBGWCR<n>_EL1 (MASK, WT, LBN, SSC, HMC, BAS, LSC, PAC, E); bits [1:0]
 * of the value registers are RES0.
 */
#define BCR_MASK 0x00ffe1e7u
#define WCR_MASK 0x1f1fffffu
#define VR_MASK (~(uint64_t)3)
#define CR_E 1u

typedef enum Reg {
	DBGBVR,
	DBGBCR,
	DBGWVR,
	DBGWCR,
	MDSCR,
	OSLAR,
	OSLSR,
	OSDLR
} Reg;

static const char *const names[] = {
	"DBGBVR<n>_EL1", "DBGBCR<n>_EL1", "DBGWVR<n>_EL1", "DBGWCR<n>_EL1",
	"MDSCR_EL1",     "OSLAR_EL1",     "OSLSR_EL1",     "OSDLR_EL1",
};

/* Which register an encoding is, and its n for the pairs. */
static bool decode(uint16_t encoding, Reg *reg, unsigned *n)
{
	*n = 0;
	switch (encoding) {
	case ARM64_MDSCR_EL1:
		*reg = MDSCR;
		return true;
	case ARM64_OSLAR_EL1:
		*reg = OSLAR;
		return true;
	case ARM64_OSLSR_EL1:
		*reg = OSLSR;
		return true;
	case ARM64_OSDLR_EL1:
		*reg = OSDLR;
		return true;
	}
	/* op0 2, op1 0, CRn 0, CRm n, op2 4 to 7. */
	if ((encoding & ~(0xfu << 3 | 7u)) != ARM64_SYSREG(2, 0, 0, 0, 0) ||
	    (encoding & 7) < 4)
		return false;
	*n = encoding >> 3 & 0xf;
	*reg = (Reg)(DBGBVR + (encoding & 7) - 4);
	return true;
}

static uint64_t dfr0(const Arm64Cpu *c)
{
	return arm64_model_value(c, ARM64_ID_AA64DFR0_EL1);
}

/* Whether the core has debug: ID_AA64DFR0_EL1.DebugVer is not 0. */
static bool implemented(const Arm64Cpu *c)
{
	return (dfr0(c) & 0xf) != 0;
}

/* How many breakpoints, or watchpoints: BRPs and WRPs, less one. */
static unsigned pairs(const Arm64Cpu *c, Reg reg)
{
	unsigned shift = reg == DBGBVR || reg == DBGBCR ? 12 : 20;

	return (unsigned)(dfr0(c) >> shift & 0xf) + 1;
}

enum { ACCESS_UNDEFINED = -1, ACCESS_STOP = -2 };

/*
 * The checks of the "Accessing" pseudocode: 0 for the register, 2 or 3
 * for a trap to that Exception level, ACCESS_UNDEFINED, or ACCESS_STOP
 * with c->detail.
 */
static int check(Arm64Cpu *c, Reg reg, unsigned n, bool read)
{
	bool os = reg == OSLAR || reg == OSLSR || reg == OSDLR;
	bool pair = reg <= DBGWCR;
	uint32_t el2_traps = ARM64_MDCR_TDE | (os ? MDCR_TDOSA : MDCR_TDA);
	uint32_t el3_trap = os ? MDCR_TDOSA : MDCR_TDA;

	if (pair && n >= pairs(c, reg))
		return ACCESS_UNDEFINED;
	if ((reg == OSLAR && read) || (reg == OSLSR && !read) || c->el == 0)
		return ACCESS_UNDEFINED;
	if (c->el == 1 && arm64_el2_enabled(c) && (c->sys.mdcr_el2 & el2_traps))
		return 2;
	if (c->el < 3 && c->config.highest_el == 3 && (c->sys.mdcr_el3 & el3_trap))
		return 3;
	if (pair && !c->debug.oslk && (c->debug.edscr & MDSCR_TDA)) {
		snprintf(c->detail, sizeof(c->detail),
		         "halting on %s, as EDSCR.TDA asks", names[reg]);
		return ACCESS_STOP;
	}
	return 0;
}

static uint64_t read_reg(const Arm64Cpu *c, Reg reg, unsigned n)
{
	const Arm64Debug *d = &c->debug;

	switch (reg) {
	case DBGBVR:
		return d->bvr[n];
	case DBGBCR:
		return d->bcr[n];
	case DBGWVR:
		return d->wvr[n];
	case DBGWCR:
		return d->wcr[n];
	case MDSCR:
		return d->mdscr | d->edscr;
	case OSLSR:
		return OSLSR_OSLM | (uint32_t)d->oslk << OSLSR_OSLK_SHIFT;
	case OSDLR:
		return d->dlk;
	case OSLAR:
		break;
	}
	return 0;
}

static void write_reg(Arm64Cpu *c, Reg reg, unsigned n, uint64_t value)
{
	Arm64Debug *d = &c->debug;

	switch (reg) {
	case DBGBVR:
		d->bvr[n] = value & VR_MASK;
		return;
	case DBGBCR:
		d->bcr[n] = (uint32_t)value & BCR_MASK;
		return;
	case DBGWVR:
		d->wvr[n] = value & VR_MASK;
		return;
	case DBGWCR:
		d->wcr[n] = (uint32_t)value & WCR_MASK;
		return;
	case MDSCR:
		d->mdscr = (uint32_t)value & MDSCR_RW;
		if (d->oslk)
			d->edscr = (uint32_t)value & MDSCR_EDSCR;
		return;
	case OSLAR:
		d->oslk = value & 1;
		return;
	case OSDLR:
		d->dlk = value & 1;
		return;
	case OSLSR:
		return;
	}
}

/* Whether an enabled breakpoint or watchpoint exists. */
static bool any_enabled(const Arm64Cpu *c)
{
	for (unsigned i = 0; i < ARM64_DEBUG_PAIRS; i++)
		if ((c->debug.bcr[i] | c->debug.wcr[i]) & CR_E)
			return true;
	return false;
}

/* Stop if software could now cause what is not emulated: see the top. */
static Arm64Exec check_state(Arm64Cpu *c)
{
	const Arm64Debug *d = &c->debug;
	const char *what = NULL;

	if (d->mdscr & MDSCR_SS)
		what = "software step, with MDSCR_EL1.SS";
	else if ((d->mdscr & MDSCR_MDE) && any_enabled(c))
		what = "breakpoints and watchpoints, with MDSCR_EL1.MDE";
	else if (!d->oslk && (d->edscr & MDSCR_HDE))
		what = "halting, with EDSCR.HDE";
	if (what == NULL)
		return ARM64_EXEC_OK;
	snprintf(c->detail, sizeof(c->detail), "%s", what);
	return ARM64_EXEC_UNIMPLEMENTED;
}

bool arm64_debug_owns(uint16_t encoding)
{
	Reg reg;
	unsigned n;

	return decode(encoding, &reg, &n);
}

const char *arm64_debug_name(uint16_t encoding)
{
	Reg reg;
	unsigned n;

	return decode(encoding, &reg, &n) ? names[reg] : NULL;
}

Arm64Exec arm64_debug_access(Arm64Cpu *c, uint32_t insn)
{
	uint16_t encoding = (uint16_t)field32(insn, 20, 5);
	bool read = bit32(insn, 21);
	unsigned t = field32(insn, 4, 0);
	Reg reg;
	unsigned n;
	int where;

	if (!implemented(c) || !decode(encoding, &reg, &n))
		return ARM64_EXEC_UNDEFINED;
	where = check(c, reg, n, read);
	if (where == ACCESS_UNDEFINED)
		return ARM64_EXEC_UNDEFINED;
	if (where == ACCESS_STOP)
		return ARM64_EXEC_UNIMPLEMENTED;
	if (where > 0)
		return arm64_sys_trap(c, (unsigned)where, insn);
	if (read) {
		set_x(c, t, read_reg(c, reg, n));
		return ARM64_EXEC_OK;
	}
	write_reg(c, reg, n, get_x(c, t));
	return check_state(c);
}

/* As a debugger would: no pair past the last. */
static bool direct(const Arm64Cpu *c, uint16_t encoding, Reg *reg, unsigned *n)
{
	return implemented(c) && decode(encoding, reg, n) &&
	       (*reg > DBGWCR || *n < pairs(c, *reg));
}

bool arm64_debug_read(Arm64Cpu *c, uint16_t encoding, uint64_t *value)
{
	Reg reg;
	unsigned n;

	if (!direct(c, encoding, &reg, &n) || reg == OSLAR)
		return false;
	*value = read_reg(c, reg, n);
	return true;
}

bool arm64_debug_write(Arm64Cpu *c, uint16_t encoding, uint64_t value)
{
	Reg reg;
	unsigned n;

	if (!direct(c, encoding, &reg, &n) || reg == OSLSR)
		return false;
	write_reg(c, reg, n, value);
	return true;
}

/*
 * A reset, taken as a Cold reset: the OS Lock locked, the rest 0. The
 * registers whose reset value is UNKNOWN reset to 0 here.
 */
void arm64_debug_reset(Arm64Cpu *c)
{
	memset(&c->debug, 0, sizeof(c->debug));
	c->debug.oslk = true;
}
