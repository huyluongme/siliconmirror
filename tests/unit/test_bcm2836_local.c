/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the ARM local peripherals of the BCM2836 and BCM2837
 * (src/hw/intc/bcm2836_local.c). Section numbers refer to Broadcom's
 * "BCM2836 ARM-local peripherals", rev 3.4.
 */

#include <stdint.h>
#include <string.h>

#include "hw/intc/bcm2836_local.h"
#include "test.h"

#define BASE 0x40000000
#define CONTROL (BASE + 0x00)
#define PRESCALER (BASE + 0x08)
#define TIMER_LS (BASE + 0x1c)
#define TIMER_MS (BASE + 0x20)
#define GPU_ROUTING (BASE + 0x0c)
#define PMU_SET (BASE + 0x10)
#define PMU_CLEAR (BASE + 0x14)
#define LOCAL_ROUTING (BASE + 0x24)
#define AXI_COUNTERS (BASE + 0x2c)
#define AXI_IRQ (BASE + 0x30)
#define LOCAL_TIMER (BASE + 0x34)
#define LOCAL_FLAGS (BASE + 0x38)
#define TIMER_ROUTING(core) (BASE + 0x40 + 4 * (core))
#define MAILBOX_ROUTING(core) (BASE + 0x50 + 4 * (core))
#define IRQ_SOURCE(core) (BASE + 0x60 + 4 * (core))
#define FIQ_SOURCE(core) (BASE + 0x70 + 4 * (core))
#define MAILBOX_SET(core, n) (BASE + 0x80 + 16 * (core) + 4 * (n))
#define MAILBOX_CLEAR(core, n) (BASE + 0xc0 + 16 * (core) + 4 * (n))

/* Local timer bits, section 4.11. */
#define LT_FLAG (UINT32_C(1) << 31)
#define LT_RELOAD_NOW (1u << 30)
#define LT_IRQ_ENABLE (1u << 29)
#define LT_ENABLE (1u << 28)

#define SECOND UINT64_C(1000000000) /* in ns of virtual time */

/* The clocks of the Raspberry Pi 3 Model B. */
static const Bcm2836LocalConfig config = {
	.crystal_hz = 19200000,
	.apb_hz = 600000000,
};

static Machine *m;
static Bcm2836Local *local;
static SystemCounter *counter;

/* The IRQ and FIQ outputs for each core, as lines with no handler. */
static IrqLine irq_out[4], fiq_out[4];

static void setup(void)
{
	m = machine_create();
	local = bcm2836_local_create(m, &config);
	CHECK(m != NULL && local != NULL);
	CHECK_EQ(bcm2836_local_map(local, m->bus, "local", BASE), BUS_OK);
	counter = bcm2836_local_counter(local);
	for (unsigned core = 0; core < 4; core++) {
		irq_init(&irq_out[core], NULL, NULL, core);
		irq_init(&fiq_out[core], NULL, NULL, core);
		bcm2836_local_connect(local, core, &irq_out[core], &fiq_out[core]);
	}
}

/* Check the outputs: bit n of irq and fiq for core n. */
static void check_outputs(unsigned irq, unsigned fiq, int line)
{
	for (unsigned core = 0; core < 4; core++) {
		if (irq_level(&irq_out[core]) != ((irq >> core & 1) != 0) ||
		    irq_level(&fiq_out[core]) != ((fiq >> core & 1) != 0)) {
			fprintf(stderr, "%s:%d: core %u has IRQ %d FIQ %d\n", __FILE__,
			        line, core, irq_level(&irq_out[core]),
			        irq_level(&fiq_out[core]));
			test_failures++;
		}
	}
}

#define CHECK_OUTPUTS(irq, fiq) check_outputs(irq, fiq, __LINE__)

static void teardown(void)
{
	bcm2836_local_destroy(local);
	machine_destroy(m);
}

static uint32_t rd(uint64_t address)
{
	uint64_t value = 0x5555;

	CHECK_EQ(bus_read(m->bus, address, 4, &value), BUS_OK);
	return (uint32_t)value;
}

static void wr(uint64_t address, uint32_t value)
{
	CHECK_EQ(bus_write(m->bus, address, 4, value), BUS_OK);
}

/* Move virtual time on to t. */
static void advance(uint64_t t)
{
	event_queue_run_until(&m->events, t);
}

/* The count, as the cores see it. */
static uint64_t count(void)
{
	return system_counter_read(counter, machine_now(m));
}

/* Sections 4.2 and 4.3: all reset to 0, and the timer does not count. */
static void test_reset(void)
{
	setup();
	CHECK_EQ(rd(CONTROL), 0);
	CHECK_EQ(rd(PRESCALER), 0);
	CHECK_EQ(rd(TIMER_LS), 0);
	CHECK_EQ(rd(TIMER_MS), 0);
	advance(SECOND);
	CHECK_EQ(count(), 0);
	CHECK_EQ(system_counter_time_of(counter, SECOND, 1), EVENT_NEVER);
	CHECK_EQ(system_counter_time_of(counter, SECOND, 0), SECOND);

	/* After a reset, the settings and the count are gone. */
	wr(CONTROL, 0x300);
	wr(PRESCALER, 0x80000000);
	wr(TIMER_LS, 7);
	wr(TIMER_MS, 0);
	advance(2 * SECOND);
	bcm2836_local_reset(local);
	CHECK_EQ(rd(CONTROL), 0);
	CHECK_EQ(rd(PRESCALER), 0);
	advance(3 * SECOND);
	CHECK_EQ(count(), 0);
	teardown();
}

/* Section 3.1.1: a prescaler of 2^31 divides the crystal by 1. */
static void test_crystal(void)
{
	setup();
	wr(PRESCALER, 0x80000000);
	CHECK_EQ(rd(PRESCALER), 0x80000000);
	advance(SECOND);
	CHECK_EQ(count(), 19200000);

	/* One tick every 1e9 / 19.2e6 = 52.083 ns. */
	CHECK_EQ(system_counter_read(counter, SECOND + 52), 19200000);
	CHECK_EQ(system_counter_read(counter, SECOND + 53), 19200001);
	CHECK_EQ(system_counter_time_of(counter, SECOND, 19200001), SECOND + 53);
	CHECK_EQ(system_counter_time_of(counter, SECOND, 19200000), SECOND);
	CHECK_EQ(system_counter_time_of(counter, 0, 19200000), SECOND);

	/* Beyond the end of virtual time: never. */
	wr(PRESCALER, 1);
	CHECK_EQ(system_counter_time_of(counter, SECOND, UINT64_MAX), EVENT_NEVER);
	teardown();
}

/* Fractional ratios: 2^31 / prescaler. */
static void test_fractions(void)
{
	setup();

	/* The document's example: 0x06AAAAAB divides by 19.2, giving 1 MHz. */
	wr(PRESCALER, 0x06aaaaab);
	advance(SECOND);
	CHECK_EQ(count(), 1000000);
	CHECK_EQ(system_counter_time_of(counter, SECOND, 1000001), 1000001042);

	/* Changing the prescaler keeps the count and the divider's fraction. */
	wr(PRESCALER, 0x40000000); /* divides by 2: 9.6 MHz */
	advance(2 * SECOND);
	CHECK_EQ(count(), 1000000 + 9600000);
	advance(2 * SECOND + 105); /* 2 more crystal ticks: 1 more step */
	CHECK_EQ(count(), 1000000 + 9600001);

	/* The odd crystal tick leaves half a step in the divider. */
	advance(2 * SECOND + 157);
	CHECK_EQ(count(), 1000000 + 9600001);
	wr(PRESCALER, 0x40000000);
	advance(2 * SECOND + 209); /* 1 more tick completes the step */
	CHECK_EQ(count(), 1000000 + 9600002);
	CHECK_EQ(system_counter_time_of(counter, 2 * SECOND + 209, 10600003),
	         2 * SECOND + 313);

	/* A prescaler of 0 stops the timer. */
	wr(PRESCALER, 0);
	advance(3 * SECOND);
	CHECK_EQ(count(), 1000000 + 9600002);
	teardown();
}

/* Section 4.2: bit 8 selects the APB clock, bit 9 steps of 2. */
static void test_control(void)
{
	setup();
	wr(PRESCALER, 0x80000000);
	wr(CONTROL, 0xffffffff);
	CHECK_EQ(rd(CONTROL), 0x300); /* the other bits are reserved */
	advance(SECOND);
	CHECK_EQ(count(), 2 * UINT64_C(600000000));
	CHECK_EQ(system_counter_time_of(counter, SECOND, 1200000001),
	         SECOND + 2); /* 600 MHz: an edge every 1.67 ns */
	CHECK_EQ(system_counter_time_of(counter, SECOND, 1200000002), SECOND + 2);
	wr(CONTROL, 0x100);
	advance(2 * SECOND);
	CHECK_EQ(count(), 3 * UINT64_C(600000000));
	wr(CONTROL, 0x200);
	advance(3 * SECOND);
	CHECK_EQ(count(), 3 * UINT64_C(600000000) + 2 * 19200000);
	teardown();
}

/* Section 3.1.2: the read and write holding registers. */
static void test_timer_access(void)
{
	setup();
	wr(PRESCALER, 0x80000000);
	wr(TIMER_LS, 0xffffff00);
	CHECK_EQ(count(), 0); /* nothing happens before the MS write */
	wr(TIMER_MS, 0x12);
	CHECK_EQ(count(), 0x12ffffff00);

	/* Reading the low half latches the high half. */
	advance(1000); /* 19 ticks */
	CHECK_EQ(rd(TIMER_LS), 0xffffff13);
	CHECK_EQ(rd(TIMER_MS), 0x12);
	advance(20000);                     /* 384 ticks */
	CHECK_EQ(rd(TIMER_MS), 0x12);       /* still the latched value */
	CHECK_EQ(rd(TIMER_LS), 0x00000080); /* carried into bit 32 */
	CHECK_EQ(rd(TIMER_MS), 0x13);

	/* The MS write restarts the divider: here, half a step is lost. */
	wr(PRESCALER, 0x40000000);
	advance(20053); /* one crystal tick: half a step */
	wr(TIMER_LS, 100);
	wr(TIMER_MS, 0);
	advance(20105); /* one more tick: half a step again */
	CHECK_EQ(count(), 100);
	advance(20157);
	CHECK_EQ(count(), 101);
	teardown();
}

/*
 * A core that runs later through the same stretch of time can access the
 * block at a time before the last change: the block's time stays put.
 */
static void test_time_never_goes_back(void)
{
	setup();
	advance(1000);
	wr(PRESCALER, 0x80000000);
	CHECK_EQ(system_counter_read(counter, 500), 0);
	CHECK_EQ(system_counter_time_of(counter, 500, 0), 1000);
	CHECK_EQ(system_counter_time_of(counter, 500, 1), 1042);
	m->events.now = 500;
	wr(CONTROL, 0x200);
	m->events.now = 1100;
	CHECK_EQ(count(), 2 * 2); /* ticks at 1041.7 and 1093.75 */
	teardown();
}

/* The cores hear about every change. */
static uint64_t heard_before, heard_after;
static unsigned heard;

static void listener(void *ctx, uint64_t before, uint64_t after)
{
	(void)ctx;
	heard_before = before;
	heard_after = after;
	heard++;
}

static void test_listeners(void)
{
	setup();
	CHECK(system_counter_listen(counter, listener, NULL));
	wr(PRESCALER, 0x80000000);
	CHECK_EQ(heard, 1);
	advance(SECOND);
	wr(CONTROL, 0x200);
	CHECK_EQ(heard, 2);
	CHECK_EQ(heard_before, 19200000);
	CHECK_EQ(heard_after, 19200000);
	wr(TIMER_LS, 5);
	CHECK_EQ(heard, 2);
	wr(TIMER_MS, 0);
	CHECK_EQ(heard, 3);
	CHECK_EQ(heard_before, 19200000);
	CHECK_EQ(heard_after, 5);
	rd(TIMER_LS);
	CHECK_EQ(heard, 3);
	bcm2836_local_reset(local);
	CHECK_EQ(heard, 4);
	CHECK_EQ(heard_before, 5);
	CHECK_EQ(heard_after, 0);
	teardown();
}

/* What the model does not implement stops the machine. */
static void test_not_implemented(void)
{
	uint64_t value;

	setup();
	value = rd(BASE + 0x28);
	CHECK_EQ(m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message,
	             "bcm2836-local: 4-byte read from offset 0x28 is not "
	             "implemented yet") != NULL);
	m->stop = MACHINE_STOP_NONE;
	wr(BASE + 0x04, 1);
	CHECK(strstr(m->stop_message, "4-byte write to offset 0x04") != NULL);
	m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(bus_read(m->bus, TIMER_LS, 8, &value), BUS_OK);
	CHECK(strstr(m->stop_message, "8-byte read from offset 0x1c") != NULL);
	m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(bus_write(m->bus, CONTROL, 2, 0x300), BUS_OK);
	CHECK(strstr(m->stop_message, "2-byte write to offset 0x00") != NULL);
	CHECK_EQ(rd(CONTROL), 0);

	/* A prescaler above 2^31, which the document says not to use. */
	m->stop = MACHINE_STOP_NONE;
	wr(PRESCALER, 0x80000001);
	CHECK_EQ(m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "prescaler 0x80000001 is above 2^31") !=
	      NULL);
	CHECK_EQ(rd(PRESCALER), 0);
	teardown();
}

/* Section 3.2: every routing bit, mailbox and source resets to 0. */
static void test_routing_reset(void)
{
	setup();
	for (unsigned core = 0; core < 4; core++) {
		CHECK_EQ(rd(TIMER_ROUTING(core)), 0);
		CHECK_EQ(rd(MAILBOX_ROUTING(core)), 0);
		CHECK_EQ(rd(IRQ_SOURCE(core)), 0);
		CHECK_EQ(rd(FIQ_SOURCE(core)), 0);
		for (unsigned n = 0; n < 4; n++)
			CHECK_EQ(rd(MAILBOX_CLEAR(core, n)), 0);
	}
	CHECK_EQ(rd(GPU_ROUTING), 0);
	CHECK_EQ(rd(PMU_SET), 0);
	CHECK_EQ(rd(LOCAL_ROUTING), 0);
	CHECK_EQ(rd(AXI_IRQ), 0);
	CHECK_EQ(rd(LOCAL_TIMER), 0);

	/* Set everything, then reset. */
	for (unsigned core = 0; core < 4; core++) {
		wr(TIMER_ROUTING(core), 0xff);
		wr(MAILBOX_ROUTING(core), 0x0f);
		for (unsigned n = 0; n < 4; n++)
			wr(MAILBOX_SET(core, n), 1);
	}
	wr(GPU_ROUTING, 0xf);
	wr(PMU_SET, 0xff);
	wr(LOCAL_ROUTING, 7);
	wr(AXI_IRQ, 1u << 20);
	wr(LOCAL_TIMER, LT_ENABLE | LT_IRQ_ENABLE | 1);
	advance(1000);
	CHECK_OUTPUTS(0xf, 0x8);
	bcm2836_local_reset(local);
	CHECK_OUTPUTS(0, 0);
	for (unsigned core = 0; core < 4; core++) {
		CHECK_EQ(rd(TIMER_ROUTING(core)), 0);
		CHECK_EQ(rd(MAILBOX_ROUTING(core)), 0);
		CHECK_EQ(rd(MAILBOX_CLEAR(core, 3)), 0);
	}
	CHECK_EQ(rd(GPU_ROUTING), 0);
	CHECK_EQ(rd(PMU_CLEAR), 0);
	CHECK_EQ(rd(LOCAL_ROUTING), 0);
	CHECK_EQ(rd(AXI_IRQ), 0);
	CHECK_EQ(rd(LOCAL_TIMER), 0);
	advance(10000);
	CHECK_EQ(rd(LOCAL_TIMER), 0);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	teardown();
}

/*
 * Unused addresses and write-only registers read as 0; writes to unused
 * addresses and read-only registers change nothing.
 */
static void test_unused_and_read_only(void)
{
	setup();
	wr(BASE + 0x18, 0xffffffff);
	wr(BASE + 0x3c, 0xffffffff);
	wr(IRQ_SOURCE(0), 0xffffffff);
	wr(FIQ_SOURCE(3), 0xffffffff);
	wr(AXI_COUNTERS, 0);
	wr(LOCAL_FLAGS, 0);
	CHECK_EQ(rd(TIMER_ROUTING(0)), 0);
	CHECK_EQ(rd(TIMER_ROUTING(3)), 0);
	CHECK_EQ(rd(MAILBOX_ROUTING(3)), 0);
	CHECK_EQ(rd(BASE + 0x18), 0);
	CHECK_EQ(rd(BASE + 0x3c), 0);
	CHECK_EQ(rd(IRQ_SOURCE(0)), 0);
	CHECK_EQ(rd(FIQ_SOURCE(3)), 0);
	CHECK_EQ(rd(AXI_COUNTERS), 1);
	CHECK_EQ(rd(LOCAL_FLAGS), 0);
	CHECK_EQ(rd(LOCAL_TIMER), 0);
	wr(MAILBOX_SET(1, 2), 5);
	CHECK_EQ(rd(MAILBOX_SET(1, 2)), 0);
	CHECK_EQ(rd(MAILBOX_CLEAR(1, 2)), 5);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	teardown();
}

/* Sections 3.2.1 and 4.6: the timers of a core, to its IRQ or FIQ. */
static void test_timer_routing(void)
{
	IrqLine *cntpns;
	IrqLine line;

	setup();
	cntpns = bcm2836_local_input(local, 1, BCM2836_LOCAL_CNTPNS);
	irq_raise(cntpns);
	CHECK_EQ(rd(IRQ_SOURCE(1)), 0);
	CHECK_OUTPUTS(0, 0);
	wr(TIMER_ROUTING(1), 1u << 1); /* IRQ */
	CHECK_EQ(rd(IRQ_SOURCE(1)), 1u << 1);
	CHECK_EQ(rd(FIQ_SOURCE(1)), 0);
	CHECK_OUTPUTS(0x2, 0);
	irq_init(&line, NULL, NULL, 0);
	bcm2836_local_connect(local, 1, &line, &fiq_out[1]);
	CHECK(irq_level(&line));
	bcm2836_local_connect(local, 1, &irq_out[1], &fiq_out[1]);
	wr(TIMER_ROUTING(1), 1u << 5); /* FIQ */
	CHECK_EQ(rd(IRQ_SOURCE(1)), 0);
	CHECK_EQ(rd(FIQ_SOURCE(1)), 1u << 1);
	CHECK_OUTPUTS(0, 0x2);
	wr(TIMER_ROUTING(1), 1u << 1 | 1u << 5); /* FIQ wins */
	CHECK_EQ(rd(FIQ_SOURCE(1)), 1u << 1);
	CHECK_OUTPUTS(0, 0x2);

	/* A line connected now takes the output's level. */
	irq_init(&line, NULL, NULL, 0);
	bcm2836_local_connect(local, 1, &irq_out[1], &line);
	CHECK(irq_level(&line));
	bcm2836_local_connect(local, 1, &irq_out[1], &fiq_out[1]);

	irq_lower(cntpns);
	CHECK_EQ(rd(FIQ_SOURCE(1)), 0);
	CHECK_OUTPUTS(0, 0);
	wr(TIMER_ROUTING(1), 0xffffffff);
	CHECK_EQ(rd(TIMER_ROUTING(1)), 0xff);

	/* The four timers of core 2, in their bits, and no other core's. */
	wr(TIMER_ROUTING(2), 0x0f);
	for (unsigned i = 0; i < 4; i++) {
		irq_raise(bcm2836_local_input(local, 2, (Bcm2836LocalInput)i));
		CHECK_EQ(rd(IRQ_SOURCE(2)), (2u << i) - 1);
	}
	CHECK_EQ(rd(IRQ_SOURCE(0)), 0);
	CHECK_EQ(rd(IRQ_SOURCE(3)), 0);
	CHECK_OUTPUTS(0x4, 0);
	wr(TIMER_ROUTING(2), 0x90); /* CNTV and CNTPS to FIQ, others off */
	CHECK_EQ(rd(FIQ_SOURCE(2)), 0x9);
	CHECK_EQ(rd(IRQ_SOURCE(2)), 0);
	teardown();
}

/* Sections 3.3, 4.1, 4.7 and 4.8: the mailboxes. */
static void test_mailboxes(void)
{
	setup();
	/* The examples of section 4.1. */
	wr(MAILBOX_SET(2, 3), 0x30840008);
	wr(MAILBOX_SET(2, 3), 0xfc060014);
	CHECK_EQ(rd(MAILBOX_CLEAR(2, 3)), 0xfc86001c);
	wr(MAILBOX_CLEAR(2, 3), 0xffffffff);
	CHECK_EQ(rd(MAILBOX_CLEAR(2, 3)), 0);
	wr(MAILBOX_SET(2, 3), 0x30840008);
	wr(MAILBOX_CLEAR(2, 3), 0xfc060014);
	CHECK_EQ(rd(MAILBOX_CLEAR(2, 3)), 0x00800008);

	/* Not 0, so it interrupts where its routing sends it. */
	CHECK_EQ(rd(IRQ_SOURCE(2)), 0);
	CHECK_OUTPUTS(0, 0);
	wr(MAILBOX_ROUTING(2), 1u << 3);
	CHECK_EQ(rd(IRQ_SOURCE(2)), 1u << 7);
	CHECK_OUTPUTS(0x4, 0);
	wr(MAILBOX_ROUTING(2), 1u << 3 | 1u << 7);
	CHECK_EQ(rd(IRQ_SOURCE(2)), 0);
	CHECK_EQ(rd(FIQ_SOURCE(2)), 1u << 7);
	CHECK_OUTPUTS(0, 0x4);
	wr(MAILBOX_CLEAR(2, 3), 0x00800008);
	CHECK_EQ(rd(FIQ_SOURCE(2)), 0);
	CHECK_OUTPUTS(0, 0);
	wr(MAILBOX_ROUTING(2), 0xffffffff);
	CHECK_EQ(rd(MAILBOX_ROUTING(2)), 0xff);

	/* Bit 5, not bit 4 as the document says, overrides bit 1. */
	wr(MAILBOX_SET(0, 1), 1);
	wr(MAILBOX_ROUTING(0), 1u << 1 | 1u << 4);
	CHECK_EQ(rd(IRQ_SOURCE(0)), 1u << 5);
	wr(MAILBOX_ROUTING(0), 1u << 1 | 1u << 5);
	CHECK_EQ(rd(FIQ_SOURCE(0)), 1u << 5);
	wr(MAILBOX_CLEAR(0, 1), 1);

	/* Each of the sixteen mailboxes has its own register. */
	for (unsigned core = 0; core < 4; core++)
		for (unsigned n = 0; n < 4; n++)
			wr(MAILBOX_SET(core, n), 0x100 * core + n + 1);
	for (unsigned core = 0; core < 4; core++)
		for (unsigned n = 0; n < 4; n++)
			CHECK_EQ(rd(MAILBOX_CLEAR(core, n)), 0x100 * core + n + 1);
	for (unsigned core = 0; core < 4; core++)
		wr(MAILBOX_ROUTING(core), 1u << core);
	for (unsigned core = 0; core < 4; core++)
		CHECK_EQ(rd(IRQ_SOURCE(core)), 1u << (4 + core));
	CHECK_OUTPUTS(0xf, 0);
	teardown();
}

/* Section 4.5: the performance monitors' routing. */
static void test_pmu_routing(void)
{
	setup();
	wr(PMU_SET, 0x30840008);
	CHECK_EQ(rd(PMU_SET), 0x08);
	wr(PMU_SET, 0x14);
	CHECK_EQ(rd(PMU_CLEAR), 0x1c);
	wr(PMU_CLEAR, 0x0c);
	CHECK_EQ(rd(PMU_SET), 0x10);

	irq_raise(bcm2836_local_input(local, 3, BCM2836_LOCAL_PMU));
	CHECK_EQ(rd(IRQ_SOURCE(3)), 0);
	CHECK_EQ(rd(FIQ_SOURCE(0)), 0);
	wr(PMU_SET, 1u << 3);
	CHECK_EQ(rd(IRQ_SOURCE(3)), 1u << 9);
	CHECK_OUTPUTS(0x8, 0);
	wr(PMU_SET, 1u << 7);
	CHECK_EQ(rd(IRQ_SOURCE(3)), 0);
	CHECK_EQ(rd(FIQ_SOURCE(3)), 1u << 9);
	CHECK_OUTPUTS(0, 0x8);
	wr(PMU_CLEAR, 0xff);
	CHECK_OUTPUTS(0, 0);
	teardown();
}

/* Section 4.4: the GPU's IRQ and FIQ, to the core that the routing names. */
static void test_gpu_routing(void)
{
	setup();
	irq_raise(bcm2836_local_gpu_input(local, false));
	CHECK_EQ(rd(IRQ_SOURCE(0)), 1u << 8);
	CHECK_OUTPUTS(0x1, 0);
	wr(GPU_ROUTING, 2);
	CHECK_EQ(rd(IRQ_SOURCE(0)), 0);
	CHECK_EQ(rd(IRQ_SOURCE(2)), 1u << 8);
	CHECK_OUTPUTS(0x4, 0);
	irq_raise(bcm2836_local_gpu_input(local, true));
	CHECK_EQ(rd(FIQ_SOURCE(0)), 1u << 8);
	wr(GPU_ROUTING, 3u << 2 | 2);
	CHECK_EQ(rd(FIQ_SOURCE(0)), 0);
	CHECK_EQ(rd(FIQ_SOURCE(3)), 1u << 8);
	CHECK_EQ(rd(IRQ_SOURCE(3)), 0);
	CHECK_OUTPUTS(0x4, 0x8);
	irq_lower(bcm2836_local_gpu_input(local, false));
	irq_lower(bcm2836_local_gpu_input(local, true));
	CHECK_OUTPUTS(0, 0);
	wr(GPU_ROUTING, 0xffffffff);
	CHECK_EQ(rd(GPU_ROUTING), 0xf);
	teardown();
}

/* The time of the k-th pulse of the local timer, at 38.4 MHz. */
static uint64_t pulse(uint64_t k)
{
	return (k * SECOND + 38400000 - 1) / 38400000;
}

/* Section 4.11: the local timer. */
static void test_local_timer(void)
{
	setup();
	/* Reload 100: the flag sets every 100 pulses, the first at 2605 ns. */
	wr(LOCAL_TIMER, LT_IRQ_ENABLE | LT_ENABLE | 100);
	CHECK_EQ(pulse(100), 2605);
	advance(pulse(100) - 1);
	CHECK_OUTPUTS(0, 0);
	CHECK_EQ(rd(LOCAL_TIMER), LT_IRQ_ENABLE | LT_ENABLE | 100);
	advance(pulse(100));
	CHECK_OUTPUTS(0x1, 0);
	CHECK_EQ(rd(LOCAL_TIMER), LT_FLAG | LT_IRQ_ENABLE | LT_ENABLE | 100);
	CHECK_EQ(rd(IRQ_SOURCE(0)), 1u << 11);

	/* Clearing the flag; it sets again at the next reload. */
	wr(LOCAL_FLAGS, LT_FLAG);
	CHECK_OUTPUTS(0, 0);
	advance(pulse(200) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(pulse(200));
	CHECK_OUTPUTS(0x1, 0);

	/* The flag stays while reloads go by, which keep their pace. */
	advance(10000); /* pulse 384, after the reload at 300 */
	wr(LOCAL_FLAGS, LT_FLAG);
	advance(pulse(400) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(pulse(400));
	CHECK_OUTPUTS(0x1, 0);

	/* Without the interrupt enable, the flag sets but does not interrupt. */
	wr(LOCAL_TIMER, LT_ENABLE | 100);
	CHECK_OUTPUTS(0, 0);
	CHECK_EQ(rd(LOCAL_TIMER), LT_FLAG | LT_ENABLE | 100);
	CHECK_EQ(rd(IRQ_SOURCE(0)), 0);

	/* The routing: to the FIQ of core 3. */
	wr(LOCAL_ROUTING, 0xffffffff);
	CHECK_EQ(rd(LOCAL_ROUTING), 7);
	wr(LOCAL_TIMER, LT_IRQ_ENABLE | LT_ENABLE | 100);
	CHECK_EQ(rd(FIQ_SOURCE(3)), 1u << 11);
	CHECK_OUTPUTS(0, 0x8);
	wr(LOCAL_ROUTING, 1);
	CHECK_OUTPUTS(0x2, 0);

	/* Reloading now (pulse 460) moves the next reload. */
	advance(12000);
	wr(LOCAL_FLAGS, LT_FLAG | LT_RELOAD_NOW);
	advance(pulse(560) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(pulse(560));
	CHECK_OUTPUTS(0x2, 0);

	/* A new reload value applies after the next reload. */
	wr(LOCAL_FLAGS, LT_FLAG);
	wr(LOCAL_TIMER, LT_IRQ_ENABLE | LT_ENABLE | 50);
	advance(pulse(660) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(pulse(660));
	CHECK_OUTPUTS(0x2, 0);
	wr(LOCAL_FLAGS, LT_FLAG);
	advance(pulse(710) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(pulse(710));
	CHECK_OUTPUTS(0x2, 0);

	/* Disabled, it stops; enabled again, it starts from the reload value. */
	wr(LOCAL_FLAGS, LT_FLAG);
	wr(LOCAL_TIMER, LT_IRQ_ENABLE | 50);
	advance(pulse(1000));
	CHECK_OUTPUTS(0, 0);
	wr(LOCAL_TIMER, LT_IRQ_ENABLE | LT_ENABLE | 50);
	advance(pulse(1050) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(pulse(1050));
	CHECK_OUTPUTS(0x2, 0);

	/* A reload value of 0 acts as 1. */
	wr(LOCAL_TIMER, LT_IRQ_ENABLE);
	wr(LOCAL_FLAGS, LT_FLAG);
	wr(LOCAL_TIMER, LT_IRQ_ENABLE | LT_ENABLE);
	advance(pulse(1051) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(pulse(1051));
	CHECK_OUTPUTS(0x2, 0);
	teardown();
}

/* The time of the k-th edge of the 600 MHz APB clock. */
static uint64_t apb(uint64_t k)
{
	return (k * SECOND + 600000000 - 1) / 600000000;
}

/* Section 4.9: the AXI outstanding counters and interrupt. */
static void test_axi(void)
{
	setup();
	CHECK_EQ(rd(AXI_COUNTERS), 1);
	wr(AXI_IRQ, 0xffffffff);
	CHECK_EQ(rd(AXI_IRQ), 0x1fffff);

	/* Time-out 2: 47 APB clocks after the bus goes quiet at 1000 ns. */
	wr(AXI_IRQ, 1u << 20 | 2);
	m->idle_listener(m->idle_ctx, true, 1000);
	advance(apb(600 + 47) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(apb(600 + 47));
	CHECK_OUTPUTS(0x1, 0);
	CHECK_EQ(rd(IRQ_SOURCE(0)), 1u << 10);
	CHECK_EQ(rd(FIQ_SOURCE(0)), 0);
	CHECK_EQ(rd(IRQ_SOURCE(1)), 0);

	/* It stays when the bus gets busy, until the enable bit clears. */
	m->idle_listener(m->idle_ctx, false, 2000);
	CHECK_OUTPUTS(0x1, 0);
	wr(AXI_IRQ, 2);
	CHECK_OUTPUTS(0, 0);

	/* The bus busy again before the time-out: no interrupt. */
	wr(AXI_IRQ, 1u << 20 | 2);
	m->idle_listener(m->idle_ctx, true, 2100);
	m->idle_listener(m->idle_ctx, false, 2110);
	advance(2900);
	CHECK_OUTPUTS(0, 0);
	wr(AXI_IRQ, 2);

	/* Disabled, it does not come. */
	m->idle_listener(m->idle_ctx, true, 3000);
	advance(10000);
	CHECK_OUTPUTS(0, 0);

	/*
	 * An access to the block is a transfer: the wait starts over, even
	 * for the write that enables the interrupt.
	 */
	wr(AXI_IRQ, 1u << 20);
	advance(10100);
	CHECK_OUTPUTS(0, 0);
	m->idle_listener(m->idle_ctx, true, 11000);
	advance(11010);
	CHECK_EQ(rd(CONTROL), 0);
	advance(20000);
	CHECK_OUTPUTS(0, 0);

	/* Time-out 0: 15 clocks. */
	m->idle_listener(m->idle_ctx, true, 30000);
	advance(apb(18000 + 15) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(apb(18000 + 15));
	CHECK_OUTPUTS(0x1, 0);

	/* The largest time-out bit, 19. */
	wr(AXI_IRQ, 0);
	wr(AXI_IRQ, 1u << 20 | 1u << 19);
	m->idle_listener(m->idle_ctx, true, 40000);
	advance(apb(24000 + (UINT64_C(1) << 23) + 15) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(apb(24000 + (UINT64_C(1) << 23) + 15));
	CHECK_OUTPUTS(0x1, 0);

	/* A reset ends it. */
	bcm2836_local_reset(local);
	CHECK_OUTPUTS(0, 0);
	wr(AXI_IRQ, 1u << 20);
	CHECK_OUTPUTS(0, 0);
	teardown();
}

/*
 * While its flag is set, the local timer reloads without an event; when
 * software clears the flag, the next reload keeps the pace.
 */
static void test_local_timer_catches_up(void)
{
	setup();
	wr(LOCAL_TIMER, LT_IRQ_ENABLE | LT_ENABLE | 100);
	advance(pulse(100));
	CHECK_OUTPUTS(0x1, 0);
	advance(pulse(550));
	wr(LOCAL_FLAGS, LT_FLAG);
	advance(pulse(600) - 1);
	CHECK_OUTPUTS(0, 0);
	advance(pulse(600));
	CHECK_OUTPUTS(0x1, 0);
	teardown();
}

/*
 * In a core's turn, the block sees the local timer at the core's time,
 * even before the event of a reload fires; a core that runs later in the
 * turn at an earlier time sees the block's later time.
 */
static void test_local_timer_in_a_turn(void)
{
	Cpu cpu = {0};

	setup();
	wr(LOCAL_TIMER, LT_IRQ_ENABLE | LT_ENABLE | 100);
	m->running = &cpu;

	/* A write sees the reload at pulse 100 too. */
	cpu.executed = 3000; /* pulse 115 */
	wr(LOCAL_FLAGS, LT_RELOAD_NOW);
	CHECK_OUTPUTS(0x1, 0);
	wr(LOCAL_FLAGS, LT_FLAG);
	cpu.executed = pulse(215) - 1;
	CHECK_EQ(rd(LOCAL_TIMER) & LT_FLAG, 0);
	cpu.executed = pulse(215);
	CHECK_EQ(rd(LOCAL_TIMER), LT_FLAG | LT_IRQ_ENABLE | LT_ENABLE | 100);
	CHECK_OUTPUTS(0x1, 0);

	/* At 1000 ns, the block's time is still pulse 215: reload there. */
	cpu.executed = 1000;
	wr(LOCAL_FLAGS, LT_FLAG | LT_RELOAD_NOW);
	CHECK_OUTPUTS(0, 0);
	cpu.executed = pulse(315) - 1;
	CHECK_EQ(rd(LOCAL_TIMER) & LT_FLAG, 0);
	cpu.executed = pulse(315);
	CHECK_EQ(rd(LOCAL_TIMER) & LT_FLAG, LT_FLAG);
	m->running = NULL;
	teardown();
}

int main(void)
{
	RUN_TEST(test_reset);
	RUN_TEST(test_crystal);
	RUN_TEST(test_fractions);
	RUN_TEST(test_control);
	RUN_TEST(test_timer_access);
	RUN_TEST(test_time_never_goes_back);
	RUN_TEST(test_listeners);
	RUN_TEST(test_not_implemented);
	RUN_TEST(test_routing_reset);
	RUN_TEST(test_unused_and_read_only);
	RUN_TEST(test_timer_routing);
	RUN_TEST(test_mailboxes);
	RUN_TEST(test_pmu_routing);
	RUN_TEST(test_gpu_routing);
	RUN_TEST(test_local_timer);
	RUN_TEST(test_axi);
	RUN_TEST(test_local_timer_catches_up);
	RUN_TEST(test_local_timer_in_a_turn);
	return test_summary();
}
