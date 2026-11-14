/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Data accesses of the AArch64 model, through the MMU (mmu.c).
 *
 * An unaligned access takes an Alignment fault when SCTLR_ELx.A asks
 * for aligned accesses, before translation (AArch64_CheckAlignment()),
 * or when the address is Device memory: every data address while stage
 * 1 translation is disabled. The IMPLEMENTATION DEFINED choice "Device
 * location supports unaligned access" (J1.2.4.30) is not made. Like the
 * pseudocode, callers check the alignment of each register's element,
 * so LDP X0, X1 is aligned at 8 bytes.
 *
 * An unaligned access to Normal memory that crosses into another 4KB
 * page translates each page on its own, and accesses its bytes one by
 * one, as the pseudocode does for an unaligned access. A fault in the
 * second page has the first address of that page in FAR_ELx, and leaves
 * memory as it was.
 *
 * An access to an address where the machine emulates nothing stops the
 * machine: what the real board does there is not known.
 */

#include "core/machine.h"
#include "cpu/arm64/internal.h"
#include "util/byteorder.h"

/* SCTLR_ELx.A for the current Exception level, SCTLR_EL1.A at EL0. */
static bool alignment_checked(const Arm64Cpu *c)
{
	return c->sys.sctlr[c->el > 1 ? c->el : 1] & ARM64_SCTLR_A;
}

/* The bus access to a physical address, for the virtual address va. */
static Arm64Exec physical_access(Arm64Cpu *c, uint64_t va, uint64_t pa,
                                 unsigned size, bool write, uint64_t *value)
{
	Bus *bus = c->cpu.machine->bus;
	BusStatus status = write ? bus_write(bus, pa, size, *value)
	                         : bus_read(bus, pa, size, value);

	if (status == BUS_OK)
		return ARM64_EXEC_OK;
	return arm64_bus_failure(c, pa, size, write, status, false, va);
}

/*
 * An unaligned access whose bytes from va + split on are in another
 * page, which pa2 holds: byte by byte, least significant first.
 */
static Arm64Exec split_access(Arm64Cpu *c, uint64_t va, uint64_t pa,
                              uint64_t pa2, unsigned split, unsigned size,
                              bool write, uint64_t *value)
{
	uint64_t result = 0;

	for (unsigned i = 0; i < size; i++) {
		uint64_t byte = write ? (*value >> (8 * i)) & 0xff : 0;
		uint64_t address = i < split ? pa + i : pa2 + (i - split);
		Arm64Exec status = physical_access(c, va + i, address, 1, write, &byte);

		if (status != ARM64_EXEC_OK)
			return status;
		result |= byte << (8 * i);
	}
	if (!write)
		*value = result;
	return ARM64_EXEC_OK;
}

/* An access to host memory, which holds RAM. */
static Arm64Exec host_access(uint8_t *host, unsigned size, bool write,
                             uint64_t *value)
{
	if (write)
		store_le(host, size, *value);
	else
		*value = load_le(host, size);
	return ARM64_EXEC_OK;
}

static Arm64Exec data_access(Arm64Cpu *c, uint64_t va, unsigned size,
                             bool write, uint64_t *value)
{
	Arm64Access access = write ? ARM64_ACCESS_WRITE : ARM64_ACCESS_READ;
	unsigned kind = write ? ARM64_PAGE_WRITE : ARM64_PAGE_READ;
	bool aligned = (va & (size - 1)) == 0, device;
	uint64_t next_page = (va | 0xfff) + 1;
	Arm64Translation t, t2;
	Arm64Exec result;
	uint8_t *host;

	if (!aligned && alignment_checked(c))
		return arm64_alignment_fault(c, va, write);
	/*
	 * In a page that this kind of access translated before, at this
	 * level; but an unaligned access to Device memory faults.
	 */
	if (next_page - va >= size) {
		host = arm64_page_lookup(c, kind, va, &device);
		if (host != NULL && (aligned || !device))
			return host_access(host, size, write, value);
	}
	result = arm64_translate(c, va, access, c->el, aligned, &t);
	if (result != ARM64_EXEC_OK)
		return result;
	if (t.fault != ARM64_FAULT_NONE)
		return arm64_translation_fault(c, va, access, &t);
	if (next_page - va >= size) {
		host = arm64_page_fill(c, kind, va, t.pa, t.attr);
		if (host != NULL)
			return host_access(host, size, write, value);
		return physical_access(c, va, t.pa, size, write, value);
	}

	result = arm64_translate(c, next_page, access, c->el, false, &t2);
	if (result != ARM64_EXEC_OK)
		return result;
	if (t2.fault != ARM64_FAULT_NONE)
		return arm64_translation_fault(c, next_page, access, &t2);
	return split_access(c, va, t.pa, t2.pa, (unsigned)(next_page - va), size,
	                    write, value);
}

Arm64Exec arm64_mem_read(Arm64Cpu *c, uint64_t address, unsigned size,
                         uint64_t *value)
{
	return data_access(c, address, size, false, value);
}

Arm64Exec arm64_mem_write(Arm64Cpu *c, uint64_t address, unsigned size,
                          uint64_t value)
{
	return data_access(c, address, size, true, &value);
}
