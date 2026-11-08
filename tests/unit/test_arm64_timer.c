/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the Generic Timer of the AArch64 model
 * (src/cpu/arm64/timer.c). Section numbers refer to the Arm Architecture
 * Reference Manual, ARM DDI 0487 M.d.
 *
 * The CPU counts from a test counter that steps once per nanosecond of
 * virtual time, and that a test can stop, restart or set. Time moves on
 * with event_queue_run_until() while the CPU does not run.
 */

#include <stdint.h>

#include "arm64_test.h"
#include "core/counter.h"
#include "core/irq.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"

#define WFE 0xd503205fu
#define EC_SYSREG 0x18

typedef struct TestCounter {
	SystemCounter sc;
	bool stopped;
	uint64_t base_time, base_count;
} TestCounter;

static uint64_t test_read(void *ctx, uint64_t now)
{
	TestCounter *tc = ctx;

	return tc->stopped ? tc->base_count
	                   : tc->base_count + (now - tc->base_time);
}

static uint64_t test_time_of(void *ctx, uint64_t now, uint64_t count)
{
	TestCounter *tc = ctx;
	uint64_t current = test_read(tc, now);

	if (current >= count)
		return now;
	return tc->stopped ? EVENT_NEVER : now + (count - current);
}

static const SystemCounterOps test_counter_ops = {
	.read = test_read,
	.time_of = test_time_of,
};

static TestCounter counter;

/* Set the count to value at time now, then stop or run it. */
static void set_counter(uint64_t now, uint64_t value, bool stopped)
{
	uint64_t before = test_read(&counter, now);

	counter.base_time = now;
	counter.base_count = value;
	counter.stopped = stopped;
	system_counter_changed(&counter.sc, before, value);
}

/* A machine at time 0 with one CPU at EL3 counting from the counter. */
static Machine *machine(Arm64Cpu **c)
{
	Arm64Config config = {
		.reset_pc = RESET_PC,
		.highest_el = 3,
		.counter = &counter.sc,
	};
	Machine *m = machine_create();
	Cpu *base;

	counter = (TestCounter){0};
	system_counter_init(&counter.sc, &test_counter_ops, &counter);
	base = arm64_cpu_create(&config);
	CHECK(m != NULL && base != NULL);
	CHECK_EQ(bus_map_ram(m->bus, "ram", 0, 0x100000), BUS_OK);
	CHECK(machine_add_cpu(m, base));
	*c = arm64_cpu_of(base);
	return m;
}

static void advance(Machine *m, uint64_t t)
{
	event_queue_run_until(&m->events, t);
}

static uint32_t mrs(uint16_t encoding, unsigned t)
{
	return 0xd5200000u | (uint32_t)encoding << 5 | t;
}

static uint32_t msr(uint16_t encoding, unsigned t)
{
	return 0xd5000000u | (uint32_t)encoding << 5 | t;
}

/* MRS X0, <reg> at the current level; returns X0. */
static uint64_t read_reg(Arm64Cpu *c, uint16_t encoding)
{
	c->x[0] = 0x5555;
	CHECK_EQ(arm64_execute(c, mrs(encoding, 0)), ARM64_EXEC_OK);
	return c->x[0];
}

/* MSR <reg>, X1 with X1 = value at the current level. */
static void write_reg(Arm64Cpu *c, uint16_t encoding, uint64_t value)
{
	c->x[1] = value;
	CHECK_EQ(arm64_execute(c, msr(encoding, 1)), ARM64_EXEC_OK);
}

/* Set a register as EL3 would, whatever the current level. */
static void set_reg(Arm64Cpu *c, uint16_t encoding, uint64_t value)
{
	CHECK(arm64_cpu_write_sysreg(c, encoding, value));
}

/* Check that insn traps to target_el as a System register access. */
static void expect_trap(Arm64Cpu *c, uint32_t insn, unsigned target_el)
{
	c->exception.target_el = 0;
	CHECK_EQ(arm64_execute(c, insn), ARM64_EXEC_EXCEPTION);
	CHECK_EQ(c->exception.target_el, target_el);
	CHECK_EQ(c->exception.esr >> 26, EC_SYSREG);
}

static void expect_undefined(Arm64Cpu *c, uint32_t insn)
{
	CHECK_EQ(arm64_execute(c, insn), ARM64_EXEC_UNDEFINED);
}

/* Go to EL in the Non-secure state, or Secure if secure. */
static void enter(Arm64Cpu *c, unsigned el, bool secure)
{
	c->el = el;
	c->sys.scr_el3 = secure ? 0 : ARM64_SCR_NS | ARM64_SCR_RW;
}

/* An interrupt line that records its level. */
static void record_level(void *ctx, unsigned n, bool level)
{
	bool *levels = ctx;

	levels[n] = level;
}

static void test_reset_values(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* A53 TRM 4.3: CNTHCTL_EL2 bits 1:0 set, the rest clear. */
	CHECK_EQ(read_reg(c, ARM64_CNTHCTL_EL2), 3);
	CHECK_EQ(read_reg(c, ARM64_CNTKCTL_EL1), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTFRQ_EL0), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTVOFF_EL2), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTP_CTL_EL0), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTV_CTL_EL0), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTHP_CTL_EL2), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTPS_CTL_EL1), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTP_CVAL_EL0), 0);
	machine_destroy(m);
}

static void test_counts(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	advance(m, 1000);
	CHECK_EQ(read_reg(c, ARM64_CNTPCT_EL0), 1000);
	CHECK_EQ(read_reg(c, ARM64_CNTVCT_EL0), 1000);

	/* D24.10.26: the virtual count is the physical one less CNTVOFF_EL2. */
	write_reg(c, ARM64_CNTVOFF_EL2, 300);
	CHECK_EQ(read_reg(c, ARM64_CNTVCT_EL0), 700);
	write_reg(c, ARM64_CNTVOFF_EL2, 2000);
	CHECK_EQ(read_reg(c, ARM64_CNTVCT_EL0), (uint64_t)-1000);

	/* The offset applies at every level without FEAT_VHE. */
	enter(c, 1, false);
	CHECK_EQ(read_reg(c, ARM64_CNTVCT_EL0), (uint64_t)-1000);
	enter(c, 2, false);
	CHECK_EQ(read_reg(c, ARM64_CNTVCT_EL0), (uint64_t)-1000);

	/* Neither count can be written. */
	expect_undefined(c, msr(ARM64_CNTPCT_EL0, 0));
	expect_undefined(c, msr(ARM64_CNTVCT_EL0, 0));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_CNTPCT_EL0, 0));
	CHECK(!arm64_cpu_write_sysreg(c, ARM64_CNTVCT_EL0, 0));

	/* Other encodings are not Generic Timer registers. */
	uint64_t value;

	CHECK(!arm64_timer_read(c, ARM64_SCTLR_EL1, &value));
	CHECK(!arm64_timer_write(c, ARM64_SCTLR_EL1, 0));
	machine_destroy(m);
}

/* Each instruction reads the count at its own time in the CPU's turn. */
static void test_count_in_a_turn(void)
{
	static const uint32_t program[] = {
		0xd53be020, /* MRS X0, CNTPCT_EL0 */
		0xd503201f, /* NOP */
		0xd53be021, /* MRS X1, CNTPCT_EL0 */
	};
	Arm64Cpu *c;
	Machine *m = machine(&c);

	for (unsigned i = 0; i < 3; i++)
		CHECK_EQ(bus_write(m->bus, RESET_PC + 4 * i, 4, program[i]), BUS_OK);
	CHECK_EQ(machine_run(m, 3), MACHINE_STOP_LIMIT);
	CHECK_EQ(c->x[0], 0);
	CHECK_EQ(c->x[1], 2 * MACHINE_NS_PER_INSN);
	machine_destroy(m);
}

static void test_no_counter(void)
{
	static const Arm64Config config = {.reset_pc = RESET_PC, .highest_el = 3};
	Machine *m = machine_create();
	Cpu *base = arm64_cpu_create(&config);
	Arm64Cpu *c = arm64_cpu_of(base);
	bool level = false;
	IrqLine line;

	CHECK(machine_add_cpu(m, base));
	irq_init(&line, record_level, &level, 0);
	arm64_cpu_connect_timer(c, ARM64_TIMER_PHYS, &line);
	advance(m, 1000);
	CHECK_EQ(read_reg(c, ARM64_CNTPCT_EL0), 0);

	/* The count stays 0: a CompareValue of 1 is never met. */
	write_reg(c, ARM64_CNTP_CVAL_EL0, 1);
	write_reg(c, ARM64_CNTP_CTL_EL0, 1);
	advance(m, 5000);
	CHECK(!level);
	CHECK_EQ(event_queue_next(&m->events), EVENT_NEVER);
	write_reg(c, ARM64_CNTP_CVAL_EL0, 0);
	CHECK(level);

	/* A NULL counter is at 0 from the start, and stays there. */
	CHECK_EQ(system_counter_time_of(NULL, 5, 0), 5);
	CHECK_EQ(system_counter_time_of(NULL, 5, 1), EVENT_NEVER);
	CHECK(system_counter_listen(NULL, NULL, NULL));
	machine_destroy(m);
}

static void test_cntfrq(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* Bits 63:32 are RES0. */
	write_reg(c, ARM64_CNTFRQ_EL0, 0xabcd00000124f800);
	CHECK_EQ(read_reg(c, ARM64_CNTFRQ_EL0), 0x0124f800);

	/* D24.10.1: only the highest Exception level writes it. */
	enter(c, 2, false);
	expect_undefined(c, msr(ARM64_CNTFRQ_EL0, 1));
	CHECK_EQ(read_reg(c, ARM64_CNTFRQ_EL0), 0x0124f800);
	enter(c, 1, false);
	expect_undefined(c, msr(ARM64_CNTFRQ_EL0, 1));
	CHECK_EQ(read_reg(c, ARM64_CNTFRQ_EL0), 0x0124f800);

	/* EL0 reads it when it may read either count. */
	enter(c, 0, false);
	expect_undefined(c, msr(ARM64_CNTFRQ_EL0, 1));
	expect_trap(c, mrs(ARM64_CNTFRQ_EL0, 0), 1);
	set_reg(c, ARM64_CNTKCTL_EL1, 1); /* EL0PCTEN */
	CHECK_EQ(read_reg(c, ARM64_CNTFRQ_EL0), 0x0124f800);
	set_reg(c, ARM64_CNTKCTL_EL1, 2); /* EL0VCTEN */
	CHECK_EQ(read_reg(c, ARM64_CNTFRQ_EL0), 0x0124f800);

	/* With HCR_EL2.TGE, the EL0 trap goes to EL2. */
	set_reg(c, ARM64_CNTKCTL_EL1, 0);
	c->sys.hcr_el2 = ARM64_HCR_TGE | ARM64_HCR_RW;
	expect_trap(c, mrs(ARM64_CNTFRQ_EL0, 0), 2);
	machine_destroy(m);
}

static void test_count_traps(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* D24.10.17: CNTPCT_EL0 at EL0 needs CNTKCTL_EL1.EL0PCTEN... */
	enter(c, 0, false);
	expect_trap(c, mrs(ARM64_CNTPCT_EL0, 0), 1);
	set_reg(c, ARM64_CNTKCTL_EL1, 1);
	read_reg(c, ARM64_CNTPCT_EL0);

	/* ...and, when EL2 is enabled, CNTHCTL_EL2.EL1PCTEN at EL0 and EL1. */
	set_reg(c, ARM64_CNTHCTL_EL2, 2);
	expect_trap(c, mrs(ARM64_CNTPCT_EL0, 0), 2);
	enter(c, 1, false);
	expect_trap(c, mrs(ARM64_CNTPCT_EL0, 0), 2);
	enter(c, 1, true); /* Secure: EL2 is not enabled */
	read_reg(c, ARM64_CNTPCT_EL0);
	enter(c, 2, false);
	read_reg(c, ARM64_CNTPCT_EL0);

	/* D24.10.26: CNTVCT_EL0 only needs EL0VCTEN at EL0. */
	enter(c, 0, false);
	expect_trap(c, mrs(ARM64_CNTVCT_EL0, 0), 1);
	set_reg(c, ARM64_CNTKCTL_EL1, 2);
	read_reg(c, ARM64_CNTVCT_EL0);
	expect_trap(c, mrs(ARM64_CNTPCT_EL0, 0), 1);
	enter(c, 1, false);
	read_reg(c, ARM64_CNTVCT_EL0);
	machine_destroy(m);
}

static void test_timer_traps(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* D24.10.22: the EL1 physical timer at EL0 needs EL0PTEN... */
	enter(c, 0, false);
	expect_trap(c, mrs(ARM64_CNTP_CTL_EL0, 0), 1);
	expect_trap(c, msr(ARM64_CNTP_CVAL_EL0, 0), 1);
	expect_trap(c, mrs(ARM64_CNTP_TVAL_EL0, 0), 1);
	set_reg(c, ARM64_CNTKCTL_EL1, 1u << 9);
	read_reg(c, ARM64_CNTP_CTL_EL0);
	write_reg(c, ARM64_CNTP_CVAL_EL0, 7);

	/* ...and CNTHCTL_EL2.EL1PCEN at EL0 and EL1 when EL2 is enabled. */
	set_reg(c, ARM64_CNTHCTL_EL2, 1);
	expect_trap(c, mrs(ARM64_CNTP_CTL_EL0, 0), 2);
	enter(c, 1, false);
	expect_trap(c, msr(ARM64_CNTP_TVAL_EL0, 0), 2);
	enter(c, 1, true);
	CHECK_EQ(read_reg(c, ARM64_CNTP_CVAL_EL0), 7);

	/* D24.10.28: the virtual timer at EL0 needs EL0VTEN, and that is all. */
	enter(c, 0, false);
	expect_trap(c, mrs(ARM64_CNTV_CTL_EL0, 0), 1);
	set_reg(c, ARM64_CNTKCTL_EL1, 1u << 8);
	write_reg(c, ARM64_CNTV_CVAL_EL0, 9);
	CHECK_EQ(read_reg(c, ARM64_CNTV_CVAL_EL0), 9);
	expect_trap(c, mrs(ARM64_CNTP_CTL_EL0, 0), 1);
	set_reg(c, ARM64_CNTHCTL_EL2, 0);
	enter(c, 1, false);
	read_reg(c, ARM64_CNTV_TVAL_EL0);

	/* CNTKCTL_EL1 from EL1; the EL2 registers from EL2. */
	read_reg(c, ARM64_CNTKCTL_EL1);
	expect_undefined(c, mrs(ARM64_CNTHCTL_EL2, 0));
	expect_undefined(c, mrs(ARM64_CNTVOFF_EL2, 0));
	expect_undefined(c, mrs(ARM64_CNTHP_CTL_EL2, 0));
	enter(c, 0, false);
	expect_undefined(c, mrs(ARM64_CNTKCTL_EL1, 0));
	expect_undefined(c, msr(ARM64_CNTHP_CVAL_EL2, 0));
	enter(c, 2, false);
	read_reg(c, ARM64_CNTHP_CTL_EL2);
	read_reg(c, ARM64_CNTHCTL_EL2);
	machine_destroy(m);
}

/* D24.10.19: CNTPS_* belong to EL3 and, with SCR_EL3.ST, Secure EL1. */
static void test_secure_timer_access(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	read_reg(c, ARM64_CNTPS_CTL_EL1);
	enter(c, 0, true);
	expect_undefined(c, mrs(ARM64_CNTPS_CTL_EL1, 0));
	enter(c, 1, false);
	expect_undefined(c, mrs(ARM64_CNTPS_CTL_EL1, 0));
	c->sys.scr_el3 |= ARM64_SCR_ST;
	expect_undefined(c, msr(ARM64_CNTPS_CVAL_EL1, 0));
	enter(c, 2, false);
	expect_undefined(c, mrs(ARM64_CNTPS_TVAL_EL1, 0));
	enter(c, 1, true);
	expect_trap(c, mrs(ARM64_CNTPS_CTL_EL1, 0), 3);
	expect_trap(c, msr(ARM64_CNTPS_CVAL_EL1, 0), 3);
	c->sys.scr_el3 |= ARM64_SCR_ST;
	write_reg(c, ARM64_CNTPS_CVAL_EL1, 5);
	CHECK_EQ(read_reg(c, ARM64_CNTPS_CVAL_EL1), 5);
	machine_destroy(m);

	/* Without EL3 there is no secure timer. */
	static const Arm64Config no_el3 = {.reset_pc = 0, .highest_el = 2};
	Cpu *base = arm64_cpu_create(&no_el3);

	c = arm64_cpu_of(base);
	expect_undefined(c, mrs(ARM64_CNTPS_CTL_EL1, 0));

	/* Outside a machine, nothing is scheduled. */
	write_reg(c, ARM64_CNTHCTL_EL2, 1u << 2);
	write_reg(c, ARM64_CNTHP_CTL_EL2, 1);
	write_reg(c, ARM64_CNTHP_CVAL_EL2, 10);
	c->el = 1;
	expect_undefined(c, mrs(ARM64_CNTPS_CTL_EL1, 0));
	base->ops->destroy(base);
}

static void test_register_values(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* RES0 bits read as 0. */
	write_reg(c, ARM64_CNTKCTL_EL1, UINT64_MAX);
	CHECK_EQ(read_reg(c, ARM64_CNTKCTL_EL1), 0x3ff);
	write_reg(c, ARM64_CNTHCTL_EL2, UINT64_MAX);
	CHECK_EQ(read_reg(c, ARM64_CNTHCTL_EL2), 0xff);
	write_reg(c, ARM64_CNTHCTL_EL2, 0);
	write_reg(c, ARM64_CNTVOFF_EL2, UINT64_MAX);
	CHECK_EQ(read_reg(c, ARM64_CNTVOFF_EL2), UINT64_MAX);
	write_reg(c, ARM64_CNTVOFF_EL2, 0);

	/* ISTATUS cannot be written; it reads 0 while the timer is off. */
	write_reg(c, ARM64_CNTP_CTL_EL0, UINT64_MAX & ~UINT64_C(1));
	CHECK_EQ(read_reg(c, ARM64_CNTP_CTL_EL0), 2);
	write_reg(c, ARM64_CNTP_CTL_EL0, 1);
	CHECK_EQ(read_reg(c, ARM64_CNTP_CTL_EL0), 5); /* 0 >= CVAL 0: met */

	/* D12.2.4.2: TimerValue is CompareValue - count, 32 bits. */
	advance(m, 1000);
	write_reg(c, ARM64_CNTP_CVAL_EL0, 1500);
	CHECK_EQ(read_reg(c, ARM64_CNTP_TVAL_EL0), 500);
	advance(m, 1600);
	CHECK_EQ(read_reg(c, ARM64_CNTP_TVAL_EL0), (uint32_t)-100);

	/* Writing it sets CompareValue to count + SignExtend(value). */
	write_reg(c, ARM64_CNTP_TVAL_EL0, 0xffffffff00000040);
	CHECK_EQ(read_reg(c, ARM64_CNTP_CVAL_EL0), 1600 + 0x40);
	write_reg(c, ARM64_CNTP_TVAL_EL0, 0xfffffff0);
	CHECK_EQ(read_reg(c, ARM64_CNTP_CVAL_EL0), 1600 - 16);

	/* The virtual timer counts from the virtual count. */
	write_reg(c, ARM64_CNTVOFF_EL2, 600);
	write_reg(c, ARM64_CNTV_TVAL_EL0, 10);
	CHECK_EQ(read_reg(c, ARM64_CNTV_CVAL_EL0), 1010);
	write_reg(c, ARM64_CNTV_CVAL_EL0, 1100);
	CHECK_EQ(read_reg(c, ARM64_CNTV_TVAL_EL0), 100);

	/* The other timers have no offset. */
	write_reg(c, ARM64_CNTHP_TVAL_EL2, 10);
	CHECK_EQ(read_reg(c, ARM64_CNTHP_CVAL_EL2), 1610);
	write_reg(c, ARM64_CNTPS_TVAL_EL1, 20);
	CHECK_EQ(read_reg(c, ARM64_CNTPS_CVAL_EL1), 1620);
	CHECK_EQ(read_reg(c, ARM64_CNTPS_TVAL_EL1), 20);
	machine_destroy(m);
}

/* Each timer drives its own output: ENABLE, ISTATUS and not IMASK. */
static void test_outputs(void)
{
	static const struct {
		Arm64TimerId id;
		uint16_t ctl, cval;
	} timers[] = {
		{ARM64_TIMER_PHYS, ARM64_CNTP_CTL_EL0, ARM64_CNTP_CVAL_EL0},
		{ARM64_TIMER_VIRT, ARM64_CNTV_CTL_EL0, ARM64_CNTV_CVAL_EL0},
		{ARM64_TIMER_HYP, ARM64_CNTHP_CTL_EL2, ARM64_CNTHP_CVAL_EL2},
		{ARM64_TIMER_SECURE, ARM64_CNTPS_CTL_EL1, ARM64_CNTPS_CVAL_EL1},
	};
	bool levels[ARM64_NUM_TIMERS] = {false};
	IrqLine lines[ARM64_NUM_TIMERS];

	for (unsigned i = 0; i < ARM64_NUM_TIMERS; i++) {
		Arm64Cpu *c;
		Machine *m = machine(&c);
		unsigned id = timers[i].id;

		for (unsigned j = 0; j < ARM64_NUM_TIMERS; j++) {
			irq_init(&lines[j], record_level, levels, j);
			arm64_cpu_connect_timer(c, timers[j].id, &lines[j]);
		}
		write_reg(c, timers[i].cval, 500);
		write_reg(c, timers[i].ctl, 1);
		advance(m, 499);
		CHECK(!levels[id]);
		CHECK_EQ(read_reg(c, timers[i].ctl), 1);
		advance(m, 500);
		CHECK(levels[id]);
		CHECK_EQ(read_reg(c, timers[i].ctl), 5);
		for (unsigned j = 0; j < ARM64_NUM_TIMERS; j++)
			CHECK(j == id || !levels[j]);

		/* IMASK masks the output, not ISTATUS. */
		write_reg(c, timers[i].ctl, 3);
		CHECK(!levels[id]);
		CHECK_EQ(read_reg(c, timers[i].ctl), 7);
		write_reg(c, timers[i].ctl, 1);
		CHECK(levels[id]);

		/* A later CompareValue clears it until the count gets there. */
		write_reg(c, timers[i].cval, 800);
		CHECK(!levels[id]);
		advance(m, 799);
		CHECK(!levels[id]);
		advance(m, 800);
		CHECK(levels[id]);

		/* Disabled, the output is low and nothing is pending. */
		write_reg(c, timers[i].cval, 900);
		write_reg(c, timers[i].ctl, 0);
		CHECK(!levels[id]);
		CHECK_EQ(event_queue_next(&m->events), EVENT_NEVER);

		/* Connecting a new line gives it the current level. */
		write_reg(c, timers[i].cval, 0);
		write_reg(c, timers[i].ctl, 1);
		arm64_cpu_connect_timer(c, timers[i].id, NULL);
		irq_init(&lines[id], record_level, levels, id);
		levels[id] = false;
		arm64_cpu_connect_timer(c, timers[i].id, &lines[id]);
		CHECK(levels[id]);
		write_reg(c, timers[i].ctl, 3); /* IMASK */
		irq_init(&lines[id], record_level, levels, id);
		levels[id] = true;
		lines[id].level = true;
		arm64_cpu_connect_timer(c, timers[i].id, &lines[id]);
		CHECK(!levels[id]);
		machine_destroy(m);
		levels[id] = false;
	}
}

/* The virtual timer meets its condition at CompareValue + CNTVOFF_EL2. */
static void test_virtual_offset(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	bool level = false;
	IrqLine line;

	irq_init(&line, record_level, &level, 0);
	arm64_cpu_connect_timer(c, ARM64_TIMER_VIRT, &line);

	/* At count 0, an offset of 100 makes the virtual count wrap. */
	write_reg(c, ARM64_CNTVOFF_EL2, 100);
	write_reg(c, ARM64_CNTV_CTL_EL0, 1);
	CHECK(level);
	write_reg(c, ARM64_CNTV_CTL_EL0, 0);
	advance(m, 200);

	write_reg(c, ARM64_CNTV_CVAL_EL0, 400);
	write_reg(c, ARM64_CNTV_CTL_EL0, 1);
	advance(m, 499);
	CHECK(!level);
	advance(m, 500);
	CHECK(level);

	/* A larger offset moves the virtual count back. */
	write_reg(c, ARM64_CNTVOFF_EL2, 200);
	CHECK(!level);
	advance(m, 600);
	CHECK(level);

	/* A CompareValue that the count cannot reach before wrapping. */
	write_reg(c, ARM64_CNTVOFF_EL2, 0);
	write_reg(c, ARM64_CNTV_CVAL_EL0, UINT64_MAX);
	CHECK(!level);
	CHECK_EQ(event_queue_next(&m->events), EVENT_NEVER);
	advance(m, 2000);
	write_reg(c, ARM64_CNTVOFF_EL2, 1000);
	write_reg(c, ARM64_CNTV_CVAL_EL0, UINT64_MAX - 10);
	CHECK(!level);
	CHECK_EQ(event_queue_next(&m->events), EVENT_NEVER);
	machine_destroy(m);
}

/* The timers follow the counter when it stops, restarts or jumps. */
static void test_counter_changes(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	bool level = false;
	IrqLine line;

	irq_init(&line, record_level, &level, 0);
	arm64_cpu_connect_timer(c, ARM64_TIMER_PHYS, &line);
	write_reg(c, ARM64_CNTP_CVAL_EL0, 1000);
	write_reg(c, ARM64_CNTP_CTL_EL0, 1);
	CHECK_EQ(event_queue_next(&m->events), 1000);

	/* Stopped at 400: never. Restarted at time 2000: at 2600. */
	advance(m, 400);
	set_counter(400, 400, true);
	CHECK_EQ(event_queue_next(&m->events), EVENT_NEVER);
	advance(m, 2000);
	CHECK(!level);
	set_counter(2000, 400, false);
	CHECK_EQ(event_queue_next(&m->events), 2600);
	advance(m, 2600);
	CHECK(level);

	/* Jumping back clears the output; jumping forward sets it. */
	set_counter(2600, 0, false);
	CHECK(!level);
	set_counter(2600, 5000, false);
	CHECK(level);
	machine_destroy(m);
}

/* Halt the CPU in WFE, with the Event Register clear. */
static void wait_for_event(Arm64Cpu *c)
{
	c->event_register = false;
	CHECK_EQ(arm64_execute(c, WFE), ARM64_EXEC_OK);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
}

static void test_event_streams(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* CNTKCTL_EL1: EVNTEN, bit 2 of the virtual count, 0 to 1: at 4 + 8k. */
	write_reg(c, ARM64_CNTKCTL_EL1, 2u << 4 | 1u << 2);
	wait_for_event(c);
	advance(m, 3);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	advance(m, 4);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	CHECK(!c->event_register); /* the WFE completes with it clear */
	advance(m, 11);
	CHECK(!c->event_register);
	advance(m, 12);
	CHECK(c->event_register); /* a running CPU keeps the event */

	/* EVNTDIR: 1 to 0 transitions, at 8k. CNTVOFF_EL2 moves them by 3. */
	write_reg(c, ARM64_CNTKCTL_EL1, 2u << 4 | 1u << 3 | 1u << 2);
	write_reg(c, ARM64_CNTVOFF_EL2, 3);
	wait_for_event(c);
	advance(m, 18);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	advance(m, 19);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);

	/* EVNTEN clear: no more events. */
	write_reg(c, ARM64_CNTKCTL_EL1, 0);
	CHECK_EQ(event_queue_next(&m->events), EVENT_NEVER);

	/* CNTHCTL_EL2: the physical count, bit 0, 0 to 1: at odd counts. */
	write_reg(c, ARM64_CNTHCTL_EL2, 1u << 2 | 3);
	wait_for_event(c);
	advance(m, 20);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	advance(m, 21);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);

	/* A jump of the count that changes the bit is an event too. */
	write_reg(c, ARM64_CNTHCTL_EL2, 4u << 4 | 1u << 2 | 3);
	wait_for_event(c);
	set_counter(21, 0x05, true); /* from 0x15, bit 4: 1 to 0 */
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	set_counter(21, 0x13, true); /* bit 4: 0 to 1 */
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	wait_for_event(c);
	set_counter(21, 0x2f, true); /* bit 4 stays 0 */
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	set_counter(21, 0x30, true); /* bit 4: 0 to 1 */
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	write_reg(c, ARM64_CNTHCTL_EL2, 4u << 4 | 1u << 3 | 1u << 2 | 3);
	wait_for_event(c);
	set_counter(21, 0x40, true); /* bit 4: 1 to 0 */
	CHECK_EQ(c->cpu.state, CPU_RUNNING);

	/* With EVNTEN clear, a jump is no event either. */
	write_reg(c, ARM64_CNTHCTL_EL2, 4u << 4 | 3);
	wait_for_event(c);
	set_counter(21, 0x50, true); /* bit 4: 0 to 1 */
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	c->cpu.state = CPU_RUNNING;

	/* A stream follows the counter: stopped, it has no next event. */
	write_reg(c, ARM64_CNTHCTL_EL2, 1u << 2 | 3);
	set_counter(21, 0x50, false);
	CHECK_EQ(event_queue_next(&m->events), 22);
	set_counter(21, 0x50, true);
	CHECK_EQ(event_queue_next(&m->events), EVENT_NEVER);

	/* No more transitions before the count wraps: no event. */
	write_reg(c, ARM64_CNTHCTL_EL2, 4u << 4 | 1u << 3 | 1u << 2 | 3);
	set_counter(21, UINT64_MAX - 3, false);
	CHECK_EQ(event_queue_next(&m->events), EVENT_NEVER);
	machine_destroy(m);
}

static void test_reset(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	bool level = false;
	IrqLine line;

	irq_init(&line, record_level, &level, 0);
	arm64_cpu_connect_timer(c, ARM64_TIMER_HYP, &line);
	write_reg(c, ARM64_CNTHP_CTL_EL2, 1);
	CHECK(level);
	write_reg(c, ARM64_CNTKCTL_EL1, 1u << 2);
	write_reg(c, ARM64_CNTHCTL_EL2, 1u << 2);
	write_reg(c, ARM64_CNTP_CVAL_EL0, 100);
	write_reg(c, ARM64_CNTP_CTL_EL0, 1);

	write_reg(c, ARM64_CNTFRQ_EL0, 19200000);
	write_reg(c, ARM64_CNTVOFF_EL2, 5);

	/* Reset turns the output off and forgets the pending events. */
	machine_reset(m);
	CHECK(!level);
	CHECK_EQ(event_queue_next(&m->events), EVENT_NEVER);
	CHECK_EQ(read_reg(c, ARM64_CNTFRQ_EL0), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTKCTL_EL1), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTHCTL_EL2), 3);
	CHECK_EQ(read_reg(c, ARM64_CNTVOFF_EL2), 0);
	CHECK_EQ(read_reg(c, ARM64_CNTP_CTL_EL0), 0);
	machine_destroy(m);
}

/* A counter has room for SYSTEM_COUNTER_MAX_LISTENERS CPUs. */
static void test_too_many_cpus(void)
{
	Arm64Config config = {.highest_el = 3, .counter = &counter.sc};
	Cpu *cpus[SYSTEM_COUNTER_MAX_LISTENERS];

	system_counter_init(&counter.sc, &test_counter_ops, &counter);
	for (unsigned i = 0; i < SYSTEM_COUNTER_MAX_LISTENERS; i++) {
		cpus[i] = arm64_cpu_create(&config);
		CHECK(cpus[i] != NULL);
	}
	CHECK(arm64_cpu_create(&config) == NULL);
	for (unsigned i = 0; i < SYSTEM_COUNTER_MAX_LISTENERS; i++)
		cpus[i]->ops->destroy(cpus[i]);
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_counts);
	RUN_TEST(test_count_in_a_turn);
	RUN_TEST(test_no_counter);
	RUN_TEST(test_cntfrq);
	RUN_TEST(test_count_traps);
	RUN_TEST(test_timer_traps);
	RUN_TEST(test_secure_timer_access);
	RUN_TEST(test_register_values);
	RUN_TEST(test_outputs);
	RUN_TEST(test_virtual_offset);
	RUN_TEST(test_counter_changes);
	RUN_TEST(test_event_streams);
	RUN_TEST(test_reset);
	RUN_TEST(test_too_many_cpus);
	return test_summary();
}
