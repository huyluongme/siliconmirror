/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835's random number generator
 * (src/hw/misc/bcm2835_rng.c), as Linux's bcm2835-rng.c uses it. The
 * numbers expected are SplitMix64's from the seed 0x534d4952, as a
 * Python model of the generator gives them.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/event.h"
#include "core/machine.h"
#include "hw/misc/bcm2835_rng.h"
#include "test.h"

#define BASE 0x3f104000
#define WORD 34000u        /* ns: a word every 34 us */
#define WARMUP 0x40000u    /* Linux's RNG_WARMUP_COUNT */
#define FIRST 278562000ull /* 0x40000 bits at 32 a word, then a word */

enum { CTRL = 0x0, STATUS = 0x4, DATA = 0x8 };

static const uint32_t numbers[] = {0x812265ca, 0x3b5688a6, 0x22c837e0,
                                   0x72c31c5c, 0xe433dd3b, 0x21a1a3a0};

typedef struct Setup {
	Machine *m;
	Bcm2835Rng *rng;
} Setup;

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->rng = bcm2835_rng_create(s->m);
	CHECK(s->rng != NULL);
	CHECK_EQ(bcm2835_rng_map(s->rng, s->m->bus, "rng", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_rng_destroy(s->rng);
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

/* The words in the FIFO, as STATUS counts them. */
static uint64_t words(Setup *s)
{
	return rd(s, STATUS) >> 24;
}

static void check_stop(Setup *s, const char *message)
{
	CHECK_EQ(s->m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s->m->stop_message, "bcm2835-rng: ") != NULL);
	CHECK(strstr(s->m->stop_message, message) != NULL);
	s->m->stop = MACHINE_STOP_NONE;
}

/*
 * Linux's bcm2835_rng_init: the warm-up, then RBGEN. The first word comes
 * after the warm-up's bits and a word's time; the FIFO fills to 4.
 */
static void test_linux(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, CTRL), 0);
	CHECK_EQ(rd(&s, STATUS), 0);
	wr(&s, STATUS, WARMUP);
	CHECK_EQ(rd(&s, STATUS), 0);
	wr(&s, CTRL, 1);
	CHECK_EQ(rd(&s, CTRL), 1);
	advance(&s, FIRST - 1);
	CHECK_EQ(words(&s), 0);
	advance(&s, FIRST);
	CHECK_EQ(rd(&s, STATUS), 1u << 24);
	advance(&s, FIRST + 2 * WORD);
	CHECK_EQ(words(&s), 3);
	/* Not full: one word out, and the next comes on time. */
	CHECK_EQ(rd(&s, DATA), numbers[0]);
	CHECK_EQ(words(&s), 2);
	advance(&s, FIRST + 3 * WORD - 1);
	CHECK_EQ(words(&s), 2);
	advance(&s, FIRST + 3 * WORD);
	CHECK_EQ(words(&s), 3);
	advance(&s, FIRST + 100 * WORD);
	CHECK_EQ(words(&s), 4);
	/* Full: the generator waited, and starts again with a read. */
	for (unsigned i = 1; i < 5; i++)
		CHECK_EQ(rd(&s, DATA), numbers[i]);
	CHECK_EQ(words(&s), 0);
	advance(&s, FIRST + 101 * WORD - 1);
	CHECK_EQ(words(&s), 0);
	advance(&s, FIRST + 101 * WORD);
	CHECK_EQ(words(&s), 1);
	CHECK_EQ(rd(&s, DATA), numbers[5]);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/*
 * Without a warm-up, the first word after a word's time. Stopped, the
 * generator keeps its words and makes no more; it warms up again when it
 * starts. A reset starts the numbers again.
 */
static void test_stop_start(void)
{
	Setup s;

	setup(&s);
	wr(&s, CTRL, 1);
	advance(&s, WORD - 1);
	CHECK_EQ(words(&s), 0);
	advance(&s, WORD);
	CHECK_EQ(words(&s), 1);
	advance(&s, WORD + 1000);
	wr(&s, CTRL, 1); /* running already: nothing changes */
	advance(&s, 2 * WORD);
	/* Stopped without a read since: the words made until then stay. */
	wr(&s, CTRL, 0);
	CHECK_EQ(rd(&s, CTRL), 0);
	advance(&s, 10 * WORD);
	CHECK_EQ(words(&s), 2);
	CHECK_EQ(rd(&s, DATA), numbers[0]);
	wr(&s, STATUS, 64);
	wr(&s, CTRL, 1);
	advance(&s, 10 * WORD + 3 * WORD - 1);
	CHECK_EQ(words(&s), 1);
	advance(&s, 10 * WORD + 3 * WORD);
	CHECK_EQ(words(&s), 2);
	CHECK_EQ(rd(&s, DATA), numbers[1]);

	/* A reset forgets the warm-up too. */
	wr(&s, CTRL, 0);
	wr(&s, STATUS, 1000);
	bcm2835_rng_reset(s.rng);
	CHECK_EQ(rd(&s, CTRL), 0);
	CHECK_EQ(words(&s), 0);
	wr(&s, CTRL, 1);
	advance(&s, 14 * WORD - 1);
	CHECK_EQ(words(&s), 0);
	advance(&s, 14 * WORD);
	CHECK_EQ(words(&s), 1);
	CHECK_EQ(rd(&s, DATA), numbers[0]);
	teardown(&s);
}

/* What no driver does stops the machine. */
static void test_stops(void)
{
	Setup s;
	uint64_t value;

	setup(&s);
	CHECK_EQ(rd(&s, DATA), 0);
	check_stop(&s, "4-byte read of offset 0x08 is not emulated: the FIFO "
	               "is empty");
	wr(&s, CTRL, 3);
	check_stop(&s, "write to offset 0x00 is not emulated: bits other than "
	               "RBGEN");
	wr(&s, CTRL, 0x80000001u);
	check_stop(&s, "bits other than RBGEN");
	CHECK_EQ(rd(&s, CTRL), 0);
	wr(&s, CTRL, 1);
	wr(&s, STATUS, 5);
	check_stop(&s, "write to offset 0x04 is not emulated: a warm-up while "
	               "the generator runs");
	/* The warm-up stayed 0. */
	wr(&s, CTRL, 0);
	wr(&s, CTRL, 1);
	advance(&s, WORD);
	CHECK_EQ(words(&s), 1);
	CHECK_EQ(rd(&s, 0xc), 0);
	check_stop(&s, "read of offset 0x0c is not emulated: no driver uses it");
	wr(&s, 0xc, 1);
	check_stop(&s, "write to offset 0x0c is not emulated: no driver writes "
	               "it");
	wr(&s, DATA, 1);
	check_stop(&s, "write to offset 0x08 is not emulated");
	CHECK_EQ(bus_read(s.m->bus, BASE + STATUS, 1, &value), BUS_OK);
	check_stop(&s, "1-byte read of offset 0x04 is not a 32-bit register");
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_read(s.m->bus, BASE + 2, 4, &value), BUS_OK);
	check_stop(&s, "read of offset 0x02 is not a 32-bit register");
	CHECK_EQ(bus_write(s.m->bus, BASE + CTRL, 2, 0), BUS_OK);
	check_stop(&s, "2-byte write to offset 0x00 is not a 32-bit register");
	CHECK_EQ(bus_write(s.m->bus, BASE + 1, 4, 0), BUS_OK);
	check_stop(&s, "write to offset 0x01 is not a 32-bit register");
	CHECK_EQ(rd(&s, CTRL), 1);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_linux);
	RUN_TEST(test_stop_start);
	RUN_TEST(test_stops);
	return test_summary();
}
