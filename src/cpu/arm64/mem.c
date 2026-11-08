/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Data accesses of the AArch64 model.
 *
 * The MMU is not implemented yet: turning it on stops the machine, so it
 * is always off. With stage 1 translation disabled, addresses map flat
 * to physical addresses and every data access uses the Device-nGnRnE
 * memory type (D8.2.12.1). An unaligned access to Device memory takes an
 * Alignment fault unless the IMPLEMENTATION DEFINED choice "Device
 * location supports unaligned access" is made (J1.2.4.30); this model
 * does not make it. Like the pseudocode, callers check the alignment of
 * each register's element, so LDP X0, X1 needs 8-byte alignment.
 *
 * An access to an address where the machine emulates nothing stops the
 * machine: what the real board does there is not known.
 */

#include "core/machine.h"
#include "cpu/arm64/internal.h"

static Arm64Exec data_access(Arm64Cpu *c, uint64_t address, unsigned size,
                             bool write, uint64_t *value)
{
	if (address & (size - 1))
		return arm64_alignment_fault(c, address, write);

	Bus *bus = c->cpu.machine->bus;
	BusStatus status = write ? bus_write(bus, address, size, *value)
	                         : bus_read(bus, address, size, value);

	if (status == BUS_OK)
		return ARM64_EXEC_OK;
	c->data_abort.address = address;
	c->data_abort.size = size;
	c->data_abort.write = write;
	c->data_abort.reason = bus_status_str(status);
	return ARM64_EXEC_DATA_ABORT;
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
