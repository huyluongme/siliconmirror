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

#define SECOND UINT64_C(1000000000) /* in ns of virtual time */

/* The clocks of the Raspberry Pi 3 Model B. */
static const Bcm2836LocalConfig config = {
	.crystal_hz = 19200000,
	.apb_hz = 600000000,
};

static Machine *m;
static Bcm2836Local *local;
static SystemCounter *counter;

static void setup(void)
{
	m = machine_create();
	local = bcm2836_local_create(m, &config);
	CHECK(m != NULL && local != NULL);
	CHECK_EQ(bcm2836_local_map(local, m->bus, "local", BASE), BUS_OK);
	counter = bcm2836_local_counter(local);
}

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
	value = rd(BASE + 0x40);
	CHECK_EQ(m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message,
	             "bcm2836-local: 4-byte read from offset 0x40 is not "
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
	return test_summary();
}
