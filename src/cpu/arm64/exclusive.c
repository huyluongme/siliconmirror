/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The Load/store exclusive class of C4.1, as Armv8.0 has it: the
 * Load-Exclusive and Store-Exclusive instructions LDXR, LDAXR, STXR,
 * STLXR and their pairs LDXP, LDAXP, STXP, STLXP, and the ordered LDAR and
 * STLR, each after its description in C6.2. The other encodings of the
 * class (CAS, CASP, LDLAR, STLLR) are newer: UNDEFINED.
 *
 * The exclusive monitor of a PE is the Cortex-A53's (TRM 6.5.1): local
 * and global in one, it marks the block of ARM64_ERG bytes, one cache
 * line, around the address of a Load-Exclusive (B2.12). It goes back to
 * Open Access with a Store-Exclusive, CLREX or an exception return; a
 * store of the PE itself leaves it, as the TRM says. A write of another
 * PE to the block clears it, which is an event that wakes the PE from
 * WFE (D1.7.1): Linux waits for a spinlock so. Writes of other bus
 * masters, such as DMA, do not: they take no part in the coherence of
 * the cores' caches, where the A53 keeps the monitor. See cpu.c.
 *
 * Memory accesses are single-copy atomic and in program order here, as
 * acquire and release ask: the emulator runs one instruction of one PE
 * at a time. So LDAR and STLR are LDR and STR that must be aligned, and
 * LDAXR and STLXR are LDXR and STXR.
 *
 * Exclusive accesses and ordered ones must be aligned to their whole
 * size, whatever SCTLR_ELx.A says. Choices of this model:
 *  - A Store-Exclusive whose address, size or number of registers is not
 *    those of the Load-Exclusive before it fails, one of the CONSTRAINED
 *    UNPREDICTABLE behaviors of B2.12.5.
 *  - A Store-Exclusive checks the monitor before it translates its
 *    address (AArch64_ExclusiveMonitorsPass(), an IMPLEMENTATION DEFINED
 *    order): one that fails takes no abort.
 *  - On the A53, an exclusive access to memory that is not Normal Inner
 *    and Outer Write-Back, with the data cache on, goes to a monitor
 *    outside the core, on the AXI bus. Whether the BCM2837 has one is not
 *    known: the machine stops, "not implemented yet".
 *  - A Store-Exclusive whose status register Rs is also Rt, Rt2 or Rn
 *    stores the registers' values from before it writes the status
 *    (Constraint_UNKNOWN). A load pair with Rt == Rt2 leaves the second
 *    value, as LDP does. The should-be-one fields, Rs of the loads and
 *    Rt2 of the single-register forms, are not checked.
 */

#include <stdio.h>

#include "cpu/arm64/internal.h"
#include "util/bits.h"

/* Whether a nibble of a MAIR attribute is Normal Write-Back memory. */
static bool write_back_nibble(unsigned n)
{
	return (n & 0xc) == 0xc || ((n & 0xc) == 0x4 && (n & 3) != 0);
}

/*
 * Whether the internal monitor serves an access to memory of attributes
 * attr: Normal Inner and Outer Write-Back, the data cache on (SCTLR.C of
 * the translation regime).
 */
static bool internal_monitor(const Arm64Cpu *c, uint8_t attr)
{
	unsigned regime = c->el > 1 ? c->el : 1;

	return write_back_nibble(attr >> 4) && write_back_nibble(attr & 0xf) &&
	       (c->sys.sctlr[regime] & ARM64_SCTLR_C);
}

/* The address of Xn|SP, with the alignment check of SP. */
static Arm64Exec base_address(Arm64Cpu *c, unsigned n, uint64_t *address)
{
	if (n == 31) {
		Arm64Exec result = arm64_check_sp_alignment(c);

		if (result != ARM64_EXEC_OK)
			return result;
	}
	*address = get_xsp(c, n);
	return ARM64_EXEC_OK;
}

/*
 * Translate an exclusive access at address, which is aligned, and check
 * that the internal monitor serves it. Returns ARM64_EXEC_OK with its PA
 * in *pa, or the abort or the stop.
 */
static Arm64Exec translate(Arm64Cpu *c, uint64_t address, bool write,
                           uint64_t *pa)
{
	Arm64Access access = write ? ARM64_ACCESS_WRITE : ARM64_ACCESS_READ;
	Arm64Translation t;
	Arm64Exec result = arm64_translate(c, address, access, c->el, true, &t);

	if (result != ARM64_EXEC_OK)
		return result;
	if (t.fault != ARM64_FAULT_NONE)
		return arm64_translation_fault(c, address, access, &t);
	if (!internal_monitor(c, t.attr)) {
		snprintf(c->detail, sizeof(c->detail),
		         "an exclusive access to memory of attributes 0x%02x",
		         (unsigned)t.attr);
		return ARM64_EXEC_UNIMPLEMENTED;
	}
	*pa = t.pa;
	return ARM64_EXEC_OK;
}

/* LDAR and STLR, C6.2.192 and C6.2.402: size bytes, aligned. */
static Arm64Exec ordered(Arm64Cpu *c, unsigned size, bool load, unsigned n,
                         unsigned t)
{
	uint64_t address, data;
	Arm64Exec result = base_address(c, n, &address);

	if (result != ARM64_EXEC_OK)
		return result;
	if (address & (size - 1))
		return arm64_alignment_fault(c, address, !load);
	if (!load)
		return arm64_mem_write(c, address, size, get_x(c, t));
	result = arm64_mem_read(c, address, size, &data);
	if (result == ARM64_EXEC_OK)
		set_x(c, t, data);
	return result;
}

/*
 * LDXR, LDAXR, LDXP and LDAXP, C6.2.266, C6.2.197, C6.2.265 and
 * C6.2.196: AArch64_SetExclusiveMonitors(), then the load. regs registers
 * of size bytes each.
 */
static Arm64Exec load_exclusive(Arm64Cpu *c, unsigned size, unsigned regs,
                                unsigned n, unsigned t, unsigned t2)
{
	uint64_t address, pa, first, second = 0;
	Arm64Exec result = base_address(c, n, &address);

	if (result != ARM64_EXEC_OK)
		return result;
	if (address & (size * regs - 1))
		return arm64_alignment_fault(c, address, false);
	result = translate(c, address, false, &pa);
	if (result != ARM64_EXEC_OK)
		return result;
	result = arm64_mem_read(c, address, size, &first);
	if (result == ARM64_EXEC_OK && regs == 2)
		result = arm64_mem_read(c, address + size, size, &second);
	if (result != ARM64_EXEC_OK)
		return result;
	c->monitor.block = pa & ~(uint64_t)(ARM64_ERG - 1);
	c->monitor.va = address;
	c->monitor.size = size;
	c->monitor.regs = regs;
	arm64_monitor_mark(c);
	set_x(c, t, first);
	if (regs == 2)
		set_x(c, t2, second);
	return ARM64_EXEC_OK;
}

/*
 * STXR, STLXR, STXP and STLXP, C6.2.450, C6.2.410, C6.2.449 and
 * C6.2.409: AArch64_ExclusiveMonitorsPass(), then the store, and the
 * status in Ws: 0 if it stored, 1 if not.
 */
static Arm64Exec store_exclusive(Arm64Cpu *c, unsigned size, unsigned regs,
                                 unsigned s, unsigned n, unsigned t,
                                 unsigned t2)
{
	uint64_t address, pa;
	uint64_t first = get_x(c, t), second = regs == 2 ? get_x(c, t2) : 0;
	Arm64Exec result = base_address(c, n, &address);
	bool passed;

	if (result != ARM64_EXEC_OK)
		return result;
	if (address & (size * regs - 1))
		return arm64_alignment_fault(c, address, true);
	passed = c->monitor.exclusive;
	if (passed) {
		result = translate(c, address, true, &pa);
		if (result != ARM64_EXEC_OK)
			return result;
		passed = c->monitor.block == (pa & ~(uint64_t)(ARM64_ERG - 1)) &&
		         c->monitor.va == address && c->monitor.size == size &&
		         c->monitor.regs == regs;
		arm64_monitor_clear(c);
	}
	if (passed) {
		result = arm64_mem_write(c, address, size, first);
		if (result == ARM64_EXEC_OK && regs == 2)
			result = arm64_mem_write(c, address + size, size, second);
		if (result != ARM64_EXEC_OK)
			return result;
	}
	set_x(c, s, passed ? 0 : 1);
	return ARM64_EXEC_OK;
}

/*
 * The class: size[31:30] 001000 o2[23] L[22] o1[21] Rs[20:16] o0[15]
 * Rt2[14:10] Rn[9:5] Rt[4:0]. o2 picks the ordered forms, o1 the pairs, L
 * the loads, o0 acquire or release.
 */
Arm64Exec arm64_exec_exclusive(Arm64Cpu *c, uint32_t insn)
{
	unsigned size = 1u << field32(insn, 31, 30);
	bool o2 = bit32(insn, 23), load = bit32(insn, 22), o1 = bit32(insn, 21);
	unsigned s = field32(insn, 20, 16), t2 = field32(insn, 14, 10);
	unsigned n = field32(insn, 9, 5), t = field32(insn, 4, 0);

	if (o2) {
		/* o1: CAS (FEAT_LSE); o0 = 0: LDLAR and STLLR (FEAT_LOR). */
		if (o1 || !bit32(insn, 15))
			return ARM64_EXEC_UNDEFINED;
		return ordered(c, size, load, n, t);
	}
	if (o1 && size < 4) /* CASP, FEAT_LSE */
		return ARM64_EXEC_UNDEFINED;
	if (load)
		return load_exclusive(c, size, o1 ? 2 : 1, n, t, t2);
	return store_exclusive(c, size, o1 ? 2 : 1, s, n, t, t2);
}
