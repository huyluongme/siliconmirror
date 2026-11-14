/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "cpu/arm64/cpu.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/machine.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/byteorder.h"
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
	arm64_tlb_flush(c);
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
 * Stop the machine on what the model does not emulate: result is
 * ARM64_EXEC_UNIMPLEMENTED or ARM64_EXEC_DATA_ABORT, from the
 * instruction insn, or from the fetch of the instruction at the PC.
 */
static void stop(Arm64Cpu *c, Arm64Exec result, bool fetch, uint32_t insn)
{
	const char *what = c->detail[0] != '\0' || fetch
	                       ? c->detail
	                       : arm64_group_name(arm64_group(insn));
	char other[48] = ""; /* the other address of a translated access */
	bool translated = !c->data_abort.walk &&
	                  c->data_abort.address != c->data_abort.virtual_address;

	if (result == ARM64_EXEC_UNIMPLEMENTED) {
		if (fetch)
			cpu_error(c,
			          "instruction fetch from 0x%016" PRIx64
			          " is not implemented yet (%s)",
			          c->pc, what);
		else
			cpu_error(c,
			          "instruction 0x%08" PRIx32 " at 0x%016" PRIx64
			          " is not implemented yet (%s)",
			          insn, c->pc, what);
		return;
	}
	if (fetch && !c->data_abort.walk) {
		if (translated)
			snprintf(other, sizeof(other), " (physical 0x%016" PRIx64 ")",
			         c->data_abort.address);
		cpu_error(c, "instruction fetch from 0x%016" PRIx64 "%s failed: %s",
		          c->pc, other, c->data_abort.reason);
		return;
	}
	if (translated)
		snprintf(other, sizeof(other), " (virtual 0x%016" PRIx64 ")",
		         c->data_abort.virtual_address);
	cpu_error(
		c, "%s%u-byte %s 0x%016" PRIx64 "%s at 0x%016" PRIx64 " failed: %s",
		c->data_abort.walk ? "translation table walk: " : "",
		c->data_abort.size, c->data_abort.write ? "write to" : "read from",
		c->data_abort.address, other, c->pc, c->data_abort.reason);
}

/*
 * Fetch the instruction at the PC through the MMU into *insn. Returns
 * ARM64_EXEC_OK, ARM64_EXEC_EXCEPTION for an Instruction Abort, or what
 * stops the machine.
 */
static Arm64Exec fetch(Arm64Cpu *c, uint32_t *insn)
{
	Arm64Translation t;
	Arm64Exec result;
	const uint8_t *host;
	uint64_t value;
	BusStatus status;
	bool device;

	/* The PC is aligned: the 4 bytes are in its page. */
	host = arm64_page_lookup(c, ARM64_PAGE_FETCH, c->pc, &device);
	if (host != NULL) {
		*insn = load_le32(host);
		return ARM64_EXEC_OK;
	}
	result = arm64_translate(c, c->pc, ARM64_ACCESS_IFETCH, c->el, true, &t);
	if (result != ARM64_EXEC_OK)
		return result;
	if (t.fault != ARM64_FAULT_NONE)
		return arm64_translation_fault(c, c->pc, ARM64_ACCESS_IFETCH, &t);
	host = arm64_page_fill(c, ARM64_PAGE_FETCH, c->pc, t.pa, t.attr);
	if (host != NULL) {
		*insn = load_le32(host);
		return ARM64_EXEC_OK;
	}
	status = bus_read(c->cpu.machine->bus, t.pa, 4, &value);
	if (status != BUS_OK)
		return arm64_bus_failure(c, t.pa, 4, false, status, false, c->pc);
	*insn = (uint32_t)value;
	return ARM64_EXEC_OK;
}

Arm64Exec arm64_bus_failure(Arm64Cpu *c, uint64_t address, unsigned size,
                            bool write, BusStatus status, bool walk,
                            uint64_t va)
{
	c->data_abort.address = address;
	c->data_abort.size = size;
	c->data_abort.write = write;
	c->data_abort.reason = bus_status_str(status);
	c->data_abort.walk = walk;
	c->data_abort.virtual_address = va;
	return ARM64_EXEC_DATA_ABORT;
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

	uint32_t insn = 0;
	Arm64Exec result;

	c->detail[0] = '\0';
	result = fetch(c, &insn);
	if (result == ARM64_EXEC_OK) {
		c->next_pc = c->pc + 4;
		result = arm64_execute(c, insn);
	} else if (result != ARM64_EXEC_EXCEPTION) {
		stop(c, result, true, 0);
		return false;
	}
	switch (result) {
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
	case ARM64_EXEC_DATA_ABORT:
		stop(c, result, false, insn);
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

void arm64_tlb_flush_all_pes(Arm64Cpu *c)
{
	Machine *m = c->cpu.machine;

	for (unsigned i = 0; i < m->num_cpus; i++)
		if (m->cpus[i]->ops == &arm64_ops)
			arm64_tlb_flush(arm64_cpu_of(m->cpus[i]));
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

/*
 * What ID_AA64MMFR0_EL1 says about the MMU (D24.2): PARange, the size of
 * physical addresses, and TGran4, TGran16 and TGran64, which granules
 * the core has. 0b1111 means "not implemented" for TGran4 and TGran64,
 * 0b0000 for TGran16.
 */
static void mmu_features(Arm64Cpu *c)
{
	static const unsigned pa_sizes[7] = {32, 36, 40, 42, 44, 48, 52};
	uint64_t mmfr0 = 0;
	unsigned parange;

	arm64_cpu_read_sysreg(c, ARM64_ID_AA64MMFR0_EL1, &mmfr0);
	parange = mmfr0 & 0xf;
	c->pa_bits = parange < 7 ? pa_sizes[parange] : 48;
	c->granule_4k = ((mmfr0 >> 28) & 0xf) != 0xf;
	c->granule_64k = ((mmfr0 >> 24) & 0xf) != 0xf;
	c->granule_16k = ((mmfr0 >> 20) & 0xf) != 0;
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
	mmu_features(c);
	arm64_interrupt_init(c);
	if (!arm64_timer_init(c)) {
		free(c);
		return NULL;
	}
	arm64_reset(&c->cpu);
	return &c->cpu;
}
