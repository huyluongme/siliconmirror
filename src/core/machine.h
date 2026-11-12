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

#define MACHINE_MAX_DEVICES 32

/*
 * Something else the machine owns: a device model, or a back end that a
 * device uses. The functions take ctx and may be NULL.
 */
typedef struct MachineDevice {
	void *ctx;
	void (*reset)(void *ctx);
	void (*destroy)(void *ctx);
} MachineDevice;

/*
 * Told when the CPUs of the machine stop running or start again. idle is
 * true when none runs any more, and since is the time the last one
 * stopped, its last instruction done; idle is false when one runs again,
 * and since is the start of its turn. The main loop calls it at the end
 * of a turn, before the events of the turn fire.
 */
typedef void (*MachineIdleListener)(void *ctx, bool idle, uint64_t since);

/* How often the main loop polls the input from the host, in virtual time. */
#define MACHINE_POLL_NS 1000000

/* The most inputs a machine has: one for each UART that the host reaches. */
#define MACHINE_MAX_INPUTS 4

/*
 * Input from the host, such as the keys typed for a UART. The main loop
 * calls the poll of every input at least every MACHINE_POLL_NS of virtual
 * time, between turns, for its device to take what the host has sent.
 * When no CPU runs and no event is pending, it calls the wait of each
 * input in turn instead of stopping as idle: wait blocks until there is
 * input for the device, takes it and returns true, or returns false when
 * the device cannot take any: it does not listen, or the input has ended.
 * As the first wait that blocks holds the machine, at most one input
 * should block at a time.
 */
typedef struct MachineInput {
	void (*poll)(void *ctx);
	bool (*wait)(void *ctx);
	void *ctx;
} MachineInput;

struct Machine {
	Bus *bus;
	EventQueue events;
	Cpu *cpus[MACHINE_MAX_CPUS];
	unsigned num_cpus;
	MachineDevice devices[MACHINE_MAX_DEVICES];
	unsigned num_devices;
	Cpu *running;   /* the CPU in its turn, or NULL between turns */
	uint64_t insns; /* instructions run so far, by all CPUs */
	MachineStop stop;
	char stop_message[256];
	bool idle; /* no CPU runs, as last told to the idle listener */
	MachineIdleListener idle_listener;
	void *idle_ctx;
	MachineInput inputs[MACHINE_MAX_INPUTS];
	unsigned num_inputs;
	uint64_t next_poll; /* virtual time of the next poll of the inputs */
};

/* Create a machine with an empty bus. Returns NULL when out of memory. */
Machine *machine_create(void);

/*
 * Destroy the devices, newest first, then the CPUs, the bus and the
 * machine. m may be NULL.
 */
void machine_destroy(Machine *m);

/*
 * Add a CPU; the machine owns it from now on. Returns false when the
 * machine already has MACHINE_MAX_CPUS CPUs.
 */
bool machine_add_cpu(Machine *m, Cpu *cpu);

/*
 * Make the machine own a device or back end. Add a back end before the
 * devices that use it, so that it is destroyed after them. Returns false,
 * and the caller keeps ctx, when the machine already has
 * MACHINE_MAX_DEVICES of them.
 */
bool machine_add_device(Machine *m, void *ctx, void (*reset)(void *ctx),
                        void (*destroy)(void *ctx));

/*
 * Set the one idle listener of the machine, or remove it with NULL. A
 * new machine counts as running.
 */
void machine_set_idle_listener(Machine *m, MachineIdleListener fn, void *ctx);

/*
 * Add an input to the machine. Returns false when it has
 * MACHINE_MAX_INPUTS already.
 */
bool machine_add_input(Machine *m, void (*poll)(void *ctx),
                       bool (*wait)(void *ctx), void *ctx);

/* Remove the input of ctx, if the machine has one. */
void machine_remove_input(Machine *m, void *ctx);

/* Reset every device, in the order they were added, then every CPU. */
void machine_reset(Machine *m);

/*
 * Run until the machine stops, and return why. max_insns limits how many
 * instructions run in this call; 0 means no limit. After a stop, calling
 * machine_run() again carries on from where it stopped.
 */
MachineStop machine_run(Machine *m, uint64_t max_insns);

/*
 * The current virtual time. In a CPU's turn, the CPUs take turns running
 * through the same stretch of time, and this is the time of the running
 * CPU: the start of the turn, plus the time of the instructions it has
 * completed in the turn (Cpu.executed). Between turns, while events and
 * devices run, it is the time of the event queue.
 */
uint64_t machine_now(const Machine *m);

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
