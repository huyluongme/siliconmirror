/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Data accesses of the AArch64 model.
 *
 * System registers are not implemented yet, so the MMU is always off.
 * With stage 1 translation disabled, addresses map flat to physical
 * addresses and every data access uses the Device-nGnRnE memory type
 * (D8.2.12.1). An unaligned access to Device memory takes an Alignment
 * fault unless the IMPLEMENTATION DEFINED choice "Device location
 * supports unaligned access" is made (J1.2.4.30); this model does not
 * make it. Like the pseudocode, callers check the alignment of each
 * register's element, so LDP X0, X1 needs 8-byte alignment.
 */

#include "core/machine.h"
#include "cpu/arm64/internal.h"

static bool data_access(Arm64Cpu *c, uint64_t address, unsigned size,
                        bool write, uint64_t *value)
{
	const char *reason = NULL;

	if (address & (size - 1)) {
		reason = "alignment fault, as data accesses use Device memory "
				 "while the MMU is off";
	} else {
		Bus *bus = c->cpu.machine->bus;
		BusStatus status = write ? bus_write(bus, address, size, *value)
		                         : bus_read(bus, address, size, value);

		if (status != BUS_OK)
			reason = bus_status_str(status);
	}
	if (reason == NULL)
		return true;

	c->data_abort.address = address;
	c->data_abort.size = size;
	c->data_abort.write = write;
	c->data_abort.reason = reason;
	return false;
}

bool arm64_mem_read(Arm64Cpu *c, uint64_t address, unsigned size,
                    uint64_t *value)
{
	return data_access(c, address, size, false, value);
}

bool arm64_mem_write(Arm64Cpu *c, uint64_t address, unsigned size,
                     uint64_t value)
{
	return data_access(c, address, size, true, &value);
}
