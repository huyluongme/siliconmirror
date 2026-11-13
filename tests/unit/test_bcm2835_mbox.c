/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835 mailboxes (src/hw/misc/bcm2835_mbox.c).
 * Expected values come from the Raspberry Pi firmware wiki ("Mailboxes",
 * "Accessing mailboxes") and Linux's drivers/mailbox/bcm2835-mailbox.c.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/event.h"
#include "core/machine.h"
#include "hw/misc/bcm2835_mbox.h"
#include "test.h"
#include "util/macros.h"

#define BASE 0x3f00b880
#define LATENCY 1000

enum {
	MAIL0_READ = 0x00,
	MAIL0_PEEK = 0x10,
	MAIL0_SENDER = 0x14,
	MAIL0_STATUS = 0x18,
	MAIL0_CONFIG = 0x1c,
	MAIL1_WRITE = 0x20,
	MAIL1_PEEK = 0x30,
	MAIL1_SENDER = 0x34,
	MAIL1_STATUS = 0x38,
	MAIL1_CONFIG = 0x3c,
};

#define FULL 0x80000000u
#define EMPTY 0x40000000u

/* A VideoCore that notes what it gets, and answers with the message + 1. */
typedef struct Vc {
	Machine *m;
	uint32_t got[32];
	uint64_t when[32];
	unsigned count;
	bool silent; /* gives no answer */
} Vc;

static bool vc_receive(void *ctx, uint32_t message, uint32_t *reply)
{
	Vc *vc = ctx;

	vc->got[vc->count] = message;
	vc->when[vc->count] = vc->m->events.now;
	vc->count++;
	*reply = message + 1;
	return !vc->silent;
}

typedef struct Setup {
	Machine *m;
	Vc vc;
	IrqLine irq;
	Bcm2835Mbox *mbox;
} Setup;

static void setup(Setup *s)
{
	memset(s, 0, sizeof(*s));
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->vc.m = s->m;
	irq_init(&s->irq, NULL, NULL, 0);
	s->mbox = bcm2835_mbox_create(s->m, &(Bcm2835MboxVc){vc_receive, &s->vc},
	                              LATENCY, &s->irq);
	CHECK(s->mbox != NULL);
	CHECK_EQ(bcm2835_mbox_map(s->mbox, s->m->bus, "mbox", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_mbox_destroy(s->mbox);
	machine_destroy(s->m);
}

static uint64_t reg_read(Setup *s, uint64_t offset)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, BASE + offset, 4, &value), BUS_OK);
	return value;
}

static void reg_write(Setup *s, uint64_t offset, uint64_t value)
{
	CHECK_EQ(bus_write(s->m->bus, BASE + offset, 4, value), BUS_OK);
}

static void advance(Setup *s, uint64_t t)
{
	event_queue_run_until(&s->m->events, t);
}

/* Both mailboxes start empty, with the interrupt off. */
static void test_reset_values(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), EMPTY);
	CHECK_EQ(reg_read(&s, MAIL1_STATUS), EMPTY);
	CHECK_EQ(reg_read(&s, MAIL0_CONFIG), 0);
	CHECK(!irq_level(&s.irq));
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/*
 * A message written to mailbox 1 reaches the VideoCore after the
 * latency; its answer waits in mailbox 0 until the Arm reads it.
 */
static void test_message(void)
{
	Setup s;

	setup(&s);
	advance(&s, 500);
	reg_write(&s, MAIL1_WRITE, 0x1238);
	CHECK_EQ(reg_read(&s, MAIL1_STATUS), 0); /* neither full nor empty */
	advance(&s, 500 + LATENCY - 1);
	CHECK_EQ(s.vc.count, 0);
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), EMPTY);
	advance(&s, 500 + LATENCY);
	CHECK_EQ(s.vc.count, 1);
	CHECK_EQ(s.vc.got[0], 0x1238);
	CHECK_EQ(reg_read(&s, MAIL1_STATUS), EMPTY);
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), 0);
	CHECK_EQ(reg_read(&s, MAIL0_PEEK), 0x1239); /* does not take it */
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), 0);
	CHECK_EQ(reg_read(&s, MAIL0_READ), 0x1239);
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), EMPTY);

	/* No answer: mailbox 0 stays empty. */
	s.vc.silent = true;
	reg_write(&s, MAIL1_WRITE, 0x5678);
	advance(&s, 10 * LATENCY);
	CHECK_EQ(s.vc.count, 2);
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), EMPTY);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/*
 * Each mailbox holds eight words, in order; the VideoCore takes one at a
 * time, and waits while mailbox 0 has no room for an answer.
 */
static void test_fifo(void)
{
	Setup s;

	CHECK_EQ(BCM2835_MBOX_DEPTH, 8); /* 8 words in each mailbox */
	setup(&s);
	for (unsigned i = 0; i < BCM2835_MBOX_DEPTH; i++) {
		CHECK_EQ(reg_read(&s, MAIL1_STATUS) & FULL, 0);
		reg_write(&s, MAIL1_WRITE, 0x100 * (i + 1));
	}
	CHECK_EQ(reg_read(&s, MAIL1_STATUS), FULL);
	advance(&s, BCM2835_MBOX_DEPTH * LATENCY);
	CHECK_EQ(s.vc.count, BCM2835_MBOX_DEPTH);
	for (unsigned i = 0; i < BCM2835_MBOX_DEPTH; i++) {
		CHECK_EQ(s.vc.got[i], 0x100 * (i + 1));
		CHECK_EQ(s.vc.when[i], (i + 1) * LATENCY);
	}
	CHECK_EQ(reg_read(&s, MAIL1_STATUS), EMPTY);
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), FULL);

	/* Mailbox 0 is full: the next message waits in mailbox 1. */
	reg_write(&s, MAIL1_WRITE, 0x900);
	advance(&s, 20 * LATENCY);
	CHECK_EQ(s.vc.count, BCM2835_MBOX_DEPTH);
	CHECK_EQ(reg_read(&s, MAIL1_STATUS), 0);
	CHECK_EQ(reg_read(&s, MAIL0_READ), 0x101);
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), 0);
	advance(&s, 21 * LATENCY - 1);
	CHECK_EQ(s.vc.count, BCM2835_MBOX_DEPTH);
	advance(&s, 21 * LATENCY);
	CHECK_EQ(s.vc.count, BCM2835_MBOX_DEPTH + 1);
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), FULL);
	for (unsigned i = 1; i < BCM2835_MBOX_DEPTH; i++)
		CHECK_EQ(reg_read(&s, MAIL0_READ), 0x100 * (i + 1) + 1);
	CHECK_EQ(reg_read(&s, MAIL0_READ), 0x901);
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), EMPTY);

	/* Both fill again after wrapping around. */
	for (unsigned i = 0; i < 2 * BCM2835_MBOX_DEPTH; i++) {
		reg_write(&s, MAIL1_WRITE, i);
		advance(&s, s.m->events.now + LATENCY);
		CHECK_EQ(reg_read(&s, MAIL0_READ), i + 1);
	}
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* Bit 0 of the configuration: an interrupt while mailbox 0 has data. */
static void test_interrupt(void)
{
	Setup s;

	setup(&s);
	reg_write(&s, MAIL1_WRITE, 0x18);
	advance(&s, LATENCY);
	CHECK(!irq_level(&s.irq));
	reg_write(&s, MAIL0_CONFIG, 1);
	CHECK_EQ(reg_read(&s, MAIL0_CONFIG), 1);
	CHECK(irq_level(&s.irq));
	reg_write(&s, MAIL0_CONFIG, 0);
	CHECK(!irq_level(&s.irq));
	reg_write(&s, MAIL0_CONFIG, 1);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(reg_read(&s, MAIL0_PEEK), 0x19);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(reg_read(&s, MAIL0_READ), 0x19);
	CHECK(!irq_level(&s.irq));

	/* An answer raises it; no answer does not. */
	reg_write(&s, MAIL1_WRITE, 0x28);
	advance(&s, 2 * LATENCY - 1);
	CHECK(!irq_level(&s.irq));
	advance(&s, 2 * LATENCY);
	CHECK(irq_level(&s.irq));
	reg_read(&s, MAIL0_READ);
	s.vc.silent = true;
	reg_write(&s, MAIL1_WRITE, 0x38);
	advance(&s, 4 * LATENCY);
	CHECK(!irq_level(&s.irq));

	/* A reset empties the mailboxes and disables the interrupt. */
	s.vc.silent = false;
	reg_write(&s, MAIL1_WRITE, 0x48);
	advance(&s, 6 * LATENCY);
	CHECK(irq_level(&s.irq));
	reg_write(&s, MAIL1_WRITE, 0x58);
	bcm2835_mbox_reset(s.mbox);
	CHECK(!irq_level(&s.irq));
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), EMPTY);
	CHECK_EQ(reg_read(&s, MAIL1_STATUS), EMPTY);
	CHECK_EQ(reg_read(&s, MAIL0_CONFIG), 0);
	advance(&s, 20 * LATENCY);
	CHECK_EQ(s.vc.count, 4); /* 0x58 was dropped */
	CHECK_EQ(reg_read(&s, MAIL0_STATUS), EMPTY);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* What the documentation does not describe stops the machine. */
static void test_stops(void)
{
	static const struct {
		bool write;
		uint64_t offset;
		unsigned size;
		bool full; /* fill mailbox 1 first */
		const char *message;
	} stops[] = {
		{false, MAIL0_READ, 4, false,
	     "4-byte read of offset 0x00 is not emulated: mailbox 0 is empty"},
		{false, MAIL0_PEEK, 4, false, "offset 0x10 is not emulated: mailbox 0"},
		{false, MAIL0_SENDER, 4, false,
	     "offset 0x14 is not emulated (the sender register)"},
		{false, MAIL1_SENDER, 4, false,
	     "offset 0x34 is not emulated (the sender register)"},
		{false, MAIL1_WRITE, 4, false,
	     "offset 0x20 is not emulated: the Arm never reads mailbox 1"},
		{false, MAIL1_PEEK, 4, false, "offset 0x30 is not emulated: the Arm"},
		{false, MAIL1_CONFIG, 4, false,
	     "offset 0x3c is not a documented register"},
		{false, 0x04, 4, false, "offset 0x04 is not a documented register"},
		{false, MAIL0_STATUS, 2, false,
	     "2-byte read of offset 0x18 is not a 32-bit register read"},
		{false, 0x1a, 4, false, "offset 0x1a is not a 32-bit register read"},
		{true, MAIL0_READ, 4, false,
	     "4-byte write to offset 0x00 is not emulated: the Arm never writes "
	     "mailbox 0"},
		{true, MAIL0_PEEK, 4, false, "offset 0x10 is not emulated: the Arm"},
		{true, MAIL0_SENDER, 4, false, "offset 0x14 is not emulated: the Arm"},
		{true, MAIL0_STATUS, 4, false, "offset 0x18 is not emulated: the Arm"},
		{true, MAIL0_CONFIG, 4, false,
	     "offset 0x1c sets bits that are not emulated"},
		{true, MAIL1_PEEK, 4, false,
	     "offset 0x30 is not emulated (a register of mailbox 1)"},
		{true, MAIL1_SENDER, 4, false, "offset 0x34 is not emulated (a"},
		{true, MAIL1_STATUS, 4, false, "offset 0x38 is not emulated (a"},
		{true, MAIL1_CONFIG, 4, false, "offset 0x3c is not emulated (a"},
		{true, 0x24, 4, false, "offset 0x24 is not a documented register"},
		{true, MAIL1_WRITE, 8, false,
	     "8-byte write to offset 0x20 is not a 32-bit register write"},
		{true, 0x22, 4, false, "offset 0x22 is not a 32-bit register write"},
		{true, MAIL1_WRITE, 4, true,
	     "offset 0x20 is not emulated: mailbox 1 is full"},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(stops); i++) {
		Setup s;
		uint64_t value = 0;

		setup(&s);
		if (stops[i].full)
			for (unsigned j = 0; j < BCM2835_MBOX_DEPTH; j++)
				reg_write(&s, MAIL1_WRITE, j);
		if (stops[i].write)
			bus_write(s.m->bus, BASE + stops[i].offset, stops[i].size,
			          0xfffffff8);
		else
			bus_read(s.m->bus, BASE + stops[i].offset, stops[i].size, &value);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		CHECK(strstr(s.m->stop_message, "bcm2835-mbox: ") != NULL);
		/* The access did nothing else. */
		CHECK_EQ(value, 0);
		CHECK_EQ(reg_read(&s, MAIL0_STATUS), EMPTY);
		CHECK_EQ(reg_read(&s, MAIL0_CONFIG), 0);
		CHECK_EQ(reg_read(&s, MAIL1_STATUS), stops[i].full ? FULL : EMPTY);
		advance(&s, 20 * LATENCY);
		CHECK_EQ(s.vc.count, stops[i].full ? BCM2835_MBOX_DEPTH : 0);
		teardown(&s);
	}
}

/* Without an interrupt line, the mailboxes still work. */
static void test_no_irq(void)
{
	Machine *m = machine_create();
	Vc vc = {.m = m};
	Bcm2835Mbox *mbox =
		bcm2835_mbox_create(m, &(Bcm2835MboxVc){vc_receive, &vc}, 0, NULL);
	uint64_t value = 0;

	CHECK(mbox != NULL);
	CHECK_EQ(bcm2835_mbox_map(mbox, m->bus, "mbox", BASE), BUS_OK);
	CHECK_EQ(bus_write(m->bus, BASE + MAIL0_CONFIG, 4, 1), BUS_OK);
	CHECK_EQ(bus_write(m->bus, BASE + MAIL1_WRITE, 4, 0x48), BUS_OK);
	event_queue_run_until(&m->events, 0);
	CHECK_EQ(bus_read(m->bus, BASE + MAIL0_READ, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x49);
	/* Destroying with a message on its way. */
	CHECK_EQ(bus_write(m->bus, BASE + MAIL1_WRITE, 4, 0x58), BUS_OK);
	bcm2835_mbox_destroy(mbox);
	event_queue_run_until(&m->events, 100);
	CHECK_EQ(vc.count, 1);
	machine_destroy(m);
}

/* The registers take 0x40 bytes: 0x3c is one, 0x40 is not mapped. */
static void test_extent(void)
{
	Setup s;
	uint64_t value;

	setup(&s);
	CHECK_EQ(bus_read(s.m->bus, BASE + 0x40, 4, &value), BUS_ERR_UNMAPPED);
	CHECK_EQ(bus_read(s.m->bus, BASE + 0x3c, 4, &value), BUS_OK);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_message);
	RUN_TEST(test_fifo);
	RUN_TEST(test_interrupt);
	RUN_TEST(test_stops);
	RUN_TEST(test_no_irq);
	RUN_TEST(test_extent);
	return test_summary();
}
