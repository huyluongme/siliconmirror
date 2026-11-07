/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "core/machine.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

Machine *machine_create(void)
{
	Machine *m = calloc(1, sizeof(*m));

	if (m == NULL)
		return NULL;
	m->bus = bus_create();
	if (m->bus == NULL) {
		free(m);
		return NULL;
	}
	event_queue_init(&m->events);
	return m;
}

void machine_destroy(Machine *m)
{
	if (m == NULL)
		return;
	for (unsigned i = m->num_devices; i-- > 0;) {
		MachineDevice *dev = &m->devices[i];

		if (dev->destroy != NULL)
			dev->destroy(dev->ctx);
	}
	for (unsigned i = 0; i < m->num_cpus; i++)
		m->cpus[i]->ops->destroy(m->cpus[i]);
	bus_destroy(m->bus);
	free(m);
}

bool machine_add_cpu(Machine *m, Cpu *cpu)
{
	if (m->num_cpus == MACHINE_MAX_CPUS)
		return false;
	cpu->machine = m;
	cpu->index = m->num_cpus;
	m->cpus[m->num_cpus++] = cpu;
	return true;
}

bool machine_add_device(Machine *m, void *ctx, void (*reset)(void *ctx),
                        void (*destroy)(void *ctx))
{
	if (m->num_devices == MACHINE_MAX_DEVICES)
		return false;
	m->devices[m->num_devices++] = (MachineDevice){
		.ctx = ctx,
		.reset = reset,
		.destroy = destroy,
	};
	return true;
}

void machine_reset(Machine *m)
{
	for (unsigned i = 0; i < m->num_devices; i++) {
		MachineDevice *dev = &m->devices[i];

		if (dev->reset != NULL)
			dev->reset(dev->ctx);
	}
	for (unsigned i = 0; i < m->num_cpus; i++)
		m->cpus[i]->ops->reset(m->cpus[i]);
}

void machine_stop(Machine *m, MachineStop reason, const char *fmt, ...)
{
	if (m->stop != MACHINE_STOP_NONE)
		return;

	va_list args;

	m->stop = reason;
	va_start(args, fmt);
	vsnprintf(m->stop_message, sizeof(m->stop_message), fmt, args);
	va_end(args);
}

/*
 * Give every running CPU one turn, covering virtual time from now to end.
 * Returns whether any CPU was running.
 */
static bool run_cpus(Machine *m, uint64_t end, uint64_t limit)
{
	uint64_t slice = (end - m->events.now) / MACHINE_NS_PER_INSN;
	bool any_running = false;

	for (unsigned i = 0; i < m->num_cpus; i++) {
		Cpu *cpu = m->cpus[i];

		if (cpu->state != CPU_RUNNING)
			continue;
		any_running = true;

		uint64_t budget = slice;

		if (budget > limit - m->insns)
			budget = limit - m->insns;
		if (budget == 0)
			continue;

		m->insns += cpu->ops->run(cpu, budget);
		if (m->stop != MACHINE_STOP_NONE)
			break;
	}
	return any_running;
}

MachineStop machine_run(Machine *m, uint64_t max_insns)
{
	uint64_t limit = UINT64_MAX;

	if (max_insns != 0 && max_insns < UINT64_MAX - m->insns)
		limit = m->insns + max_insns;
	m->stop = MACHINE_STOP_NONE;
	m->stop_message[0] = '\0';

	while (m->stop == MACHINE_STOP_NONE) {
		uint64_t now = m->events.now;
		uint64_t next = event_queue_next(&m->events);

		/* A turn ends at the end of the slice or at the next event. */
		uint64_t end =
			next - now < MACHINE_SLICE_NS ? next : now + MACHINE_SLICE_NS;

		if (!run_cpus(m, end, limit)) {
			if (next == EVENT_NEVER) {
				machine_stop(m, MACHINE_STOP_IDLE,
				             "every CPU is waiting and no event is pending");
				break;
			}
			/* Nothing runs until the next event: skip straight to it. */
			end = next;
		}
		if (m->stop != MACHINE_STOP_NONE)
			break;

		event_queue_run_until(&m->events, end);
		if (m->insns >= limit)
			machine_stop(m, MACHINE_STOP_LIMIT,
			             "instruction limit reached after %" PRIu64
			             " instructions",
			             m->insns);
	}
	return m->stop;
}

const char *machine_stop_str(MachineStop stop)
{
	switch (stop) {
	case MACHINE_STOP_NONE:
		return "running";
	case MACHINE_STOP_IDLE:
		return "idle";
	case MACHINE_STOP_LIMIT:
		return "instruction limit";
	case MACHINE_STOP_SHUTDOWN:
		return "shutdown";
	case MACHINE_STOP_ERROR:
		return "error";
	}
	return "unknown stop reason";
}
