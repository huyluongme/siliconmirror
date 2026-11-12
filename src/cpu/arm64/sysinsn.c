/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The System instructions of Armv8.0, SYS with op0 = 1 (C5.3 to C5.5):
 * cache maintenance (DC, IC), address translation (AT) and TLB
 * maintenance (TLBI). SYSL has no instruction in Armv8.0, so it is
 * UNDEFINED, like the encodings that hold no instruction.
 *
 * Caches are not modelled (see mmu.c), so DC and IC change nothing,
 * except DC ZVA, which writes zeros. DC and IC by virtual address still
 * translate it, and can fault. Every TLBI empties the TLBs of all the
 * PEs, so the address or ASID in Xt does not matter. For the TLBI
 * instructions without an operand, Rt other than 31 is CONSTRAINED
 * UNPREDICTABLE; this model behaves as if Rt were 31.
 */

#include "core/machine.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/bits.h"

typedef enum Kind {
	KIND_NOP,   /* DC by set/way, IC IALLU and IALLUIS: only traps */
	KIND_BY_VA, /* DC IVAC, CVAC, CVAU, CIVAC and IC IVAU */
	KIND_ZVA,   /* DC ZVA */
	KIND_AT,
	KIND_TLBI,
} Kind;

typedef struct SysInsn {
	uint16_t encoding; /* ARM64_SYSREG(1, op1, CRn, CRm, op2) */
	uint8_t kind;
	uint8_t min_el; /* UNDEFINED below this Exception level */
	/*
	 * For the EL2 translation regime: UNDEFINED at EL3 when EL2 is not
	 * enabled, as in Secure state.
	 */
	bool el2_regime;
	uint32_t el0;  /* the SCTLR_EL1 bit without which EL0 traps */
	uint32_t hcr;  /* the HCR_EL2 bit that traps EL0 and EL1 to EL2 */
	uint8_t param; /* KIND_BY_VA: Arm64Access; KIND_AT: AT_PARAM() */
} SysInsn;

/* An AT instruction: the Exception level, and whether it writes. */
#define AT_PARAM(el, write) ((el) | (write) << 2)

#define I(op1, crn, crm, op2) ARM64_SYSREG(1, op1, crn, crm, op2)
#define NOP(op1, crn, crm, op2, min, trap)                                     \
	{I(op1, crn, crm, op2), KIND_NOP, min, false, 0, trap, 0}
#define BY_VA(op1, crn, crm, op2, min, el0, trap, access)                      \
	{I(op1, crn, crm, op2), KIND_BY_VA, min, false, el0, trap, access}
#define AT(op1, crn, crm, op2, min, el2, at_el, write)                         \
	{I(op1, crn, crm, op2), KIND_AT, min, el2, 0, 0, AT_PARAM(at_el, write)}
#define ZVA(op1, crn, crm, op2, min, el0, trap)                                \
	{I(op1, crn, crm, op2), KIND_ZVA, min, false, el0, trap, 0}
#define TLBI(op1, crn, crm, op2, min, el2, trap)                               \
	{I(op1, crn, crm, op2), KIND_TLBI, min, el2, 0, trap, 0}

/* Short names for the table. */
#define UCI ARM64_SCTLR_UCI
#define DZE ARM64_SCTLR_DZE
#define TPC ARM64_HCR_TPC
#define TPU ARM64_HCR_TPU
#define TSW ARM64_HCR_TSW
#define TTLB ARM64_HCR_TTLB
#define TDZ ARM64_HCR_TDZ
#define INVALIDATE ARM64_ACCESS_DC_INVALIDATE
#define CLEAN ARM64_ACCESS_DC_CLEAN
#define IC ARM64_ACCESS_IC

/* Sorted by encoding, for the binary search in find(). */
static const SysInsn insns[] = {
	NOP(0, 7, 1, 0, 1, TPU),                  /* IC IALLUIS */
	NOP(0, 7, 5, 0, 1, TPU),                  /* IC IALLU */
	BY_VA(0, 7, 6, 1, 1, 0, TPC, INVALIDATE), /* DC IVAC */
	NOP(0, 7, 6, 2, 1, TSW),                  /* DC ISW */
	AT(0, 7, 8, 0, 1, false, 1, 0),           /* AT S1E1R */
	AT(0, 7, 8, 1, 1, false, 1, 1),           /* AT S1E1W */
	AT(0, 7, 8, 2, 1, false, 0, 0),           /* AT S1E0R */
	AT(0, 7, 8, 3, 1, false, 0, 1),           /* AT S1E0W */
	NOP(0, 7, 10, 2, 1, TSW),                 /* DC CSW */
	NOP(0, 7, 14, 2, 1, TSW),                 /* DC CISW */
	TLBI(0, 8, 3, 0, 1, false, TTLB),         /* TLBI VMALLE1IS */
	TLBI(0, 8, 3, 1, 1, false, TTLB),         /* TLBI VAE1IS */
	TLBI(0, 8, 3, 2, 1, false, TTLB),         /* TLBI ASIDE1IS */
	TLBI(0, 8, 3, 3, 1, false, TTLB),         /* TLBI VAAE1IS */
	TLBI(0, 8, 3, 5, 1, false, TTLB),         /* TLBI VALE1IS */
	TLBI(0, 8, 3, 7, 1, false, TTLB),         /* TLBI VAALE1IS */
	TLBI(0, 8, 7, 0, 1, false, TTLB),         /* TLBI VMALLE1 */
	TLBI(0, 8, 7, 1, 1, false, TTLB),         /* TLBI VAE1 */
	TLBI(0, 8, 7, 2, 1, false, TTLB),         /* TLBI ASIDE1 */
	TLBI(0, 8, 7, 3, 1, false, TTLB),         /* TLBI VAAE1 */
	TLBI(0, 8, 7, 5, 1, false, TTLB),         /* TLBI VALE1 */
	TLBI(0, 8, 7, 7, 1, false, TTLB),         /* TLBI VAALE1 */
	ZVA(3, 7, 4, 1, 0, DZE, TDZ),             /* DC ZVA */
	BY_VA(3, 7, 5, 1, 0, UCI, TPU, IC),       /* IC IVAU */
	BY_VA(3, 7, 10, 1, 0, UCI, TPC, CLEAN),   /* DC CVAC */
	BY_VA(3, 7, 11, 1, 0, UCI, TPU, CLEAN),   /* DC CVAU */
	BY_VA(3, 7, 14, 1, 0, UCI, TPC, CLEAN),   /* DC CIVAC */
	AT(4, 7, 8, 0, 2, true, 2, 0),            /* AT S1E2R */
	AT(4, 7, 8, 1, 2, true, 2, 1),            /* AT S1E2W */
	AT(4, 7, 8, 4, 2, false, 1, 0),           /* AT S12E1R */
	AT(4, 7, 8, 5, 2, false, 1, 1),           /* AT S12E1W */
	AT(4, 7, 8, 6, 2, false, 0, 0),           /* AT S12E0R */
	AT(4, 7, 8, 7, 2, false, 0, 1),           /* AT S12E0W */
	TLBI(4, 8, 0, 1, 2, false, 0),            /* TLBI IPAS2E1IS */
	TLBI(4, 8, 0, 5, 2, false, 0),            /* TLBI IPAS2LE1IS */
	TLBI(4, 8, 3, 0, 2, true, 0),             /* TLBI ALLE2IS */
	TLBI(4, 8, 3, 1, 2, true, 0),             /* TLBI VAE2IS */
	TLBI(4, 8, 3, 4, 2, false, 0),            /* TLBI ALLE1IS */
	TLBI(4, 8, 3, 5, 2, true, 0),             /* TLBI VALE2IS */
	TLBI(4, 8, 3, 6, 2, false, 0),            /* TLBI VMALLS12E1IS */
	TLBI(4, 8, 4, 1, 2, false, 0),            /* TLBI IPAS2E1 */
	TLBI(4, 8, 4, 5, 2, false, 0),            /* TLBI IPAS2LE1 */
	TLBI(4, 8, 7, 0, 2, true, 0),             /* TLBI ALLE2 */
	TLBI(4, 8, 7, 1, 2, true, 0),             /* TLBI VAE2 */
	TLBI(4, 8, 7, 4, 2, false, 0),            /* TLBI ALLE1 */
	TLBI(4, 8, 7, 5, 2, true, 0),             /* TLBI VALE2 */
	TLBI(4, 8, 7, 6, 2, false, 0),            /* TLBI VMALLS12E1 */
	AT(6, 7, 8, 0, 3, false, 3, 0),           /* AT S1E3R */
	AT(6, 7, 8, 1, 3, false, 3, 1),           /* AT S1E3W */
	TLBI(6, 8, 3, 0, 3, false, 0),            /* TLBI ALLE3IS */
	TLBI(6, 8, 3, 1, 3, false, 0),            /* TLBI VAE3IS */
	TLBI(6, 8, 3, 5, 3, false, 0),            /* TLBI VALE3IS */
	TLBI(6, 8, 7, 0, 3, false, 0),            /* TLBI ALLE3 */
	TLBI(6, 8, 7, 1, 3, false, 0),            /* TLBI VAE3 */
	TLBI(6, 8, 7, 5, 3, false, 0),            /* TLBI VALE3 */
};

static const SysInsn *find(uint16_t encoding)
{
	size_t lo = 0, hi = sizeof(insns) / sizeof(insns[0]);

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (insns[mid].encoding == encoding)
			return &insns[mid];
		if (insns[mid].encoding < encoding)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

bool arm64_sys_table_is_sorted(void)
{
	for (size_t i = 1; i < sizeof(insns) / sizeof(insns[0]); i++)
		if (insns[i - 1].encoding >= insns[i].encoding)
			return false;
	return true;
}

/*
 * Where the instruction traps to, or 0. At EL0, the SCTLR_EL1 bit that
 * enables it comes first (AArch64_SystemAccessTrapToEL1orEL2(), which
 * HCR_EL2.TGE sends to EL2), then the HCR_EL2 bit.
 */
static unsigned trap_target(const Arm64Cpu *c, const SysInsn *s)
{
	if (c->el == 0 && !(c->sys.sctlr[1] & s->el0))
		return arm64_el1_target(c);
	if (c->el <= 1 && arm64_el2_enabled(c) && (c->sys.hcr_el2 & s->hcr))
		return 2;
	return 0;
}

/*
 * DC and IC by virtual address: AArch64_DC() and AArch64_IC() translate
 * va, which can fault; there is no cache to maintain.
 */
static Arm64Exec by_va(Arm64Cpu *c, uint64_t va, Arm64Access access)
{
	Arm64Translation t;
	Arm64Exec result = arm64_translate(c, va, access, c->el, true, &t);

	if (result != ARM64_EXEC_OK)
		return result;
	if (t.fault != ARM64_FAULT_NONE)
		return arm64_translation_fault(c, va, access, &t);
	return ARM64_EXEC_OK;
}

/*
 * DC ZVA, C5.3.38: AArch64_DataMemZero(), J1.2.3.20, writes zeros to the
 * block of DCZID_EL0.BS words that holds xt. A fault reports xt in
 * FAR_ELx. The block is at most 2KB, so it is in one page.
 */
static Arm64Exec zero_block(Arm64Cpu *c, uint64_t xt)
{
	uint64_t dczid = 0;
	Arm64Translation t;
	Arm64Exec result;

	arm64_cpu_read_sysreg(c, ARM64_DCZID_EL0, &dczid);

	uint64_t size = UINT64_C(4) << (dczid & 0xf);
	unsigned chunk = size < 8 ? (unsigned)size : 8;

	result = arm64_translate(c, xt & ~(size - 1), ARM64_ACCESS_DC_ZVA, c->el,
	                         true, &t);
	if (result != ARM64_EXEC_OK)
		return result;
	if (t.fault != ARM64_FAULT_NONE)
		return arm64_translation_fault(c, xt, ARM64_ACCESS_DC_ZVA, &t);
	for (uint64_t offset = 0; offset < size; offset += chunk) {
		BusStatus status =
			bus_write(c->cpu.machine->bus, t.pa + offset, chunk, 0);

		if (status != BUS_OK)
			return arm64_bus_failure(c, t.pa + offset, chunk, true, status,
			                         false, (xt & ~(size - 1)) + offset);
	}
	return ARM64_EXEC_OK;
}

Arm64Exec arm64_exec_sys(Arm64Cpu *c, uint32_t insn)
{
	const SysInsn *s = find((uint16_t)field32(insn, 20, 5));
	unsigned target;

	if (bit32(insn, 21) || s == NULL || c->el < s->min_el) /* SYSL */
		return ARM64_EXEC_UNDEFINED;
	if (s->el2_regime && c->el == 3 && !arm64_el2_enabled(c))
		return ARM64_EXEC_UNDEFINED;
	target = trap_target(c, s);
	if (target != 0)
		return arm64_sys_trap(c, target, insn);

	uint64_t xt = get_x(c, field32(insn, 4, 0));

	switch ((Kind)s->kind) {
	case KIND_NOP:
		return ARM64_EXEC_OK;
	case KIND_BY_VA:
		return by_va(c, xt, (Arm64Access)s->param);
	case KIND_ZVA:
		return zero_block(c, xt);
	case KIND_AT:
		return arm64_at(c, xt, s->param & 3, (s->param >> 2) & 1);
	case KIND_TLBI:
		arm64_tlb_flush_all_pes(c);
		return ARM64_EXEC_OK;
	}
	return ARM64_EXEC_UNDEFINED;
}
