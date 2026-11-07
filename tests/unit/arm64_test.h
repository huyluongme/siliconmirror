/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Helpers shared by the unit tests of the AArch64 CPU model. */

#ifndef TESTS_UNIT_ARM64_TEST_H
#define TESTS_UNIT_ARM64_TEST_H

#include <stdint.h>

#include "core/machine.h"
#include "cpu/arm64/cpu.h"
#include "test.h"

#define RESET_PC 0x80000

/* A machine with 1 MiB of RAM at 0 and one AArch64 CPU that resets to EL3. */
static inline Machine *arm64_test_machine(Arm64Cpu **cpu)
{
	static const Arm64Config config = {.reset_pc = RESET_PC, .highest_el = 3};
	Machine *m = machine_create();
	Cpu *base = arm64_cpu_create(&config);

	CHECK(m != NULL && base != NULL);
	CHECK_EQ(bus_map_ram(m->bus, "ram", 0, 0x100000), BUS_OK);
	CHECK(machine_add_cpu(m, base));
	*cpu = arm64_cpu_of(base);
	return m;
}

/* Store insn at the PC and run that one instruction. */
static inline MachineStop arm64_test_step(Machine *m, Arm64Cpu *c,
                                          uint32_t insn)
{
	CHECK_EQ(bus_write(m->bus, c->pc, 4, insn), BUS_OK);
	return machine_run(m, 1);
}

#endif /* TESTS_UNIT_ARM64_TEST_H */
