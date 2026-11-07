/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for the machine main loop (src/core/machine.c). */

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/machine.h"
#include "test.h"
#include "util/macros.h"

/*
 * A fake CPU. It "executes" instructions by counting them, and can halt
 * or report an error after a given number of instructions.
 */
typedef struct FakeCpu {
	Cpu cpu;
	uint64_t executed; /* instructions executed so far */
	uint64_t halt_at;  /* halt when executed reaches this */
	uint64_t error_at; /* stop the machine with an error here */
	unsigned runs;     /* calls to run() */
	unsigned resets;
} FakeCpu;

/* Order in which CPUs got their turns, by CPU index. */
static unsigned turns[16];
static unsigned num_turns;

static FakeCpu *fake_of(Cpu *cpu)
{
	return container_of(cpu, FakeCpu, cpu);
}

static void fake_reset(Cpu *cpu)
{
	fake_of(cpu)->resets++;
}

static uint64_t fake_run(Cpu *cpu, uint64_t max_insns)
{
	FakeCpu *f = fake_of(cpu);
	uint64_t n = 0;

	f->runs++;
	if (num_turns < ARRAY_SIZE(turns))
		turns[num_turns] = cpu->index;
	num_turns++;

	while (n < max_insns) {
		if (f->executed == f->error_at) {
			machine_stop(cpu->machine, MACHINE_STOP_ERROR,
			             "fake error after %" PRIu64 " instructions",
			             f->executed);
			break;
		}
		f->executed++;
		n++;
		if (f->executed == f->halt_at) {
			cpu->state = CPU_HALTED;
			break;
		}
	}
	return n;
}

static void fake_destroy(Cpu *cpu)
{
	free(fake_of(cpu));
}

static const CpuOps fake_ops = {
	.reset = fake_reset,
	.run = fake_run,
	.destroy = fake_destroy,
};

/* Add a fake CPU that halts after halt_at instructions. */
static FakeCpu *add_fake_cpu(Machine *m, uint64_t halt_at)
{
	FakeCpu *f = calloc(1, sizeof(*f));

	cpu_init(&f->cpu, &fake_ops);
	f->halt_at = halt_at;
	f->error_at = UINT64_MAX;
	CHECK(machine_add_cpu(m, &f->cpu));
	return f;
}

static void test_no_cpus_is_idle(void)
{
	Machine *m = machine_create();

	CHECK(m != NULL);
	CHECK_EQ(machine_run(m, 0), MACHINE_STOP_IDLE);
	CHECK_EQ(m->events.now, 0);
	CHECK_EQ(m->insns, 0);
	CHECK(strlen(m->stop_message) > 0);
	machine_destroy(m);
	machine_destroy(NULL);
}

static void test_cpu_halts_then_machine_is_idle(void)
{
	Machine *m = machine_create();
	FakeCpu *f = add_fake_cpu(m, 2500);

	/* Turns of 1000, 1000, then 500 instructions before the halt. */
	CHECK_EQ(machine_run(m, 0), MACHINE_STOP_IDLE);
	CHECK_EQ(f->executed, 2500);
	CHECK_EQ(f->runs, 3);
	CHECK_EQ(m->insns, 2500);
	CHECK_EQ(m->events.now, 3000);
	machine_destroy(m);
}

static void test_instruction_limit(void)
{
	Machine *m = machine_create();
	FakeCpu *f = add_fake_cpu(m, UINT64_MAX);

	CHECK_EQ(machine_run(m, 1500), MACHINE_STOP_LIMIT);
	CHECK_EQ(f->executed, 1500);

	/* Running again carries on, with a new limit. */
	CHECK_EQ(machine_run(m, 500), MACHINE_STOP_LIMIT);
	CHECK_EQ(f->executed, 2000);
	machine_destroy(m);
}

static void test_limit_shared_by_cpus(void)
{
	Machine *m = machine_create();
	FakeCpu *a = add_fake_cpu(m, UINT64_MAX);
	FakeCpu *b = add_fake_cpu(m, UINT64_MAX);
	FakeCpu *c = add_fake_cpu(m, UINT64_MAX);

	/* The limit runs out during the second CPU's turn. */
	CHECK_EQ(machine_run(m, 1500), MACHINE_STOP_LIMIT);
	CHECK_EQ(a->executed, 1000);
	CHECK_EQ(b->executed, 500);
	CHECK_EQ(c->executed, 0);
	CHECK_EQ(c->runs, 0);
	CHECK_EQ(m->insns, 1500);
	machine_destroy(m);
}

/* An event that wakes a halted fake CPU for 10 more instructions. */
typedef struct Waker {
	Event ev;
	FakeCpu *cpu;
	uint64_t fired_at;
} Waker;

static void wake(void *ctx)
{
	Waker *w = ctx;

	w->fired_at = w->cpu->cpu.machine->events.now;
	w->cpu->halt_at = w->cpu->executed + 10;
	w->cpu->cpu.state = CPU_RUNNING;
}

static void test_event_wakes_halted_cpu(void)
{
	Machine *m = machine_create();
	FakeCpu *f = add_fake_cpu(m, 100);
	Waker w = {.cpu = f};

	event_init(&w.ev, wake, &w);
	event_schedule(&m->events, &w.ev, 50000);

	CHECK_EQ(machine_run(m, 0), MACHINE_STOP_IDLE);
	CHECK_EQ(w.fired_at, 50000);
	CHECK_EQ(f->executed, 110);

	/* Idle time is skipped in one go: one turn before, one after. */
	CHECK_EQ(f->runs, 2);
	CHECK_EQ(m->events.now, 51000);
	machine_destroy(m);
}

/* Records how many instructions a CPU had run when the event fired. */
typedef struct Probe {
	Event ev;
	FakeCpu *cpu;
	uint64_t executed_then;
} Probe;

static void probe(void *ctx)
{
	Probe *p = ctx;

	p->executed_then = p->cpu->executed;
}

static void test_turn_ends_at_next_event(void)
{
	Machine *m = machine_create();
	FakeCpu *f = add_fake_cpu(m, 3000);
	Probe p = {.cpu = f};

	/* At 1 ns per instruction, the CPU has run exactly 1500 by then. */
	event_init(&p.ev, probe, &p);
	event_schedule(&m->events, &p.ev, 1500);
	CHECK_EQ(machine_run(m, 0), MACHINE_STOP_IDLE);
	CHECK_EQ(p.executed_then, 1500);
	CHECK_EQ(f->executed, 3000);
	machine_destroy(m);
}

static void test_cpus_take_turns(void)
{
	Machine *m = machine_create();
	FakeCpu *a = add_fake_cpu(m, 2500);
	FakeCpu *b = add_fake_cpu(m, 2500);

	num_turns = 0;
	CHECK_EQ(machine_run(m, 0), MACHINE_STOP_IDLE);
	CHECK_EQ(a->executed, 2500);
	CHECK_EQ(b->executed, 2500);
	CHECK_EQ(m->insns, 5000);
	CHECK_EQ(num_turns, 6);
	for (unsigned i = 0; i < 6; i++)
		CHECK_EQ(turns[i], i % 2);
	machine_destroy(m);
}

static void test_halted_and_off_cpus_do_not_run(void)
{
	Machine *m = machine_create();
	FakeCpu *halted = add_fake_cpu(m, UINT64_MAX);
	FakeCpu *off = add_fake_cpu(m, UINT64_MAX);

	halted->cpu.state = CPU_HALTED;
	off->cpu.state = CPU_OFF;
	CHECK_EQ(machine_run(m, 0), MACHINE_STOP_IDLE);
	CHECK_EQ(halted->runs, 0);
	CHECK_EQ(off->runs, 0);
	machine_destroy(m);
}

static void test_error_stops_machine(void)
{
	Machine *m = machine_create();
	FakeCpu *f = add_fake_cpu(m, UINT64_MAX);
	FakeCpu *other = add_fake_cpu(m, UINT64_MAX);

	f->error_at = 42;
	CHECK_EQ(machine_run(m, 0), MACHINE_STOP_ERROR);
	CHECK_EQ(f->executed, 42);
	CHECK(strcmp(m->stop_message, "fake error after 42 instructions") == 0);

	/* The machine stops at once: the next CPU does not get its turn. */
	CHECK_EQ(other->runs, 0);
	machine_destroy(m);
}

static void test_first_stop_reason_wins(void)
{
	Machine *m = machine_create();

	machine_stop(m, MACHINE_STOP_ERROR, "first");
	machine_stop(m, MACHINE_STOP_SHUTDOWN, "second");
	CHECK_EQ(m->stop, MACHINE_STOP_ERROR);
	CHECK(strcmp(m->stop_message, "first") == 0);
	machine_destroy(m);
}

static void test_reset_and_cpu_limit(void)
{
	Machine *m = machine_create();
	FakeCpu *cpus[MACHINE_MAX_CPUS];

	for (unsigned i = 0; i < MACHINE_MAX_CPUS; i++) {
		cpus[i] = add_fake_cpu(m, 1);
		CHECK_EQ(cpus[i]->cpu.index, i);
		CHECK(cpus[i]->cpu.machine == m);
	}

	/* One more than the maximum is refused. */
	Cpu extra;

	cpu_init(&extra, &fake_ops);
	CHECK(!machine_add_cpu(m, &extra));

	machine_reset(m);
	for (unsigned i = 0; i < MACHINE_MAX_CPUS; i++)
		CHECK_EQ(cpus[i]->resets, 1);
	machine_destroy(m);
}

/* Each fake device appends its letter to a log when reset or destroyed. */
static char device_log[64];

static void log_letter(void *ctx)
{
	size_t len = strlen(device_log);

	device_log[len] = *(const char *)ctx;
	device_log[len + 1] = '\0';
}

static void test_devices(void)
{
	static const char letters[] = "abc";
	Machine *m = machine_create();
	FakeCpu *cpu = add_fake_cpu(m, 1);

	/* b has no reset function, c no destroy function. */
	CHECK(machine_add_device(m, (void *)&letters[0], log_letter, log_letter));
	CHECK(machine_add_device(m, (void *)&letters[1], NULL, log_letter));
	CHECK(machine_add_device(m, (void *)&letters[2], log_letter, NULL));

	/* Reset in the order of adding, devices before CPUs. */
	device_log[0] = '\0';
	machine_reset(m);
	CHECK(strcmp(device_log, "ac") == 0);
	CHECK_EQ(cpu->resets, 1);

	/* Destroyed newest first. */
	device_log[0] = '\0';
	machine_destroy(m);
	CHECK(strcmp(device_log, "ba") == 0);
}

static void test_device_limit(void)
{
	Machine *m = machine_create();
	int dummy;

	for (unsigned i = 0; i < MACHINE_MAX_DEVICES; i++)
		CHECK(machine_add_device(m, &dummy, NULL, NULL));
	CHECK(!machine_add_device(m, &dummy, NULL, NULL));
	CHECK_EQ(m->num_devices, MACHINE_MAX_DEVICES);
	machine_destroy(m);
}

static void test_stop_strings(void)
{
	for (int s = MACHINE_STOP_NONE; s <= MACHINE_STOP_ERROR; s++) {
		const char *str = machine_stop_str((MachineStop)s);

		CHECK(str != NULL && str[0] != '\0');
	}
	CHECK(strcmp(machine_stop_str((MachineStop)99), "unknown stop reason") ==
	      0);
}

int main(void)
{
	RUN_TEST(test_no_cpus_is_idle);
	RUN_TEST(test_cpu_halts_then_machine_is_idle);
	RUN_TEST(test_instruction_limit);
	RUN_TEST(test_limit_shared_by_cpus);
	RUN_TEST(test_event_wakes_halted_cpu);
	RUN_TEST(test_turn_ends_at_next_event);
	RUN_TEST(test_cpus_take_turns);
	RUN_TEST(test_halted_and_off_cpus_do_not_run);
	RUN_TEST(test_error_stops_machine);
	RUN_TEST(test_first_stop_reason_wins);
	RUN_TEST(test_reset_and_cpu_limit);
	RUN_TEST(test_devices);
	RUN_TEST(test_device_limit);
	RUN_TEST(test_stop_strings);
	return test_summary();
}
