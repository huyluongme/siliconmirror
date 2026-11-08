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
	c->daif = ARM64_DAIF_D | ARM64_DAIF_A | ARM64_DAIF_I | ARM64_DAIF_F;
	c->pc = c->config.reset_pc;
	c->next_pc = c->pc;
	c->event_register = false;
	arm64_sysreg_reset(c);
	base->state = CPU_RUNNING;
}

/* Stop the machine with a message that names this CPU. */
#define cpu_error(c, fmt, ...)                                                 \
	machine_stop((c)->cpu.machine, MACHINE_STOP_ERROR, "cpu%u: " fmt,          \
	             (c)->cpu.index, __VA_ARGS__)

/* Fetch, decode and execute one instruction. Returns false on error. */
static bool step(Arm64Cpu *c)
{
	/* D1.4.10.1: an instruction at a misaligned PC takes an exception. */
	if (c->pc & 3) {
		cpu_error(c,
		          "PC alignment fault at 0x%016" PRIx64
		          " (exceptions are not implemented yet)",
		          c->pc);
		return false;
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
		cpu_error(c,
		          "undefined instruction 0x%08" PRIx32 " at 0x%016" PRIx64
		          " (exceptions are not implemented yet)",
		          (uint32_t)insn, c->pc);
		return false;
	case ARM64_EXEC_UNIMPLEMENTED:
		cpu_error(c,
		          "instruction 0x%08" PRIx32 " at 0x%016" PRIx64
		          " is not implemented yet (%s)",
		          (uint32_t)insn, c->pc,
		          c->detail[0] != '\0'
		              ? c->detail
		              : arm64_group_name(arm64_group((uint32_t)insn)));
		return false;
	case ARM64_EXEC_EXCEPTION:
		cpu_error(c,
		          "instruction 0x%08" PRIx32 " at 0x%016" PRIx64
		          " takes an exception to EL%u, ESR 0x%08" PRIx32
		          " (exceptions are not implemented yet)",
		          (uint32_t)insn, c->pc, c->exception.target_el,
		          c->exception.esr);
		return false;
	case ARM64_EXEC_DATA_ABORT:
		cpu_error(c,
		          "data abort at 0x%016" PRIx64 ": %u-byte %s 0x%016" PRIx64
		          " failed: %s (exceptions are not implemented yet)",
		          c->pc, c->data_abort.size,
		          c->data_abort.write ? "write to" : "read from",
		          c->data_abort.address, c->data_abort.reason);
		return false;
	}
	return false;
}

static uint64_t arm64_run(Cpu *base, uint64_t max_insns)
{
	Arm64Cpu *c = arm64_cpu_of(base);
	uint64_t n = 0;

	while (n < max_insns && base->state == CPU_RUNNING &&
	       base->machine->stop == MACHINE_STOP_NONE) {
		if (!step(c))
			break;
		n++;
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
		Arm64Cpu *pe;

		if (cpu->ops != &arm64_ops || cpu->state == CPU_OFF)
			continue;
		pe = arm64_cpu_of(cpu);
		if (cpu->state == CPU_HALTED && pe->wait == ARM64_WAIT_EVENT) {
			cpu->state = CPU_RUNNING;
			pe->event_register = false;
		} else {
			pe->event_register = true;
		}
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
	arm64_reset(&c->cpu);
	return &c->cpu;
}
