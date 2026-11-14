/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The Performance Monitors of an Armv8.0 PE, FEAT_PMUv3: the registers
 * of D24.5, with the Armv8.0 part of their "Accessing" pseudocode. The
 * CPU model gives PMCR_EL0's reset value, whose IMP, IDCODE and N fields
 * do not change, and PMCEID0_EL0 and PMCEID1_EL0.
 *
 * At EL0 a register needs PMUSERENR_EL0.EN, but PMSWINC_EL0 takes SW
 * too, reads of PMCCNTR_EL0 CR, and reads of the event counters and both
 * accesses of PMSELR_EL0 ER. MDCR_EL2.TPM traps EL0 and EL1 to EL2,
 * MDCR_EL2.TPMCR does for PMCR_EL0 alone, and MDCR_EL3.TPM traps every
 * level below EL3. With EL2 enabled, EL0 and EL1 reach the event
 * counters below MDCR_EL2.HPMN, the first range: the bits of the others
 * read as 0 and ignore writes.
 *
 * The registers hold what software writes, but the counters do not count
 * yet: when software turns one on, the machine stops, "not implemented
 * yet". A counter counts as on when it is enabled and PMCR_EL0.E is set,
 * or when it is an event counter of the second range and MDCR_EL2.HPME
 * is set, whatever its filters and its range say: the check errs on the
 * side of stopping. Linux probes and resets the PMU when it boots, and
 * leaves the counters off until a tool such as perf asks for them.
 *
 * What the architecture leaves CONSTRAINED UNPREDICTABLE stops the
 * machine too: an event counter at or past PMCR_EL0.N, one past
 * MDCR_EL2.HPMN from EL0 or EL1, and a reserved MDCR_EL2.HPMN, 0 or
 * more than N.
 */

#include <stdio.h>
#include <string.h>

#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/bits.h"
#include "util/macros.h"

/* PMCR_EL0. */
#define PMCR_E (1u << 0)
#define PMCR_P (1u << 1)
#define PMCR_C (1u << 2)
#define PMCR_RW 0x79u /* E, D, X, DP and LC */
#define PMCR_N_SHIFT 11
#define PMCR_N_MASK (0x1fu << PMCR_N_SHIFT)

/* PMUSERENR_EL0. */
#define USERENR_EN (1u << 0)
#define USERENR_SW (1u << 1)
#define USERENR_CR (1u << 2)
#define USERENR_ER (1u << 3)
#define USERENR_MASK 0xfu

/* MDCR_EL2, and MDCR_EL3.TPM. */
#define MDCR_HPMN_MASK 0x1fu
#define MDCR_TPMCR (1u << 5)
#define MDCR_TPM (1u << 6)
#define MDCR_HPME (1u << 7)

#define CYCLE (1u << 31) /* the cycle counter's bit of the masks */
#define SEL_CYCLE 31     /* PMSELR_EL0.SEL that selects PMCCFILTR_EL0 */
#define SEL_MASK 0x1fu
/* P, U, NSK, NSU, NSH, M and evtCount[9:0]: the rest is RES0. */
#define EVTYPER_MASK 0xfc0003ffu
#define CCFILTR_MASK 0xfc000000u

typedef enum Reg {
	PMCR,
	PMCNTENSET,
	PMCNTENCLR,
	PMOVSCLR,
	PMSWINC,
	PMSELR,
	PMCEID0,
	PMCEID1,
	PMCCNTR,
	PMXEVTYPER,
	PMXEVCNTR,
	PMUSERENR,
	PMOVSSET,
	PMINTENSET,
	PMINTENCLR,
	PMCCFILTR,
	PMEVCNTR,  /* <m> */
	PMEVTYPER, /* <m> */
} Reg;

static const struct {
	uint16_t encoding;
	Reg reg;
	const char *name;
} regs[] = {
	{ARM64_PMCR_EL0, PMCR, "PMCR_EL0"},
	{ARM64_PMCNTENSET_EL0, PMCNTENSET, "PMCNTENSET_EL0"},
	{ARM64_PMCNTENCLR_EL0, PMCNTENCLR, "PMCNTENCLR_EL0"},
	{ARM64_PMOVSCLR_EL0, PMOVSCLR, "PMOVSCLR_EL0"},
	{ARM64_PMSWINC_EL0, PMSWINC, "PMSWINC_EL0"},
	{ARM64_PMSELR_EL0, PMSELR, "PMSELR_EL0"},
	{ARM64_PMCEID0_EL0, PMCEID0, "PMCEID0_EL0"},
	{ARM64_PMCEID1_EL0, PMCEID1, "PMCEID1_EL0"},
	{ARM64_PMCCNTR_EL0, PMCCNTR, "PMCCNTR_EL0"},
	{ARM64_PMXEVTYPER_EL0, PMXEVTYPER, "PMXEVTYPER_EL0"},
	{ARM64_PMXEVCNTR_EL0, PMXEVCNTR, "PMXEVCNTR_EL0"},
	{ARM64_PMUSERENR_EL0, PMUSERENR, "PMUSERENR_EL0"},
	{ARM64_PMOVSSET_EL0, PMOVSSET, "PMOVSSET_EL0"},
	{ARM64_PMINTENSET_EL1, PMINTENSET, "PMINTENSET_EL1"},
	{ARM64_PMINTENCLR_EL1, PMINTENCLR, "PMINTENCLR_EL1"},
	{ARM64_PMCCFILTR_EL0, PMCCFILTR, "PMCCFILTR_EL0"},
};

/* Which register an encoding is, and its counter m for PMEV*<m>. */
static bool decode(uint16_t encoding, Reg *reg, unsigned *m)
{
	*m = 0;
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		if (regs[i].encoding == encoding) {
			*reg = regs[i].reg;
			return true;
		}
	}
	/* m is CRm[1:0]:op2, from 0 to 30; PMEVTYPER<31> is PMCCFILTR. */
	if (encoding >= ARM64_PMEVCNTR_EL0(0) &&
	    encoding <= ARM64_PMEVCNTR_EL0(30)) {
		*reg = PMEVCNTR;
		*m = encoding - ARM64_PMEVCNTR_EL0(0);
		return true;
	}
	if (encoding >= ARM64_PMEVTYPER_EL0(0) &&
	    encoding <= ARM64_PMEVTYPER_EL0(30)) {
		*reg = PMEVTYPER;
		*m = encoding - ARM64_PMEVTYPER_EL0(0);
		return true;
	}
	return false;
}

/* Whether the core has the PMU: ID_AA64DFR0_EL1.PMUVer is not 0. */
static bool implemented(const Arm64Cpu *c)
{
	return (arm64_model_value(c, ARM64_ID_AA64DFR0_EL1) >> 8 & 0xf) != 0;
}

/* PMCR_EL0.N: how many event counters there are. */
static unsigned counters(const Arm64Cpu *c)
{
	return (arm64_model_value(c, ARM64_PMCR_EL0) & PMCR_N_MASK) >> PMCR_N_SHIFT;
}

static unsigned hpmn(const Arm64Cpu *c)
{
	return c->sys.mdcr_el2 & MDCR_HPMN_MASK;
}

/*
 * How many event counters the current Exception level reaches,
 * GetNumEventCountersAccessible(): those of the first range at EL0 and
 * EL1 when EL2 is enabled.
 */
static unsigned accessible(const Arm64Cpu *c)
{
	return c->el < 2 && arm64_el2_enabled(c) ? hpmn(c) : counters(c);
}

/* The bits of the enable, overflow and interrupt masks that it reaches. */
static uint32_t reachable(const Arm64Cpu *c)
{
	return (uint32_t)((1ull << accessible(c)) - 1) | CYCLE;
}

/* The PMUSERENR_EL0 bits that let EL0 make the access. */
static uint32_t el0_enables(Reg reg, bool read)
{
	switch (reg) {
	case PMSWINC:
		return USERENR_SW | USERENR_EN;
	case PMCCNTR:
		return read ? USERENR_CR | USERENR_EN : USERENR_EN;
	case PMEVCNTR:
	case PMXEVCNTR:
		return read ? USERENR_ER | USERENR_EN : USERENR_EN;
	case PMSELR:
		return USERENR_ER | USERENR_EN;
	default:
		return USERENR_EN;
	}
}

/* Where an access goes, besides the register itself, 0. */
enum { ACCESS_UNDEFINED = -1, ACCESS_STOP = -2 };

static int unpredictable(Arm64Cpu *c, const char *what, unsigned counter)
{
	snprintf(c->detail, sizeof(c->detail),
	         "event counter %u, past %s: CONSTRAINED UNPREDICTABLE", counter,
	         what);
	return ACCESS_STOP;
}

/*
 * The checks of the "Accessing" pseudocode: 0 for the register, 1 to 3
 * for a trap to that Exception level, ACCESS_UNDEFINED, or ACCESS_STOP
 * with c->detail.
 */
static int check(Arm64Cpu *c, Reg reg, unsigned m, bool read)
{
	bool el2 = arm64_el2_enabled(c);
	bool el3 = c->config.highest_el == 3;
	bool counter = true; /* whether the access reaches event counter m */

	if ((reg == PMSWINC && read) ||
	    ((reg == PMCEID0 || reg == PMCEID1) && !read))
		return ACCESS_UNDEFINED;
	if (c->el == 0 &&
	    ((reg == PMUSERENR && !read) || reg == PMINTENSET || reg == PMINTENCLR))
		return ACCESS_UNDEFINED;
	if (reg == PMXEVCNTR || (reg == PMXEVTYPER && c->pmu.selr != SEL_CYCLE))
		m = c->pmu.selr;
	else if (reg != PMEVCNTR && reg != PMEVTYPER)
		counter = false;
	if (counter && m >= counters(c))
		return unpredictable(c, "PMCR_EL0.N", m);

	switch (c->el) {
	case 0:
		if (reg != PMUSERENR && !(c->pmu.userenr & el0_enables(reg, read)))
			return (int)arm64_el1_target(c);
		/* fall through */
	case 1:
		if (el2 && (c->sys.mdcr_el2 & MDCR_TPM))
			return 2;
		if (el2 && reg == PMCR && (c->sys.mdcr_el2 & MDCR_TPMCR))
			return 2;
		if (el2 && counter && m >= hpmn(c))
			return unpredictable(c, "MDCR_EL2.HPMN", m);
		/* fall through */
	case 2:
		if (el3 && (c->sys.mdcr_el3 & MDCR_TPM))
			return 3;
		return 0;
	default:
		return 0;
	}
}

static uint64_t read_reg(const Arm64Cpu *c, Reg reg, unsigned m)
{
	const Arm64Pmu *p = &c->pmu;
	uint32_t value;

	switch (reg) {
	case PMCR:
		value = (uint32_t)arm64_model_value(c, ARM64_PMCR_EL0);
		value = (value & ~(PMCR_RW | PMCR_P | PMCR_C)) | p->pmcr;
		if (c->el < 2 && arm64_el2_enabled(c))
			value = (value & ~PMCR_N_MASK) | hpmn(c) << PMCR_N_SHIFT;
		return value;
	case PMCNTENSET:
	case PMCNTENCLR:
		return p->cnten & reachable(c);
	case PMOVSSET:
	case PMOVSCLR:
		return p->ovs & reachable(c);
	case PMINTENSET:
	case PMINTENCLR:
		return p->inten & reachable(c);
	case PMSELR:
		return p->selr;
	case PMCEID0:
	case PMCEID1:
		return arm64_model_value(c, reg == PMCEID0 ? ARM64_PMCEID0_EL0
		                                           : ARM64_PMCEID1_EL0);
	case PMCCNTR:
		return p->ccntr;
	case PMXEVTYPER:
		return p->selr == SEL_CYCLE ? p->ccfiltr : p->evtyper[p->selr];
	case PMXEVCNTR:
		return p->evcntr[p->selr];
	case PMUSERENR:
		return p->userenr;
	case PMCCFILTR:
		return p->ccfiltr;
	case PMEVCNTR:
		return p->evcntr[m];
	case PMEVTYPER:
		return p->evtyper[m];
	case PMSWINC:
		break;
	}
	return 0;
}

static void write_reg(Arm64Cpu *c, Reg reg, unsigned m, uint64_t value)
{
	Arm64Pmu *p = &c->pmu;
	uint32_t bits = (uint32_t)value & reachable(c);

	switch (reg) {
	case PMCR:
		p->pmcr = (uint32_t)value & PMCR_RW;
		if (value & PMCR_C)
			p->ccntr = 0;
		/*
		 * P resets the first range; the second too at EL2 and EL3, or
		 * when EL2 is not enabled.
		 */
		if (value & PMCR_P)
			for (unsigned i = 0; i < accessible(c); i++)
				p->evcntr[i] = 0;
		return;
	case PMCNTENSET:
		p->cnten |= bits;
		return;
	case PMCNTENCLR:
		p->cnten &= ~bits;
		return;
	case PMOVSSET:
		p->ovs |= bits;
		return;
	case PMOVSCLR:
		p->ovs &= ~bits;
		return;
	case PMINTENSET:
		p->inten |= bits;
		return;
	case PMINTENCLR:
		p->inten &= ~bits;
		return;
	case PMSWINC:
		return; /* it counts only for a counter that is on */
	case PMSELR:
		p->selr = (uint32_t)value & SEL_MASK;
		return;
	case PMCCNTR:
		p->ccntr = value;
		return;
	case PMXEVTYPER:
		if (p->selr == SEL_CYCLE)
			p->ccfiltr = (uint32_t)value & CCFILTR_MASK;
		else
			p->evtyper[p->selr] = (uint32_t)value & EVTYPER_MASK;
		return;
	case PMXEVCNTR:
		p->evcntr[p->selr] = (uint32_t)value;
		return;
	case PMUSERENR:
		p->userenr = (uint32_t)value & USERENR_MASK;
		return;
	case PMCCFILTR:
		p->ccfiltr = (uint32_t)value & CCFILTR_MASK;
		return;
	case PMEVCNTR:
		p->evcntr[m] = (uint32_t)value;
		return;
	case PMEVTYPER:
		p->evtyper[m] = (uint32_t)value & EVTYPER_MASK;
		return;
	case PMCEID0:
	case PMCEID1:
		return;
	}
}

/* Whether a counter is on, which would need counting: see the top. */
static bool counting(const Arm64Cpu *c)
{
	const Arm64Pmu *p = &c->pmu;
	uint32_t events = p->cnten & ~CYCLE;
	uint32_t second = events & ~(uint32_t)((1ull << hpmn(c)) - 1);

	if (p->pmcr & PMCR_E)
		return p->cnten != 0;
	return (c->sys.mdcr_el2 & MDCR_HPME) && second != 0;
}

/* Stop if the PMU's controls ask for what is not emulated. */
static Arm64Exec check_state(Arm64Cpu *c)
{
	unsigned n = hpmn(c);

	if (!implemented(c))
		return ARM64_EXEC_OK;
	if (n == 0 || n > counters(c)) {
		snprintf(c->detail, sizeof(c->detail),
		         "MDCR_EL2.HPMN of %u, reserved: CONSTRAINED UNPREDICTABLE", n);
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	if (counting(c)) {
		snprintf(c->detail, sizeof(c->detail),
		         "counting with the Performance Monitors");
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	return ARM64_EXEC_OK;
}

bool arm64_pmu_owns(uint16_t encoding)
{
	Reg reg;
	unsigned m;

	return decode(encoding, &reg, &m);
}

const char *arm64_pmu_name(uint16_t encoding)
{
	Reg reg;
	unsigned m;

	if (!decode(encoding, &reg, &m))
		return NULL;
	if (reg == PMEVCNTR)
		return "PMEVCNTR<n>_EL0";
	if (reg == PMEVTYPER)
		return "PMEVTYPER<n>_EL0";
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++)
		if (regs[i].reg == reg)
			return regs[i].name;
	return NULL;
}

Arm64Exec arm64_pmu_access(Arm64Cpu *c, uint32_t insn)
{
	uint16_t encoding = (uint16_t)field32(insn, 20, 5);
	bool read = bit32(insn, 21);
	unsigned t = field32(insn, 4, 0);
	Reg reg;
	unsigned m;
	int where;

	if (!implemented(c) || !decode(encoding, &reg, &m))
		return ARM64_EXEC_UNDEFINED;
	where = check(c, reg, m, read);
	if (where == ACCESS_UNDEFINED)
		return ARM64_EXEC_UNDEFINED;
	if (where == ACCESS_STOP)
		return ARM64_EXEC_UNIMPLEMENTED;
	if (where > 0)
		return arm64_sys_trap(c, (unsigned)where, insn);
	if (read) {
		set_x(c, t, read_reg(c, reg, m));
		return ARM64_EXEC_OK;
	}
	write_reg(c, reg, m, get_x(c, t));
	return check_state(c);
}

Arm64Exec arm64_pmu_mdcr_el2_written(Arm64Cpu *c)
{
	return check_state(c);
}

/* As a debugger would: event counters past N do not exist. */
static bool direct(const Arm64Cpu *c, uint16_t encoding, Reg *reg, unsigned *m)
{
	if (!implemented(c) || !decode(encoding, reg, m))
		return false;
	if (*reg == PMXEVCNTR || (*reg == PMXEVTYPER && c->pmu.selr != SEL_CYCLE))
		return c->pmu.selr < counters(c);
	if (*reg == PMEVCNTR || *reg == PMEVTYPER)
		return *m < counters(c);
	return true;
}

bool arm64_pmu_read(Arm64Cpu *c, uint16_t encoding, uint64_t *value)
{
	Reg reg;
	unsigned m;

	if (!direct(c, encoding, &reg, &m) || reg == PMSWINC)
		return false;
	*value = read_reg(c, reg, m);
	return true;
}

bool arm64_pmu_write(Arm64Cpu *c, uint16_t encoding, uint64_t value)
{
	Reg reg;
	unsigned m;

	if (!direct(c, encoding, &reg, &m) || reg == PMCEID0 || reg == PMCEID1)
		return false;
	write_reg(c, reg, m, value);
	return true;
}

/*
 * A reset: PMCR_EL0 as the CPU model gives it. The other registers whose
 * reset value is UNKNOWN reset to 0 here, as PMUSERENR_EL0 and
 * PMCCFILTR_EL0 do on the Cortex-A53.
 */
void arm64_pmu_reset(Arm64Cpu *c)
{
	memset(&c->pmu, 0, sizeof(c->pmu));
	c->pmu.pmcr = (uint32_t)arm64_model_value(c, ARM64_PMCR_EL0) & PMCR_RW;
}
