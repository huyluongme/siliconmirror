/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the VideoCore's end of VCHIQ
 * (src/machine/raspi3b_vchiq.c). Slot zero is laid out as Linux's
 * vchiq_init_slots (vchiq_core.c, Raspberry Pi's Linux at cff533a) lays
 * it out in the 65 slots that vchiq_arm.c allocates: the master's
 * synchronous slot 1 and slots 2 to 32, the slave's 33 and 34 to 64.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/machine.h"
#include "machine/raspi3b_property.h"
#include "machine/raspi3b_vchiq.h"
#include "test.h"

#define RAM_SIZE 0x100000
#define SLOTS 0x10000 /* where the tests put the shared memory */
#define BUS 0xc0000000u
#define SLOT 4096
#define FILL 0x5a5a5a5au

/* Slot zero's fields, and a half's (vchiq_core.h). */
enum {
	MAGIC = 0,
	VERSION = 4,
	ZERO_SIZE = 8,
	SLOT_SIZE = 12,
	MAX_SLOTS = 16,
	MAX_PER_SIDE = 20,
	MASTER = 32,
	SLAVE = 404,
	INITIALISED = 0,
	FIRST = 4,
	LAST = 8,
	SYNC = 12,
	TRIGGER = 16,
	TX_POS = 28,
	RECYCLE = 32,
	QUEUE_RECYCLE = 44,
	SYNC_TRIGGER = 48,
	SYNC_RELEASE = 60,
	QUEUE = 72,
	DEBUG = 328,
	HALF = 372,
};

typedef struct Setup {
	Machine *m;
	Raspi3bVchiq *v;
} Setup;

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	CHECK_EQ(bus_map_ram(s->m->bus, "ram", 0, RAM_SIZE), BUS_OK);
	s->v = raspi3b_vchiq_create(s->m, RAM_SIZE);
	CHECK(s->v != NULL);
}

static void teardown(Setup *s)
{
	raspi3b_vchiq_destroy(s->v);
	machine_destroy(s->m);
}

static uint32_t rd(Setup *s, uint64_t address)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, address, 4, &value), BUS_OK);
	return (uint32_t)value;
}

static void wr(Setup *s, uint64_t address, uint32_t value)
{
	CHECK_EQ(bus_write(s->m->bus, address, 4, value), BUS_OK);
}

/*
 * Slot zero at at, as Linux's vchiq_init_slots leaves it, and the rest of
 * the slots that the master's half points to filled with FILL.
 */
static void slot_zero(Setup *s, uint32_t at)
{
	/*
	 * The Arm clears slot zero; FILL in the VideoCore's half shows what
	 * the VideoCore writes, and around it what it must not.
	 */
	for (uint32_t a = at; a < at + 33 * SLOT; a += 4)
		wr(s, a, a >= at + MASTER + HALF && a < at + 1288 ? 0 : FILL);
	wr(s, at + MASTER + INITIALISED, 0);
	wr(s, at + MAGIC, 0x56434849); /* 'VCHI' */
	wr(s, at + VERSION, 3u << 16 | 8);
	wr(s, at + ZERO_SIZE, 1288);
	wr(s, at + SLOT_SIZE, SLOT);
	wr(s, at + MAX_SLOTS, 128);
	wr(s, at + MAX_PER_SIDE, 64);
	wr(s, at + MASTER + SYNC, 1);
	wr(s, at + MASTER + FIRST, 2);
	wr(s, at + MASTER + LAST, 32);
	wr(s, at + SLAVE + SYNC, 33);
	wr(s, at + SLAVE + FIRST, 34);
	wr(s, at + SLAVE + LAST, 64);
}

static void check_stop(Setup *s, const char *message)
{
	CHECK_EQ(s->m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s->m->stop_message, "firmware: VCHIQ: ") != NULL);
	CHECK(strstr(s->m->stop_message, message) != NULL);
	s->m->stop = MACHINE_STOP_NONE;
}

/*
 * The master's half, once started: its 31 slots queued, the first to
 * send in, its events armed but the released synchronous slot's, its
 * synchronous slot empty, and initialised. The slave's half is left.
 */
static void test_start(void)
{
	uint32_t master = SLOTS + MASTER;
	Setup s;

	setup(&s);
	slot_zero(&s, SLOTS);
	wr(&s, master + TRIGGER + 4, 0); /* fired: the Arm's to set */
	CHECK(raspi3b_vchiq_init(s.v, BUS | SLOTS));
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	for (uint32_t i = 0; i < 31; i++)
		CHECK_EQ(rd(&s, master + QUEUE + 4 * i), 2 + i);
	CHECK_EQ(rd(&s, master + QUEUE + 4 * 31), FILL);
	CHECK_EQ(rd(&s, master + TX_POS), 0);
	CHECK_EQ(rd(&s, master + QUEUE_RECYCLE), 31);
	CHECK_EQ(rd(&s, master + TRIGGER), 1);
	CHECK_EQ(rd(&s, master + TRIGGER + 4), 0);
	CHECK_EQ(rd(&s, master + RECYCLE), 1);
	CHECK_EQ(rd(&s, master + RECYCLE + 4), FILL);
	CHECK_EQ(rd(&s, master + SYNC_TRIGGER), 1);
	CHECK_EQ(rd(&s, master + SYNC_TRIGGER + 4), FILL);
	CHECK_EQ(rd(&s, master + SYNC_RELEASE), 0);
	CHECK_EQ(rd(&s, master + SYNC_RELEASE + 4), 1);
	CHECK_EQ(rd(&s, master + DEBUG), 11);
	CHECK_EQ(rd(&s, master + DEBUG + 4), FILL);
	CHECK_EQ(rd(&s, master + TRIGGER + 8), FILL);
	CHECK_EQ(rd(&s, master + INITIALISED), 1);
	CHECK_EQ(rd(&s, master + FIRST), 2);
	CHECK_EQ(rd(&s, master + LAST), 32);
	CHECK_EQ(rd(&s, master + SYNC), 1);
	/* The synchronous slot's message: padding, the rest left. */
	CHECK_EQ(rd(&s, SLOTS + SLOT), 0);
	CHECK_EQ(rd(&s, SLOTS + SLOT + 4), FILL);
	CHECK_EQ(rd(&s, SLOTS + 2 * SLOT), FILL);
	CHECK_EQ(rd(&s, SLOTS + SLAVE + INITIALISED), 0);
	CHECK_EQ(rd(&s, SLOTS + SLAVE + TRIGGER), 0);
	CHECK_EQ(rd(&s, SLOTS + SLAVE + QUEUE), 0);
	CHECK_EQ(rd(&s, SLOTS + MASTER + HALF), 0);
	/* Slot zero's head, and what is before it, are the Arm's. */
	CHECK_EQ(rd(&s, SLOTS + MAGIC), 0x56434849);
	CHECK_EQ(rd(&s, SLOTS + VERSION), 3u << 16 | 8);
	CHECK_EQ(rd(&s, SLOTS + ZERO_SIZE), 1288);
	CHECK_EQ(rd(&s, SLOTS + SLOT_SIZE), SLOT);
	CHECK_EQ(rd(&s, SLOTS + MAX_SLOTS), 128);
	CHECK_EQ(rd(&s, SLOTS + MAX_PER_SIDE), 64);
	CHECK_EQ(rd(&s, SLOTS + 24), FILL);
	CHECK_EQ(rd(&s, SLOTS + 28), FILL);
	for (uint32_t a = SLOTS - 64; a < SLOTS; a += 4)
		CHECK_EQ(rd(&s, a), 0);

	/* Once is all; after a reset, again. */
	CHECK(!raspi3b_vchiq_init(s.v, BUS | SLOTS));
	check_stop(&s, "a second VCHIQ_INIT is not emulated");
	raspi3b_vchiq_reset(s.v);
	slot_zero(&s, SLOTS + 0x40000);
	CHECK(raspi3b_vchiq_init(s.v, SLOTS + 0x40000));
	CHECK_EQ(rd(&s, SLOTS + 0x40000 + MASTER + INITIALISED), 1);
	teardown(&s);
}

/* The Arm's versions: from 3 to 8 overlap the VideoCore's. */
static void test_versions(void)
{
	static const struct {
		uint16_t version, min;
		bool ok;
	} versions[] = {
		{8, 3, true},  {3, 3, true},  {8, 8, true},  {100, 1, true},
		{2, 1, false}, {9, 9, false}, {0, 0, false},
	};

	for (size_t i = 0; i < ARRAY_SIZE(versions); i++) {
		Setup s;

		setup(&s);
		slot_zero(&s, SLOTS);
		wr(&s, SLOTS + VERSION,
		   (uint32_t)versions[i].min << 16 | versions[i].version);
		CHECK_EQ(raspi3b_vchiq_init(s.v, BUS | SLOTS), versions[i].ok);
		if (!versions[i].ok) {
			check_stop(&s, "is not the VideoCore's, 8 (from 3)");
			CHECK_EQ(rd(&s, SLOTS + MASTER + INITIALISED), 0);
		}
		teardown(&s);
	}
}

/* A slot zero the VideoCore does not know stops the machine. */
static void test_stops(void)
{
	static const struct {
		uint32_t offset, value;
		const char *message;
	} stops[] = {
		{MAGIC, 0x56434848, "slot zero at 0x00010000 has no magic 'VCHI'"},
		{ZERO_SIZE, 1292, "slot zero's sizes are not the VideoCore's"},
		{SLOT_SIZE, 8192, "slot zero's sizes are not the VideoCore's"},
		{MAX_SLOTS, 64, "slot zero's sizes are not the VideoCore's"},
		{MAX_PER_SIDE, 32, "slot zero's sizes are not the VideoCore's"},
		{MASTER + SYNC, 0,
	     "the VideoCore's slots 0 (synchronous), 2 to 32 "
	     "are not emulated"},
		{MASTER + SYNC, 2, "slots 2 (synchronous), 2 to 32"},
		{MASTER + FIRST, 3, "slots 1 (synchronous), 3 to 32"},
		{MASTER + LAST, 1, "slots 1 (synchronous), 2 to 1"},
		{MASTER + LAST, 66, "slots 1 (synchronous), 2 to 66"},
		{MASTER + LAST, 0xffffffffu, "2 to 4294967295"},
		{MASTER + INITIALISED, 1,
	     "the VideoCore's half of slot zero is "
	     "initialised already"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(stops); i++) {
		Setup s;

		setup(&s);
		slot_zero(&s, SLOTS);
		wr(&s, SLOTS + stops[i].offset, stops[i].value);
		CHECK(!raspi3b_vchiq_init(s.v, BUS | SLOTS));
		check_stop(&s, stops[i].message);
		CHECK_EQ(rd(&s, SLOTS + MASTER + TRIGGER), FILL);
		/* The VideoCore did not start: a good slot zero starts it. */
		slot_zero(&s, SLOTS);
		CHECK(raspi3b_vchiq_init(s.v, BUS | SLOTS));
		teardown(&s);
	}
}

/*
 * The VideoCore's slots must be in the RAM, slot zero at a slot's
 * boundary.
 */
static void test_places(void)
{
	Setup s;

	setup(&s);
	CHECK(!raspi3b_vchiq_init(s.v, BUS | (SLOTS + 4)));
	check_stop(&s, "slots at 0xc0010004 are not emulated");
	CHECK(!raspi3b_vchiq_init(s.v, RAM_SIZE));
	check_stop(&s, "slots at 0x00100000 are not emulated");
	CHECK(!raspi3b_vchiq_init(s.v, 0x40000000u | RAM_SIZE));
	check_stop(&s, "slots at 0x40100000 are not emulated");
	/* 33 slots fit before the end of the RAM, 34 do not. */
	slot_zero(&s, RAM_SIZE - 33 * SLOT);
	CHECK(raspi3b_vchiq_init(s.v, RAM_SIZE - 33 * SLOT));
	raspi3b_vchiq_reset(s.v);
	slot_zero(&s, RAM_SIZE - 33 * SLOT);
	wr(&s, RAM_SIZE - 33 * SLOT + MASTER + LAST, 33);
	CHECK(!raspi3b_vchiq_init(s.v, RAM_SIZE - 33 * SLOT));
	check_stop(&s, "slots 1 (synchronous), 2 to 33 are not emulated");
	/* One slot of a side; 64. */
	raspi3b_vchiq_reset(s.v);
	slot_zero(&s, SLOTS);
	wr(&s, SLOTS + MASTER + LAST, 2);
	CHECK(raspi3b_vchiq_init(s.v, SLOTS));
	CHECK_EQ(rd(&s, SLOTS + MASTER + QUEUE), 2);
	CHECK_EQ(rd(&s, SLOTS + MASTER + QUEUE + 4), FILL);
	CHECK_EQ(rd(&s, SLOTS + MASTER + QUEUE_RECYCLE), 1);
	raspi3b_vchiq_reset(s.v);
	slot_zero(&s, 0);
	wr(&s, MASTER + LAST, 65);
	CHECK(raspi3b_vchiq_init(s.v, 0));
	CHECK_EQ(rd(&s, MASTER + QUEUE + 4 * 63), 65);
	CHECK_EQ(rd(&s, MASTER + QUEUE_RECYCLE), 64);
	teardown(&s);
}

/* The Arm rings the VideoCore's doorbell: no messages yet. */
static void test_doorbell(void)
{
	Setup s;

	setup(&s);
	raspi3b_vchiq_doorbell(s.v);
	check_stop(&s, "the Arm rang the VideoCore's doorbell: the VideoCore's "
	               "messages are not emulated yet");
	teardown(&s);
}

/*
 * The firmware's tag: VCHIQ_INIT answers 0 once the VideoCore started;
 * the property service's reset forgets it.
 */
static void test_tag(void)
{
	uint32_t buf[] = {sizeof(uint32_t[7]), 0, 0x00048010, 4, 0, BUS | SLOTS, 0};
	uint32_t reply = 0;
	Setup s;
	Raspi3bProperty *p;

	setup(&s);
	p = raspi3b_property_create(s.m, RAM_SIZE);
	CHECK(p != NULL);
	slot_zero(&s, SLOTS);
	for (unsigned i = 0; i < ARRAY_SIZE(buf); i++)
		wr(&s, 0x1000 + 4 * i, buf[i]);
	CHECK(raspi3b_property_receive(p, 0x1000 | 8, &reply));
	CHECK_EQ(rd(&s, 0x1000 + 4), 0x80000000u);
	CHECK_EQ(rd(&s, 0x1000 + 16), 0x80000004u);
	CHECK_EQ(rd(&s, 0x1000 + 20), 0);
	CHECK_EQ(rd(&s, SLOTS + MASTER + INITIALISED), 1);
	CHECK(raspi3b_property_vchiq(p) != NULL);

	raspi3b_property_reset(p);
	slot_zero(&s, SLOTS);
	for (unsigned i = 0; i < ARRAY_SIZE(buf); i++)
		wr(&s, 0x1000 + 4 * i, buf[i]);
	CHECK(raspi3b_property_receive(p, 0x1000 | 8, &reply));
	CHECK_EQ(rd(&s, 0x1000 + 20), 0);

	/* A request without its address; a slot zero without its magic. */
	buf[3] = 3;
	for (unsigned i = 0; i < ARRAY_SIZE(buf); i++)
		wr(&s, 0x1000 + 4 * i, buf[i]);
	CHECK(!raspi3b_property_receive(p, 0x1000 | 8, &reply));
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message,
	             "tag 0x00048010: a value buffer too short") != NULL);
	s.m->stop = MACHINE_STOP_NONE;
	raspi3b_property_reset(p);
	buf[3] = 4;
	buf[5] = 0x2000;
	for (unsigned i = 0; i < ARRAY_SIZE(buf); i++)
		wr(&s, 0x1000 + 4 * i, buf[i]);
	CHECK(!raspi3b_property_receive(p, 0x1000 | 8, &reply));
	check_stop(&s, "slot zero at 0x00002000 has no magic 'VCHI'");
	raspi3b_property_destroy(p);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_start);
	RUN_TEST(test_versions);
	RUN_TEST(test_stops);
	RUN_TEST(test_places);
	RUN_TEST(test_doorbell);
	RUN_TEST(test_tag);
	return test_summary();
}
