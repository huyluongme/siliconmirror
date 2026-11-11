/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "cpu/arm64/cpu.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "core/machine.h"
#include "cpu/arm64/internal.h"
#include "util/macros.h"

Arm64Cpu *arm64_cpu_of(Cpu *cpu)
{
	return container_of(cpu, Arm64Cpu, cpu);
}

/*
 * Reset into AArch64 state, after AArch64_TakeReset() in J1.2.2.30. The
 * general-purpose registers, NZCV, the stack pointers and the Event
 * Register (D1.7.1.1) are UNKNOWN after reset; this model clears them so
 * that every run is the same.
 */
static void arm64_reset(Cpu *base)
{
	Arm64Cpu *c = arm64_cpu_of(base);

	memset(c->x, 0, sizeof(c->x));
	memset(c->sp_el, 0, sizeof(c->sp_el));
	c->nzcv = 0;
	c->el = c->config.highest_el;
	c->sp_sel = true; /* PSTATE.SP = '1' */
	c->il = false;
	c->daif = ARM64_DAIF_D | ARM64_DAIF_A | ARM64_DAIF_I | ARM64_DAIF_F;
	c->pc = c->config.reset_pc;
	c->next_pc = c->pc;
	c->event_register = false;
	arm64_sysreg_reset(c);
	arm64_timer_reset(c);
	base->state = CPU_RUNNING;
}

/* Stop the machine with a message that names this CPU. */
#define cpu_error(c, fmt, ...)                                                 \
	machine_stop((c)->cpu.machine, MACHINE_STOP_ERROR, "cpu%u: " fmt,          \
	             (c)->cpu.index, __VA_ARGS__)

/* Take the Undefined Instruction exception, AArch64_Undefined(). */
static void take_undefined(Arm64Cpu *c)
{
	arm64_raise(c, arm64_el1_target(c), ARM64_EC_UNKNOWN, 0);
	arm64_take_exception(c);
}

/*
 * Take a pending interrupt, or fetch, decode and execute one instruction
 * or take the exception that it raises. Returns false when the machine stops:
 * on what the model does not emulate yet.
 */
static bool step(Arm64Cpu *c)
{
	if (arm64_interrupt_requested(c) && arm64_take_pending_interrupt(c))
		return true;

	/*
	 * Priorities 6 and 7 of D1.4.5.6: a PC alignment fault, then an
	 * Illegal Execution state exception. The second may also come after
	 * the instruction fetch, at priority 12; this model takes it first.
	 */
	if (c->pc & 3) {
		arm64_pc_alignment_fault(c);
		arm64_take_exception(c);
		return true;
	}
	if (c->il) {
		arm64_illegal_state(c);
		arm64_take_exception(c);
		return true;
	}

	uint64_t insn;
	BusStatus status = bus_read(c->cpu.machine->bus, c->pc, 4, &insn);

	if (status != BUS_OK) {
		cpu_error(c, "instruction fetch from 0x%016" PRIx64 " failed: %s",
		          c->pc, bus_status_str(status));
		return false;
	}

	c->next_pc = c->pc + 4;
	c->detail[0] = '\0';
	switch (arm64_execute(c, (uint32_t)insn)) {
	case ARM64_EXEC_OK:
		c->pc = c->next_pc;
		return true;
	case ARM64_EXEC_UNDEFINED:
		take_undefined(c);
		return true;
	case ARM64_EXEC_EXCEPTION:
		arm64_take_exception(c);
		return true;
	case ARM64_EXEC_UNIMPLEMENTED:
		cpu_error(c,
		          "instruction 0x%08" PRIx32 " at 0x%016" PRIx64
		          " is not implemented yet (%s)",
		          (uint32_t)insn, c->pc,
		          c->detail[0] != '\0'
		              ? c->detail
		              : arm64_group_name(arm64_group((uint32_t)insn)));
		return false;
	case ARM64_EXEC_DATA_ABORT:
		cpu_error(
			c, "%u-byte %s 0x%016" PRIx64 " at 0x%016" PRIx64 " failed: %s",
			c->data_abort.size, c->data_abort.write ? "write to" : "read from",
			c->data_abort.address, c->pc, c->data_abort.reason);
		return false;
	}
	return false;
}

static uint64_t arm64_run(Cpu *base, uint64_t max_insns)
{
	Arm64Cpu *c = arm64_cpu_of(base);
	uint64_t n = 0;

	base->executed = 0;
	while (n < max_insns && base->state == CPU_RUNNING &&
	       base->machine->stop == MACHINE_STOP_NONE) {
		if (!step(c))
			break;
		base->executed = ++n;
	}
	return n;
}

static void arm64_destroy(Cpu *base)
{
	free(arm64_cpu_of(base));
}

static const CpuOps arm64_ops = {
	.reset = arm64_reset,
	.run = arm64_run,
	.destroy = arm64_destroy,
};

/*
 * SEV: signal an event to every AArch64 PE of the machine that is not
 * powered off, this one included (D1.7.1.5). The event sets the Event
 * Register, and it is a WFE wakeup event (D1.7.1.4): a PE waiting in WFE
 * runs again. Its WFE then completes with the Event Register clear, one
 * of the IMPLEMENTATION DEFINED states that D1.7.1.2 allows.
 */
void arm64_send_event(Arm64Cpu *c)
{
	Machine *m = c->cpu.machine;

	for (unsigned i = 0; i < m->num_cpus; i++) {
		Cpu *cpu = m->cpus[i];

		if (cpu->ops != &arm64_ops || cpu->state == CPU_OFF)
			continue;
		arm64_set_event_register(arm64_cpu_of(cpu));
	}
}

void arm64_set_event_register(Arm64Cpu *c)
{
	if (c->cpu.state == CPU_HALTED && c->wait == ARM64_WAIT_EVENT) {
		c->cpu.state = CPU_RUNNING;
		c->event_register = false;
	} else {
		c->event_register = true;
	}
}

Cpu *arm64_cpu_create(const Arm64Config *config)
{
	if (config->highest_el < 1 || config->highest_el > 3)
		return NULL;
	if (config->model != NULL && config->model->num_impdef > ARM64_MAX_IMPDEF)
		return NULL;
	if (config->cluster != NULL && config->cluster->model != config->model)
		return NULL;

	Arm64Cpu *c = calloc(1, sizeof(*c));

	if (c == NULL)
		return NULL;
	cpu_init(&c->cpu, &arm64_ops);
	c->config = *config;
	arm64_interrupt_init(c);
	if (!arm64_timer_init(c)) {
		free(c);
		return NULL;
	}
	arm64_reset(&c->cpu);
	return &c->cpu;
}
