/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835's temperature sensor
 * (src/hw/misc/bcm2835_thermal.c), as Linux's bcm2835_thermal.c reads
 * it with the coefficients of the Pi 3 Model B's device tree.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/machine.h"
#include "hw/misc/bcm2835_thermal.h"
#include "test.h"

#define BASE 0x3f212000

/* Linux's bcm2835_thermal.c */
#define CTL_RSTB (1u << 1)
#define CTL_CTRL_SHIFT 2
#define CTL_THOLD_SHIFT 8
#define CTL_RSTDELAY_SHIFT 18
#define CTL_REGULEN (1u << 26)
#define STAT_VALID (1u << 10)
#define STAT_DATA 0x3ffu

/* The device tree's thermal zone: coefficients and critical trip. */
#define SLOPE (-538)
#define OFFSET 412000
#define CRIT 110000

typedef struct Setup {
	Machine *m;
	Bcm2835Thermal *t;
} Setup;

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->t = bcm2835_thermal_create(s->m);
	CHECK(s->t != NULL);
	CHECK_EQ(bcm2835_thermal_map(s->t, s->m->bus, "thermal", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_thermal_destroy(s->t);
	machine_destroy(s->m);
}

static uint64_t rd(Setup *s, uint64_t offset)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, BASE + offset, 4, &value), BUS_OK);
	return value;
}

static void check_stop(Setup *s, const char *message)
{
	CHECK_EQ(s->m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s->m->stop_message, "bcm2835-thermal: ") != NULL);
	CHECK(strstr(s->m->stop_message, message) != NULL);
	s->m->stop = MACHINE_STOP_NONE;
}

/*
 * The firmware set the sensor up as the driver would: Linux leaves it,
 * and reads 45.084 C.
 */
static void test_linux(void)
{
	uint32_t ctl =
		(1u << CTL_CTRL_SHIFT) | CTL_REGULEN | (0xfeu << CTL_RSTDELAY_SHIFT) |
		((uint32_t)((CRIT - OFFSET) / SLOPE) << CTL_THOLD_SHIFT) | CTL_RSTB;
	uint64_t stat;
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, 0), ctl);
	CHECK_EQ(ctl, 0x07fa3106u);
	stat = rd(&s, 4);
	CHECK(stat & STAT_VALID);
	CHECK_EQ(stat, STAT_VALID | 682);
	CHECK_EQ(OFFSET + SLOPE * (int)(stat & STAT_DATA), 45084);
	CHECK_EQ(rd(&s, 4), stat);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

static void test_stops(void)
{
	Setup s;
	uint64_t value;

	setup(&s);
	CHECK_EQ(bus_write(s.m->bus, BASE, 4, 0x07fa3106u), BUS_OK);
	check_stop(&s, "4-byte write to offset 0x0 is not emulated: the "
	               "firmware set the sensor up");
	CHECK_EQ(bus_write(s.m->bus, BASE + 4, 4, 0), BUS_OK);
	check_stop(&s, "write to offset 0x4 is not emulated");
	CHECK_EQ(bus_read(s.m->bus, BASE + 4, 2, &value), BUS_OK);
	check_stop(&s, "2-byte read of offset 0x4 is not a 32-bit register read");
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_read(s.m->bus, BASE + 1, 4, &value), BUS_OK);
	check_stop(&s, "read of offset 0x1 is not a 32-bit register read");
	CHECK_EQ(rd(&s, 0), 0x07fa3106u);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_linux);
	RUN_TEST(test_stops);
	return test_summary();
}
