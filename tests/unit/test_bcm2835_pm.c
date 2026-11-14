/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835's power manager and its watchdog
 * (src/hw/misc/bcm2835_pm.c), as Linux's and U-Boot's drivers use it.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/event.h"
#include "core/machine.h"
#include "hw/misc/bcm2835_pm.h"
#include "test.h"

#define BASE 0x3f100000
#define PW 0x5a000000u

enum { RSTC = 0x1c, RSTS = 0x20, WDOG = 0x24 };

typedef struct Setup {
	Machine *m;
	Bcm2835Pm *pm;
} Setup;

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->pm = bcm2835_pm_create(s->m);
	CHECK(s->pm != NULL);
	CHECK_EQ(bcm2835_pm_map(s->pm, s->m->bus, "pm", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_pm_destroy(s->pm);
	machine_destroy(s->m);
}

static uint64_t rd(Setup *s, uint64_t offset)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, BASE + offset, 4, &value), BUS_OK);
	return value;
}

static void wr(Setup *s, uint64_t offset, uint64_t value)
{
	CHECK_EQ(bus_write(s->m->bus, BASE + offset, 4, value), BUS_OK);
}

static void advance(Setup *s, uint64_t t)
{
	event_queue_run_until(&s->m->events, t);
}

/* At power on every register is 0; a write needs the password. */
static void test_reset_values_and_password(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, RSTC), 0);
	CHECK_EQ(rd(&s, RSTS), 0);
	CHECK_EQ(rd(&s, WDOG), 0);
	wr(&s, RSTS, 0x00000555);
	wr(&s, RSTS, 0xa5000555);
	CHECK_EQ(rd(&s, RSTS), 0);
	wr(&s, RSTS, PW | 0x00fff555);
	CHECK_EQ(rd(&s, RSTS), 0x00fff555);
	wr(&s, WDOG, PW | 0x00fffff0);
	CHECK_EQ(rd(&s, WDOG), 0x000ffff0); /* 20 bits */
	wr(&s, RSTC, 0x00000020);
	CHECK_EQ(rd(&s, RSTC), 0);
	CHECK(!s.m->reset_pending);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/*
 * U-Boot's reset: 10 ticks, then a full reset. A tick is 1/65536 s, so
 * the board resets 152588 ns later (rounded up).
 */
static void test_watchdog_resets(void)
{
	Setup s;

	setup(&s);
	advance(&s, 1000);
	wr(&s, WDOG, PW | 10);
	CHECK_EQ(rd(&s, WDOG), 10); /* not running yet: it holds */
	advance(&s, 50000);
	CHECK_EQ(rd(&s, WDOG), 10);
	wr(&s, RSTC, PW | (rd(&s, RSTC) & ~0x30u) | 0x20);
	CHECK_EQ(rd(&s, RSTC), 0x20);
	advance(&s, 50000 + 15259);
	CHECK_EQ(rd(&s, WDOG), 9);
	advance(&s, 50000 + 152587);
	CHECK_EQ(rd(&s, WDOG), 1);
	CHECK(!s.m->reset_pending);
	advance(&s, 50000 + 152588);
	CHECK(s.m->reset_pending);
	CHECK_EQ(rd(&s, WDOG), 0);
	CHECK_EQ(rd(&s, RSTC), 0);
	teardown(&s);
}

/* Linux stops it (RSTC 0x102), reads the time left, pings it. */
static void test_stop_and_ping(void)
{
	Setup s;

	setup(&s);
	wr(&s, WDOG, PW | (15u << 16)); /* 15 s */
	wr(&s, RSTC, PW | 0x20);
	advance(&s, 2000000000);
	CHECK_EQ(rd(&s, WDOG) >> 16, 13);
	CHECK_EQ(rd(&s, WDOG), (15u << 16) - 131072);
	wr(&s, RSTC, PW | 0x102); /* stop: it holds what is left */
	CHECK_EQ(rd(&s, RSTC), 0x102);
	advance(&s, 30000000000);
	CHECK_EQ(rd(&s, WDOG), (15u << 16) - 131072);
	CHECK(!s.m->reset_pending);
	/* Started again, it goes on from there. */
	wr(&s, RSTC, PW | 0x20);
	advance(&s, 31000000000);
	CHECK_EQ(rd(&s, WDOG), (15u << 16) - 3 * 65536);
	/* An RSTC write that keeps it running does not change the count. */
	wr(&s, RSTC, PW | 0x22);
	CHECK_EQ(rd(&s, WDOG), (15u << 16) - 3 * 65536);
	/* A ping: a new count, from now. */
	wr(&s, WDOG, PW | (2u << 16));
	advance(&s, 32999999999);
	CHECK(!s.m->reset_pending);
	advance(&s, 33000000000);
	CHECK(s.m->reset_pending);
	teardown(&s);
}

/* The partition, in bits 0, 2, 4, 6, 8 and 10; a reset keeps RSTS. */
static void test_partition(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(bcm2835_pm_partition(s.pm), 0);
	wr(&s, RSTS, PW | 0x555);
	CHECK_EQ(bcm2835_pm_partition(s.pm), BCM2835_PM_HALT_PARTITION);
	wr(&s, RSTS, PW | 0x12aa); /* the odd bits and bit 12 are not part */
	CHECK_EQ(bcm2835_pm_partition(s.pm), 0);
	wr(&s, RSTS, PW | 0x001);
	CHECK_EQ(bcm2835_pm_partition(s.pm), 1);
	wr(&s, RSTS, PW | 0x400);
	CHECK_EQ(bcm2835_pm_partition(s.pm), 32);
	wr(&s, RSTS, PW | 0x014);
	CHECK_EQ(bcm2835_pm_partition(s.pm), 6);

	wr(&s, WDOG, PW | 100);
	wr(&s, RSTC, PW | 0x20);
	bcm2835_pm_reset(s.pm);
	CHECK_EQ(rd(&s, RSTS), 0x014);
	CHECK_EQ(rd(&s, RSTC), 0);
	CHECK_EQ(rd(&s, WDOG), 0);
	advance(&s, 10000000);
	CHECK(!s.m->reset_pending);
	teardown(&s);
}

/* A write changes its register only; WDOG keeps 20 bits. */
static void test_registers_apart(void)
{
	Setup s;

	setup(&s);
	wr(&s, WDOG, PW | 0xffffff);
	CHECK_EQ(rd(&s, WDOG), 0xfffff);
	wr(&s, RSTS, PW | 0x123456);
	wr(&s, RSTC, PW | 0x000101);
	CHECK_EQ(rd(&s, RSTS), 0x123456);
	CHECK_EQ(rd(&s, RSTC), 0x101);
	CHECK_EQ(rd(&s, WDOG), 0xfffff);
	wr(&s, RSTS, PW | 0x000555);
	CHECK_EQ(rd(&s, RSTC), 0x101);
	CHECK_EQ(rd(&s, WDOG), 0xfffff);
	/*
	 * Linux keeps RSTC's other bits when it starts the watchdog: they
	 * do not make it a reset other than full.
	 */
	wr(&s, WDOG, PW | 10);
	wr(&s, RSTC, PW | 0x121);
	CHECK_EQ(rd(&s, RSTC), 0x121);
	advance(&s, 1000000);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	CHECK(s.m->reset_pending);
	teardown(&s);
}

/* Destroyed while it runs, it leaves the event queue. */
static void test_destroy_running(void)
{
	Setup s;

	setup(&s);
	wr(&s, WDOG, PW | 10);
	wr(&s, RSTC, PW | 0x20);
	bcm2835_pm_destroy(s.pm);
	advance(&s, 1000000);
	CHECK(!s.m->reset_pending);
	machine_destroy(s.m);
}

/* What stops the machine. */
static void test_unmodelled(void)
{
	static const struct {
		uint64_t offset;
		unsigned size;
		bool write;
		uint64_t value;
		const char *message;
	} stops[] = {
		{0x000, 4, false, 0, "power domains"},
		{0x110, 4, true, PW, "power domains"},
		{RSTC, 2, false, 0, "32-bit register read"},
		{RSTS + 1, 4, true, PW, "32-bit register write"},
		{RSTC, 4, true, PW | 0x10, "other than full"},
		{RSTC, 4, true, PW | 0x30, "other than full"},
	};

	for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
		Setup s;
		uint64_t value;

		setup(&s);
		if (stops[i].write)
			bus_write(s.m->bus, BASE + stops[i].offset, stops[i].size,
			          stops[i].value);
		else
			bus_read(s.m->bus, BASE + stops[i].offset, stops[i].size, &value);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, "bcm2835-pm: ") != NULL);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		teardown(&s);
	}
}

int main(void)
{
	RUN_TEST(test_reset_values_and_password);
	RUN_TEST(test_watchdog_resets);
	RUN_TEST(test_stop_and_ping);
	RUN_TEST(test_partition);
	RUN_TEST(test_registers_apart);
	RUN_TEST(test_destroy_running);
	RUN_TEST(test_unmodelled);
	return test_summary();
}
