/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The interface between the machine and a CPU model.
 *
 * Each CPU architecture (src/cpu/<arch>/) has its own state struct with a
 * Cpu as a member, and fills in a CpuOps table. The core only sees the
 * Cpu; the CPU model gets back to its own struct with container_of().
 */

#ifndef CORE_CPU_H
#define CORE_CPU_H

#include <stddef.h>
#include <stdint.h>

typedef struct Machine Machine;
typedef struct Cpu Cpu;

typedef enum CpuState {
	CPU_RUNNING, /* executing instructions */
	CPU_HALTED,  /* waiting for an interrupt or an event (WFI, WFE) */
	CPU_OFF,     /* powered down, or not started yet */
} CpuState;

typedef struct CpuOps {
	/* Put the CPU in its reset state. */
	void (*reset)(Cpu *cpu);

	/*
	 * Execute at most max_insns instructions and return how many ran.
	 * Return early when the CPU leaves CPU_RUNNING, or after it stops the
	 * machine with machine_stop(), for example on an instruction it does
	 * not support.
	 */
	uint64_t (*run)(Cpu *cpu, uint64_t max_insns);

	/* Free the CPU and everything it owns. */
	void (*destroy)(Cpu *cpu);
} CpuOps;

struct Cpu {
	const CpuOps *ops;
	Machine *machine; /* set by machine_add_cpu() */
	unsigned index;   /* position in the machine, from 0 */
	CpuState state;
};

/* Set up the Cpu part of a CPU model. The CPU starts running. */
static inline void cpu_init(Cpu *cpu, const CpuOps *ops)
{
	cpu->ops = ops;
	cpu->machine = NULL;
	cpu->index = 0;
	cpu->state = CPU_RUNNING;
}

#endif /* CORE_CPU_H */
