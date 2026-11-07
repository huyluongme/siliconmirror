/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A machine at run time: its bus, CPUs and virtual time, and the main
 * loop that drives them.
 *
 * A board (src/machine/) creates a Machine, adds CPUs and maps devices;
 * the front end then calls machine_run(). See docs/architecture.md,
 * "Virtual time", for how the main loop shares time between CPUs.
 */

#ifndef CORE_MACHINE_H
#define CORE_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bus.h"
#include "core/cpu.h"
#include "core/event.h"
#include "util/macros.h"

#define MACHINE_MAX_CPUS 8

/* Virtual time one instruction takes: a nominal 1 GHz CPU. */
#define MACHINE_NS_PER_INSN 1

/* Longest stretch of virtual time a CPU runs before the next one. */
#define MACHINE_SLICE_NS 1000

typedef enum MachineStop {
	MACHINE_STOP_NONE = 0, /* still running */
	MACHINE_STOP_IDLE,     /* every CPU waits and no event is pending */
	MACHINE_STOP_LIMIT,    /* the instruction limit was reached */
	MACHINE_STOP_SHUTDOWN, /* the guest or the user asked to stop */
	MACHINE_STOP_ERROR,    /* emulation error; see stop_message */
} MachineStop;

struct Machine {
	Bus *bus;
	EventQueue events;
	Cpu *cpus[MACHINE_MAX_CPUS];
	unsigned num_cpus;
	uint64_t insns; /* instructions run so far, by all CPUs */
	MachineStop stop;
	char stop_message[256];
};

/* Create a machine with an empty bus. Returns NULL when out of memory. */
Machine *machine_create(void);

/* Destroy the CPUs, the bus and the machine. m may be NULL. */
void machine_destroy(Machine *m);

/*
 * Add a CPU; the machine owns it from now on. Returns false when the
 * machine already has MACHINE_MAX_CPUS CPUs.
 */
bool machine_add_cpu(Machine *m, Cpu *cpu);

/* Reset every CPU. */
void machine_reset(Machine *m);

/*
 * Run until the machine stops, and return why. max_insns limits how many
 * instructions run in this call; 0 means no limit. After a stop, calling
 * machine_run() again carries on from where it stopped.
 */
MachineStop machine_run(Machine *m, uint64_t max_insns);

/*
 * Ask the main loop to stop as soon as the current CPU returns, with a
 * printf-style message. If the machine is already stopping, the first
 * reason and message are kept.
 */
void machine_stop(Machine *m, MachineStop reason, const char *fmt, ...)
	PRINTF_FORMAT(3, 4);

/* A short description of a stop reason, for messages. */
const char *machine_stop_str(MachineStop stop);

#endif /* CORE_MACHINE_H */
