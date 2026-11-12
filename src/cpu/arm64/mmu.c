/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The MMU of the AArch64 model: stage 1 address translation of Armv8.0
 * for the EL1&0, EL2 and EL3 translation regimes (D8), after
 * AArch64_S1Translate() in J1.2.4.57 and the functions that it calls,
 * and a TLB that holds its results.
 *
 * Translation follows VMSAv8-64 as Armv8.0 has it: the 4KB and 64KB
 * granules when ID_AA64MMFR0_EL1 says the core has them, input
 * addresses of 25 to 48 bits, output addresses as large as
 * ID_AA64MMFR0_EL1.PARange allows, hierarchical permissions, and no
 * hardware update of the Access flag: a descriptor with AF == 0 gives an
 * Access flag fault. Stage 2 translation is not implemented yet: a
 * translation for EL1&0 that would need it, with HCR_EL2.VM or
 * HCR_EL2.DC set, stops the machine.
 *
 * The manual leaves some choices to the implementation, and the
 * Cortex-A53 TRM does not say which ones the core makes. Where the
 * choice changes what a translation table means, the machine stops
 * instead of guessing:
 *  - TCR_ELx.TGn selects a granule that the core does not have, or a
 *    reserved value (AArch64_S1DecodeTG0());
 *  - TCR_ELx.TnSZ is outside 16 to 39 (AArch64_S1TxSZFaults()).
 * Elsewhere this model makes these choices:
 *  - DC and IC instructions get Access flag faults, like data accesses;
 *  - an instruction fetch from Device memory does not fault;
 *  - IC IVAU at EL0 does not need read permission;
 *  - with the 64KB granule, descriptor bits 15:12 are not part of the
 *    output address: the core has no FEAT_LPA;
 *  - PAR_EL1.NS is 1 after a translation of a Non-secure regime, where
 *    the Cortex-A53 TRM says that it is UNKNOWN.
 *
 * The TLB holds successful translations, in entries for 4KB pages of
 * virtual addresses tagged with the translation regime. Every TLBI
 * instruction empties the TLBs of all the PEs, which R_MMZTY in D8.17
 * allows: an implementation may invalidate more entries than it must.
 * Writes of the registers that control translation empty the TLB of the
 * PE too: SCTLR_ELx, TCR_ELx, TTBRn_ELx, MAIR_ELx, HCR_EL2 and SCR_EL3.
 * So an entry always belongs to the registers of the moment, and the TLB
 * needs no ASID. Software that changes a translation table without the
 * TLB maintenance that the architecture asks for can still see the old
 * translation, as on a real core.
 *
 * Caches are not modelled: memory is always coherent, whatever the
 * memory attributes, and the cache maintenance instructions only check
 * their address. Software that leaves out cache maintenance works here
 * but can fail on the board.
 *
 * Descriptors are read little-endian: big-endian, SCTLR_ELx.EE, is not
 * implemented. The board has one physical address space for Secure and
 * Non-secure accesses, so the NS attribute only changes what PAR_EL1
 * reports, and what SCR_EL3.SIF allows. A translation table walk that
 * reads where the machine emulates nothing stops the machine, like a
 * data access there.
 */

#include <stdio.h>
#include <string.h>

#include "core/machine.h"
#include "cpu/arm64/internal.h"

/* Permissions of a translation, in Arm64TlbEntry.perms. */
enum {
	PERM_PR = 1 << 0, /* read, from EL1 and above */
	PERM_PW = 1 << 1, /* write */
	PERM_PX = 1 << 2, /* execute */
	PERM_UR = 1 << 3, /* read, from EL0 */
	PERM_UW = 1 << 4,
	PERM_UX = 1 << 5,
};

/* TCR_ELx fields, D24.2. */
#define TCR_EPD0 (UINT64_C(1) << 7)
#define TCR_EPD1 (UINT64_C(1) << 23)
#define TCR_TBI (UINT64_C(1) << 20) /* TCR_EL2 and TCR_EL3 */
#define TCR_TBI0 (UINT64_C(1) << 37)
#define TCR_TBI1 (UINT64_C(1) << 38)

/* Descriptor fields, D8.3. */
#define DESC_VALID (UINT64_C(1) << 0)
#define DESC_TABLE (UINT64_C(1) << 1) /* a page at level 3 */
#define DESC_NS (UINT64_C(1) << 5)
#define DESC_AP1 (UINT64_C(1) << 6) /* AP[1]: EL0 can access */
#define DESC_AP2 (UINT64_C(1) << 7) /* AP[2]: read-only */
#define DESC_AF (UINT64_C(1) << 10)
#define DESC_PXN (UINT64_C(1) << 53)
#define DESC_UXN (UINT64_C(1) << 54) /* XN in the EL2 and EL3 regimes */
#define TABLE_PXN (UINT64_C(1) << 59)
#define TABLE_UXN (UINT64_C(1) << 60) /* XNTable in EL2 and EL3 */
#define TABLE_AP0 (UINT64_C(1) << 61) /* APTable[0]: no EL0 access */
#define TABLE_AP1 (UINT64_C(1) << 62) /* APTable[1]: no write */
#define TABLE_NS (UINT64_C(1) << 63)

/*
 * Without FEAT_LPA, the addresses in translation table descriptors and
 * TTBRn_ELx have 48 bits at most.
 */
#define ADDRESS_MASK ((UINT64_C(1) << 48) - 1)

#define PAGE_MASK UINT64_C(0xfff)
#define TOP_BYTE UINT64_C(0xff00000000000000)

/* Bits lo to hi of a 64-bit value set, the others clear. */
static uint64_t bits_mask(unsigned hi, unsigned lo)
{
	uint64_t below_hi = hi == 63 ? UINT64_MAX : (UINT64_C(1) << (hi + 1)) - 1;

	return below_hi & ~((UINT64_C(1) << lo) - 1);
}

/* The parameters of a translation table walk, S1TTWParams in J1. */
typedef struct Walk {
	bool unprivileged;     /* the regime has EL0: EL1&0 */
	bool secure;           /* the regime is in Secure state */
	bool sif;              /* SCR_EL3.SIF applies */
	bool wxn;              /* SCTLR_ELx.WXN */
	uint64_t ttbr;         /* TTBRn_ELx for the address */
	uint64_t mair;         /* MAIR_ELx */
	unsigned txsz;         /* TnSZ for the address */
	unsigned granule_bits; /* 12 for 4KB, 16 for 64KB */
	unsigned oa_bits;      /* the size of output addresses */
} Walk;

/* TranslationRegime() for an Exception level, without EL2&0. */
static unsigned regime_of(unsigned el)
{
	return el > 1 ? el : 1;
}

/*
 * SecurityStateForRegime(): EL3 is Secure, EL2 Non-secure, and EL1&0 as
 * SCR_EL3.NS says, or Non-secure without EL3.
 */
static bool regime_secure(const Arm64Cpu *c, unsigned regime)
{
	if (regime != 1)
		return regime == 3;
	return c->config.highest_el == 3 && !(c->sys.scr_el3 & ARM64_SCR_NS);
}

/*
 * AArch64_S1Enabled(), J1.2.4.99: SCTLR_ELx.M, which HCR_EL2.TGE
 * overrides for EL1&0. HCR_EL2.DC would too, but needs stage 2.
 */
static bool s1_enabled(const Arm64Cpu *c, unsigned regime)
{
	if (!(c->sys.sctlr[regime] & ARM64_SCTLR_M))
		return false;
	return regime != 1 || !arm64_el2_enabled(c) ||
	       !(c->sys.hcr_el2 & ARM64_HCR_TGE);
}

/*
 * Whether TCR_ELx.TBI makes the MMU ignore the top byte of va:
 * AArch64_AddrTop() is then 55. For EL1&0, bit 55 picks TBI0 or TBI1.
 */
static bool tbi(const Arm64Cpu *c, unsigned regime, uint64_t va)
{
	if (regime == 1)
		return c->sys.tcr[1] & ((va >> 55) & 1 ? TCR_TBI1 : TCR_TBI0);
	return c->sys.tcr[regime] & TCR_TBI;
}

/*
 * va without its tag: bit 55 copied into the top byte for EL1&0, zeros
 * there for EL2 and EL3.
 */
static uint64_t untag(unsigned regime, uint64_t va)
{
	if (regime == 1 && ((va >> 55) & 1))
		return va | TOP_BYTE;
	return va & ~TOP_BYTE;
}

uint64_t arm64_branch_addr(const Arm64Cpu *c, uint64_t target)
{
	unsigned regime = regime_of(c->el);

	return tbi(c, regime, target) ? untag(regime, target) : target;
}

void arm64_tlb_flush(Arm64Cpu *c)
{
	memset(c->tlb, 0, sizeof(c->tlb));
}

static unsigned tlb_index(uint64_t key)
{
	return (unsigned)(key >> 12) & (ARM64_TLB_ENTRIES - 1);
}

static Arm64Exec fault(Arm64Translation *t, Arm64Fault kind, unsigned level)
{
	t->fault = kind;
	t->level = level;
	return ARM64_EXEC_OK;
}

/*
 * The Device memory types fault on an unaligned access, and on DC ZVA:
 * AArch64_S1HasAlignmentFaultDueToMemType(), J1.2.4.30, where this model
 * makes no Device location support unaligned accesses.
 */
static bool device_alignment_fault(uint8_t attr, Arm64Access access,
                                   bool aligned)
{
	bool device = (attr & 0xf0) == 0;

	return device && (access == ARM64_ACCESS_DC_ZVA || !aligned);
}

/*
 * With stage 1 translation disabled, AArch64_S1DisabledOutput(),
 * J1.2.4.56: the physical address is the virtual address, which must
 * fit in PAMax bits once TBI has removed the tag. Data accesses use the
 * Device-nGnRnE memory type, instruction fetches Normal memory.
 */
static Arm64Exec translate_disabled(Arm64Cpu *c, uint64_t va,
                                    Arm64Access access, unsigned regime,
                                    bool aligned, Arm64Translation *t)
{
	unsigned top = tbi(c, regime, va) ? 55 : 63;

	if (va & bits_mask(top, c->pa_bits))
		return fault(t, ARM64_FAULT_ADDRESS_SIZE, 0);
	t->pa = va & ((UINT64_C(1) << c->pa_bits) - 1);
	t->attr = access == ARM64_ACCESS_IFETCH ? 0x44 : 0x00;
	t->sh = 2; /* Outer Shareable */
	t->ns = !regime_secure(c, regime);
	if (device_alignment_fault(t->attr, access, aligned))
		return fault(t, ARM64_FAULT_ALIGNMENT, 0);
	return ARM64_EXEC_OK;
}

/*
 * The output address size: TCR_ELx.{I}PS, at most PAMax
 * (AArch64_PhysicalAddressSize()). Without FEAT_LPA, the reserved
 * values 110 and 111 give the largest size.
 */
static unsigned oa_bits(const Arm64Cpu *c, unsigned ps)
{
	static const unsigned sizes[8] = {32, 36, 40, 42, 44, 48, 48, 48};
	unsigned bits = sizes[ps & 7];

	return bits < c->pa_bits ? bits : c->pa_bits;
}

/*
 * The walk parameters for va: AArch64_S1TTWParamsEL10(), EL2() and
 * EL3(), J1.2.4.105 to 108. *disabled is set when TCR_EL1.EPDn turns off
 * walks for va's half of the address space. Returns
 * ARM64_EXEC_UNIMPLEMENTED for a granule or size that the model cannot
 * use.
 */
static Arm64Exec walk_params(Arm64Cpu *c, unsigned regime, uint64_t va, Walk *w,
                             bool *disabled)
{
	uint64_t tcr = c->sys.tcr[regime];
	bool upper = regime == 1 && ((va >> 55) & 1);
	unsigned kb; /* the granule, in KB; 0 when reserved */

	w->unprivileged = regime == 1;
	w->secure = regime_secure(c, regime);
	w->sif = w->secure && (c->sys.scr_el3 & ARM64_SCR_SIF);
	w->wxn = c->sys.sctlr[regime] & ARM64_SCTLR_WXN;
	w->mair = c->sys.mair[regime];
	if (upper) {
		static const unsigned tg1[4] = {0, 16, 4, 64};

		w->ttbr = c->sys.ttbr1_el1;
		w->txsz = (tcr >> 16) & 0x3f;
		kb = tg1[(tcr >> 30) & 3];
		*disabled = tcr & TCR_EPD1;
	} else {
		static const unsigned tg0[4] = {4, 64, 16, 0};

		w->ttbr = c->sys.ttbr0[regime];
		w->txsz = tcr & 0x3f;
		kb = tg0[(tcr >> 14) & 3];
		*disabled = regime == 1 && (tcr & TCR_EPD0);
	}
	w->oa_bits = oa_bits(c, (unsigned)(tcr >> (regime == 1 ? 32 : 16)) & 7);

	if (kb == 16 && c->granule_16k) {
		snprintf(c->detail, sizeof(c->detail), "the 16KB translation granule");
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	if (!(kb == 4 && c->granule_4k) && !(kb == 64 && c->granule_64k)) {
		snprintf(c->detail, sizeof(c->detail),
		         "TCR_EL%u.TG%u = %u, a granule that the core does not have",
		         regime, upper ? 1 : 0,
		         (unsigned)(tcr >> (upper ? 30 : 14)) & 3);
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	if (w->txsz < 16 || w->txsz > 39) {
		snprintf(c->detail, sizeof(c->detail),
		         "TCR_EL%u.T%uSZ = %u, outside 16 to 39", regime, upper ? 1 : 0,
		         w->txsz);
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	w->granule_bits = kb == 4 ? 12 : 16;
	return ARM64_EXEC_OK;
}

/*
 * AArch64_VAIsOutOfRange(), J1.2.4.46: the bits of va above the input
 * address size, up to bit 55 with TBI or 63 without, must all be 0, or
 * all 1 in the upper half of EL1&0.
 */
static bool va_out_of_range(const Walk *w, uint64_t va, bool tagged)
{
	uint64_t mask = bits_mask(tagged ? 55 : 63, 64 - w->txsz);
	bool upper = w->unprivileged && ((va >> 55) & 1);

	return (va & mask) != (upper ? mask : 0);
}

/* AArch64_OAOutOfRange(), J1.2.4.24, for an address of 48 bits at most. */
static bool oa_out_of_range(const Walk *w, uint64_t address)
{
	return (address >> w->oa_bits) != 0;
}

/* AArch64_BlockDescSupported(), J1.2.4.67, without FEAT_LPA2. */
static bool block_allowed(const Walk *w, unsigned level)
{
	if (w->granule_bits == 12)
		return level == 1 || level == 2;
	return level == 2;
}

/*
 * AArch64_S1DirectBasePermissions(), J1.2.4.29, for Armv8.0: AP[2:1]
 * with APTable, XN with the XNTable bits, then SCTLR_ELx.WXN and
 * SCR_EL3.SIF. A region that EL0 can write is never executable at EL1.
 * table holds the bits of the table descriptors ORed together.
 */
static uint8_t permissions(const Walk *w, uint64_t desc, uint64_t table,
                           bool ns)
{
	bool pw, px, ur = false, uw = false, ux = false; /* EL1 can read */

	if (w->unprivileged) {
		ur = desc & DESC_AP1;
		pw = !(desc & DESC_AP2);
		uw = ur && pw;
		if (table & TABLE_AP0)
			ur = uw = false;
		if (table & TABLE_AP1)
			pw = uw = false;
		px = !(desc & DESC_PXN) && !(table & TABLE_PXN) && !uw;
		ux = !(desc & DESC_UXN) && !(table & TABLE_UXN);
	} else {
		pw = !(desc & DESC_AP2) && !(table & TABLE_AP1);
		px = !(desc & DESC_UXN) && !(table & TABLE_UXN);
	}
	if (w->wxn) {
		px = px && !pw;
		ux = ux && !uw;
	}
	if (w->sif && ns)
		px = ux = false;
	return (uint8_t)(PERM_PR | (pw ? PERM_PW : 0) | (px ? PERM_PX : 0) |
	                 (ur ? PERM_UR : 0) | (uw ? PERM_UW : 0) |
	                 (ux ? PERM_UX : 0));
}

/*
 * Walk the translation tables for va: AArch64_S1Walk(), J1.2.4.77. On
 * success, *e holds the translation of va's 4KB page.
 */
static Arm64Exec walk(Arm64Cpu *c, const Walk *w, uint64_t va, Arm64TlbEntry *e,
                      Arm64Translation *t)
{
	unsigned g = w->granule_bits;
	unsigned stride = g - 3; /* index bits per level */
	unsigned iasize = 64 - w->txsz;
	/* AArch64_S1StartLevel(), and the index bits of that level. */
	unsigned level = 3 - (iasize - 1 - g) / stride;
	unsigned index_bits = iasize - g - (3 - level) * stride;
	/* AArch64_S1TTBaseAddress(): aligned to the size of the table. */
	uint64_t table = w->ttbr & ADDRESS_MASK & ~bits_mask(index_bits + 2, 0);
	uint64_t table_bits = 0; /* table descriptors ORed together */

	if (oa_out_of_range(w, table))
		return fault(t, ARM64_FAULT_ADDRESS_SIZE, 0);
	for (;;) {
		unsigned shift = g + (3 - level) * stride;
		uint64_t index = (va >> shift) & bits_mask(index_bits - 1, 0);
		uint64_t address = table + index * 8;
		uint64_t desc;
		BusStatus status = bus_read(c->cpu.machine->bus, address, 8, &desc);

		if (status != BUS_OK)
			return arm64_bus_failure(c, address, 8, false, status, true, va);
		if (!(desc & DESC_VALID))
			return fault(t, ARM64_FAULT_TRANSLATION, level);
		if (level < 3 && (desc & DESC_TABLE)) {
			table = desc & ADDRESS_MASK & ~bits_mask(g - 1, 0);
			if (oa_out_of_range(w, table))
				return fault(t, ARM64_FAULT_ADDRESS_SIZE, level);
			table_bits |= desc;
			level++;
			index_bits = stride;
			continue;
		}
		if (level == 3 ? !(desc & DESC_TABLE) : !block_allowed(w, level))
			return fault(t, ARM64_FAULT_TRANSLATION, level);

		/* A block or page: AArch64_S1NextWalkStateLeaf(). */
		uint64_t oa = desc & ADDRESS_MASK & ~bits_mask(shift - 1, 0);
		/* In Secure state, NSTable makes the rest Non-secure. */
		bool ns = !w->secure || (table_bits & TABLE_NS) || (desc & DESC_NS);

		if (oa_out_of_range(w, oa))
			return fault(t, ARM64_FAULT_ADDRESS_SIZE, level);
		if (!(desc & DESC_AF))
			return fault(t, ARM64_FAULT_ACCESS_FLAG, level);
		e->pa = oa | (va & bits_mask(shift - 1, 12));
		e->attr = (uint8_t)(w->mair >> (8 * ((desc >> 2) & 7)));
		e->sh = (uint8_t)((desc >> 8) & 3);
		e->level = (uint8_t)level;
		e->ns = ns;
		e->perms = permissions(w, desc, table_bits, ns);
		return ARM64_EXEC_OK;
	}
}

/*
 * Translate va with stage 1 enabled, through the TLB. key is va without
 * its tag when TBI applies, or va.
 */
static Arm64Exec translate_enabled(Arm64Cpu *c, uint64_t va, unsigned regime,
                                   uint64_t key, bool tagged,
                                   Arm64TlbEntry **entry, Arm64Translation *t)
{
	uint64_t tag = (key & ~PAGE_MASK) | regime;
	Arm64TlbEntry *e = &c->tlb[tlb_index(key)];
	Arm64TlbEntry walked;
	Walk w;
	bool disabled;
	Arm64Exec result;

	*entry = e;
	if (e->tag == tag)
		return ARM64_EXEC_OK;
	result = walk_params(c, regime, va, &w, &disabled);
	if (result != ARM64_EXEC_OK)
		return result;
	if (va_out_of_range(&w, va, tagged) || disabled)
		return fault(t, ARM64_FAULT_TRANSLATION, 0);
	result = walk(c, &w, va, &walked, t);
	if (result != ARM64_EXEC_OK || t->fault != ARM64_FAULT_NONE)
		return result;
	walked.tag = tag;
	*e = walked;
	return ARM64_EXEC_OK;
}

/*
 * AArch64_S1CheckPermissions(), J1.2.4.26, for Armv8.0: the permission
 * that an access needs from el. DC CVAC, CVAU and CIVAC need read
 * permission at EL0 only; IC IVAU needs none.
 */
static bool permitted(uint8_t perms, Arm64Access access, unsigned el)
{
	unsigned shift = el == 0 ? 3 : 0; /* PERM_U* are PERM_P* << 3 */
	unsigned r = (perms >> shift) & PERM_PR;
	unsigned w = (perms >> shift) & PERM_PW;
	unsigned x = (perms >> shift) & PERM_PX;

	switch (access) {
	case ARM64_ACCESS_READ:
		return r;
	case ARM64_ACCESS_WRITE:
	case ARM64_ACCESS_DC_ZVA:
	case ARM64_ACCESS_DC_INVALIDATE:
		return w;
	case ARM64_ACCESS_IFETCH:
		return x;
	case ARM64_ACCESS_DC_CLEAN:
		return el != 0 || r;
	case ARM64_ACCESS_IC:
		return true;
	}
	return false;
}

Arm64Exec arm64_translate(Arm64Cpu *c, uint64_t va, Arm64Access access,
                          unsigned el, bool aligned, Arm64Translation *t)
{
	unsigned regime = regime_of(el);
	Arm64TlbEntry *e;
	Arm64Exec result;

	t->fault = ARM64_FAULT_NONE;
	t->level = 0;
	if (regime == 1 && arm64_el2_enabled(c) &&
	    (c->sys.hcr_el2 & (ARM64_HCR_VM | ARM64_HCR_DC))) {
		snprintf(c->detail, sizeof(c->detail),
		         "stage 2 translation, for HCR_EL2.%s",
		         c->sys.hcr_el2 & ARM64_HCR_VM ? "VM" : "DC");
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	if (!s1_enabled(c, regime))
		return translate_disabled(c, va, access, regime, aligned, t);

	bool tagged = tbi(c, regime, va);

	result = translate_enabled(c, va, regime, tagged ? untag(regime, va) : va,
	                           tagged, &e, t);
	if (result != ARM64_EXEC_OK || t->fault != ARM64_FAULT_NONE)
		return result;
	t->pa = e->pa | (va & PAGE_MASK);
	t->attr = e->attr;
	t->sh = e->sh;
	t->ns = e->ns;
	if (device_alignment_fault(e->attr, access, aligned))
		return fault(t, ARM64_FAULT_ALIGNMENT, 0);
	if (!permitted(e->perms, access, el))
		return fault(t, ARM64_FAULT_PERMISSION, e->level);
	return ARM64_EXEC_OK;
}

/*
 * The fault status code of ESR_ELx.ISS.DFSC and IFSC, and of
 * PAR_EL1.FST: EncodeLDFSC() with the level of the fault.
 */
static uint32_t fault_status(const Arm64Translation *t)
{
	switch (t->fault) {
	case ARM64_FAULT_ADDRESS_SIZE:
		return 0x00 | t->level;
	case ARM64_FAULT_TRANSLATION:
		return 0x04 | t->level;
	case ARM64_FAULT_ACCESS_FLAG:
		return 0x08 | t->level;
	case ARM64_FAULT_PERMISSION:
		return 0x0c | t->level;
	default:
		return ARM64_DFSC_ALIGNMENT;
	}
}

/*
 * AArch64_FaultSyndrome(), J1.2.3.1: the faults of cache maintenance
 * instructions set CM, ISS bit 8, and WnR, bit 6; DC ZVA is a write.
 */
Arm64Exec arm64_translation_fault(Arm64Cpu *c, uint64_t far, Arm64Access access,
                                  const Arm64Translation *t)
{
	uint32_t iss = fault_status(t);

	switch (access) {
	case ARM64_ACCESS_DC_CLEAN:
	case ARM64_ACCESS_DC_INVALIDATE:
	case ARM64_ACCESS_IC:
		iss |= 1u << 8 | 1u << 6;
		break;
	case ARM64_ACCESS_WRITE:
	case ARM64_ACCESS_DC_ZVA:
		iss |= 1u << 6;
		break;
	default:
		break;
	}
	return arm64_abort(c, access == ARM64_ACCESS_IFETCH, iss, far);
}

/*
 * AArch64_AT(), J1.2.3.10, and AArch64_EncodePAR(), J1.2.3.11, with the
 * PAR_EL1 layout of the Cortex-A53 TRM: bit 11 is RES1, and the other
 * bits that the TRM does not name are 0. ATTR holds the attribute from
 * MAIR_ELx, with bits 1:0 clear for Device memory. SH reports Outer
 * Shareable for Device memory and for Normal Non-cacheable memory.
 */
Arm64Exec arm64_at(Arm64Cpu *c, uint64_t va, unsigned el, bool write)
{
	Arm64Translation t;
	Arm64Exec result = arm64_translate(
		c, va, write ? ARM64_ACCESS_WRITE : ARM64_ACCESS_READ, el, true, &t);

	if (result != ARM64_EXEC_OK)
		return result;
	if (t.fault != ARM64_FAULT_NONE) {
		c->sys.par_el1 = UINT64_C(1) << 11 | fault_status(&t) << 1 | 1;
		return ARM64_EXEC_OK;
	}

	bool device = (t.attr & 0xf0) == 0;
	uint64_t attr = device ? t.attr & 0x0c : t.attr;
	unsigned sh = device || t.attr == 0x44 ? 2 : t.sh;

	c->sys.par_el1 = attr << 56 | (t.pa & ADDRESS_MASK & ~PAGE_MASK) |
	                 UINT64_C(1) << 11 | (t.ns ? UINT64_C(1) << 9 : 0) |
	                 (uint64_t)sh << 7;
	return ARM64_EXEC_OK;
}
