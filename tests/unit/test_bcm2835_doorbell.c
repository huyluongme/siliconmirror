/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the doorbells between the Arm and the VideoCore
 * (src/hw/misc/bcm2835_doorbell.c), as Linux's vchiq_arm rings and reads
 * them.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/misc/bcm2835_doorbell.h"
#include "test.h"

#define BASE 0x3f00b840

enum { BELL0 = 0x00, BELL2 = 0x08 };

typedef struct Setup {
	Machine *m;
	Bcm2835Doorbell *db;
	IrqLine irq;
	bool level;
	unsigned rings; /* of the VideoCore's doorbell */
} Setup;

static void on_irq(void *ctx, unsigned n, bool level)
{
	Setup *s = ctx;

	(void)n;
	s->level = level;
}

static void on_ring(void *ctx)
{
	Setup *s = ctx;

	s->rings++;
}

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->level = false;
	s->rings = 0;
	irq_init(&s->irq, on_irq, s, 0);
	s->db = bcm2835_doorbell_create(s->m, &s->irq,
	                                &(Bcm2835DoorbellVc){on_ring, s});
	CHECK(s->db != NULL);
	CHECK_EQ(bcm2835_doorbell_map(s->db, s->m->bus, "doorbell", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_doorbell_destroy(s->db);
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
	CHECK(strstr(s->m->stop_message, "bcm2835-doorbell: ") != NULL);
	CHECK(strstr(s->m->stop_message, message) != NULL);
	s->m->stop = MACHINE_STOP_NONE;
}

/*
 * The VideoCore rings the Arm: the interrupt, and bit 2 of BELL0 once,
 * as vchiq_doorbell_irq reads it. The Arm rings the VideoCore with
 * BELL2, whatever it writes.
 */
static void test_rings(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, BELL0), 0);
	bcm2835_doorbell_ring(s.db);
	CHECK(s.level);
	bcm2835_doorbell_ring(s.db);
	CHECK_EQ(rd(&s, BELL0), 4);
	CHECK(!s.level);
	CHECK_EQ(rd(&s, BELL0), 0);
	CHECK_EQ(s.rings, 0);
	CHECK_EQ(bus_write(s.m->bus, BASE + BELL2, 4, 0), BUS_OK);
	CHECK_EQ(s.rings, 1);
	CHECK_EQ(bus_write(s.m->bus, BASE + BELL2, 4, 0xffffffff), BUS_OK);
	CHECK_EQ(s.rings, 2);
	CHECK(!s.level);

	bcm2835_doorbell_ring(s.db);
	bcm2835_doorbell_reset(s.db);
	CHECK(!s.level);
	CHECK_EQ(rd(&s, BELL0), 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* What vchiq_arm does not do stops the machine. */
static void test_stops(void)
{
	Setup s;
	uint64_t value = 0x5a;

	setup(&s);
	bcm2835_doorbell_ring(s.db);
	CHECK_EQ(rd(&s, BELL2), 0);
	check_stop(&s, "4-byte read of offset 0x08 is not emulated: only BELL0 "
	               "is read, in 32 bits");
	CHECK_EQ(rd(&s, 0x04), 0);
	check_stop(&s, "read of offset 0x04 is not emulated");
	CHECK_EQ(bus_read(s.m->bus, BASE + BELL0, 1, &value), BUS_OK);
	check_stop(&s, "1-byte read of offset 0x00 is not emulated");
	CHECK_EQ(value, 0);
	CHECK(s.level); /* those reads did not clear it */
	CHECK_EQ(bus_write(s.m->bus, BASE + BELL0, 4, 4), BUS_OK);
	check_stop(&s, "4-byte write to offset 0x00 is not emulated: only BELL2 "
	               "is written, in 32 bits");
	CHECK_EQ(bus_write(s.m->bus, BASE + 0x0c, 4, 0), BUS_OK);
	check_stop(&s, "write to offset 0x0c is not emulated");
	CHECK_EQ(bus_write(s.m->bus, BASE + BELL2, 2, 0), BUS_OK);
	check_stop(&s, "2-byte write to offset 0x08 is not emulated");
	CHECK_EQ(bus_write(s.m->bus, BASE + 0x38, 4, 0), BUS_OK);
	check_stop(&s, "write to offset 0x38 is not emulated");
	CHECK_EQ(s.rings, 0);
	CHECK(s.level);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_rings);
	RUN_TEST(test_stops);
	return test_summary();
}
