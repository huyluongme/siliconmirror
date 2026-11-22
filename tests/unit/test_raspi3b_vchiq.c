/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the VideoCore's end of VCHIQ
 * (src/machine/raspi3b_vchiq.c). Slot zero is laid out as Linux's
 * vchiq_init_slots (vchiq_core.c, Raspberry Pi's Linux at cff533a) lays
 * it out in the 65 slots that vchiq_arm.c allocates: the master's
 * synchronous slot 1 and slots 2 to 32, the slave's 33 and 34 to 64.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/bus.h"
#include "core/event.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/misc/bcm2835_doorbell.h"
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
		{SLAVE + SYNC, 0,
	     "the Arm's slots 0 (synchronous), 34 to 64 are not emulated"},
		{SLAVE + FIRST, 35, "the Arm's slots 33 (synchronous), 35 to 64"},
		{SLAVE + LAST, 33, "the Arm's slots 33 (synchronous), 34 to 33"},
		{SLAVE + LAST, 98, "the Arm's slots 33 (synchronous), 34 to 98"},
		{SLAVE + LAST, 0xffffffffu, "34 to 4294967295"},
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
	/* 65 slots fit before the end of the RAM, 66 do not. */
	slot_zero(&s, RAM_SIZE - 65 * SLOT);
	CHECK(raspi3b_vchiq_init(s.v, RAM_SIZE - 65 * SLOT));
	raspi3b_vchiq_reset(s.v);
	slot_zero(&s, RAM_SIZE - 65 * SLOT);
	wr(&s, RAM_SIZE - 65 * SLOT + SLAVE + LAST, 65);
	CHECK(!raspi3b_vchiq_init(s.v, RAM_SIZE - 65 * SLOT));
	check_stop(&s, "the Arm's slots 33 (synchronous), 34 to 65 are not "
	               "emulated");
	slot_zero(&s, RAM_SIZE - 65 * SLOT);
	wr(&s, RAM_SIZE - 65 * SLOT + MASTER + LAST, 65);
	CHECK(!raspi3b_vchiq_init(s.v, RAM_SIZE - 65 * SLOT));
	check_stop(&s, "the VideoCore's slots 1 (synchronous), 2 to 65 are not "
	               "emulated");
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

/* --- Messages ------------------------------------------------------------ */

#define LATENCY 1000     /* ns: the VideoCore's, as the board sets it */
#define BELL 0x3f00b840  /* the doorbells */
#define BELL0 BELL       /* read: whether the VideoCore rang the Arm */
#define BELL2 (BELL + 8) /* written: rings the VideoCore */
#define VC_SLOTS 31      /* 2 to 32 */
#define ARM_SLOTS 31     /* 34 to 64 */
#define ARMED 0
#define FIRED 4

#define MSG(type, src, dst) ((uint32_t)(type) << 24 | (src) << 12 | (dst))
enum { PADDING, CONNECT, OPEN, OPENACK, CLOSE, DATA };
#define AUDS 0x41554453u /* 'AUDS' */
#define MMAL 0x6d6d616cu /* 'mmal' */

/* The Arm's end, as Linux's vchiq_core plays it, and the warnings. */
typedef struct Link {
	Setup s;
	Bcm2835Doorbell *db;
	IrqLine irq;      /* the Arm's doorbell 0 */
	uint32_t tx_pos;  /* the Arm's */
	unsigned vc_read; /* the VideoCore's messages the test has read */
	unsigned warnings;
	char warning[256];
} Link;

static void collect(void *ctx, const char *message)
{
	Link *l = ctx;

	l->warnings++;
	snprintf(l->warning, sizeof(l->warning), "%s", message);
}

/*
 * The VideoCore started on slot zero at SLOTS, the Arm's half as
 * vchiq_init_state leaves it: its slots queued, waiting on its trigger
 * and recycle events.
 */
static void link_start(Link *l)
{
	uint32_t slave = SLOTS + SLAVE;

	l->tx_pos = 0;
	l->vc_read = 0;
	slot_zero(&l->s, SLOTS);
	for (uint32_t i = 0; i < ARM_SLOTS; i++)
		wr(&l->s, slave + QUEUE + 4 * i, 34 + i);
	wr(&l->s, slave + QUEUE_RECYCLE, ARM_SLOTS);
	wr(&l->s, slave + TX_POS, 0);
	wr(&l->s, slave + TRIGGER + ARMED, 1);
	wr(&l->s, slave + TRIGGER + FIRED, 0);
	wr(&l->s, slave + RECYCLE + ARMED, 1);
	wr(&l->s, slave + RECYCLE + FIRED, 0);
	wr(&l->s, SLOTS + MASTER + TRIGGER + FIRED, 0);
	wr(&l->s, SLOTS + MASTER + RECYCLE + FIRED, 0);
	wr(&l->s, SLOTS + MASTER + SYNC_TRIGGER + FIRED, 0);
	CHECK(raspi3b_vchiq_init(l->s.v, BUS | SLOTS));
}

static void link_up(Link *l, bool warn)
{
	memset(l, 0, sizeof(*l));
	setup(&l->s);
	irq_init(&l->irq, NULL, NULL, 0);
	l->db = bcm2835_doorbell_create(
		l->s.m, &l->irq, &(Bcm2835DoorbellVc){raspi3b_vchiq_doorbell, l->s.v});
	CHECK(l->db != NULL);
	CHECK_EQ(bcm2835_doorbell_map(l->db, l->s.m->bus, "doorbell", BELL),
	         BUS_OK);
	raspi3b_vchiq_attach(l->s.v, l->db, LATENCY, warn ? collect : NULL, l);
	link_start(l);
}

static void link_down(Link *l)
{
	bcm2835_doorbell_destroy(l->db);
	teardown(&l->s);
}

/*
 * reserve_space and queue_message: the Arm's message, its data words
 * (NULL: zeros), padding the rest of a slot that it does not fit.
 */
static void arm_put(Link *l, uint32_t msgid, uint32_t size,
                    const uint32_t *data)
{
	uint32_t slave = SLOTS + SLAVE;
	uint32_t stride = (size + 15) & ~7u;
	uint32_t space = SLOT - (l->tx_pos & (SLOT - 1));
	uint32_t slot, at;

	if (stride > space) {
		slot = rd(&l->s, slave + QUEUE + 4 * ((l->tx_pos / SLOT) & 63));
		at = SLOTS + slot * SLOT + (l->tx_pos & (SLOT - 1));
		wr(&l->s, at, MSG(PADDING, 0, 0));
		wr(&l->s, at + 4, space - 8);
		l->tx_pos += space;
	}
	/* The slot must have come back. */
	if ((l->tx_pos & (SLOT - 1)) == 0)
		CHECK((int32_t)(l->tx_pos / SLOT - rd(&l->s, slave + QUEUE_RECYCLE)) <
		      0);
	slot = rd(&l->s, slave + QUEUE + 4 * ((l->tx_pos / SLOT) & 63));
	CHECK(slot >= 34 && slot <= 64);
	at = SLOTS + slot * SLOT + (l->tx_pos & (SLOT - 1));
	wr(&l->s, at, msgid);
	wr(&l->s, at + 4, size);
	for (uint32_t i = 0; i < (size + 3) / 4; i++)
		wr(&l->s, at + 8 + 4 * i, data != NULL ? data[i] : 0);
	l->tx_pos += stride;
	wr(&l->s, slave + TX_POS, l->tx_pos);
}

/* remote_event_signal on the VideoCore's trigger. */
static void arm_signal(Link *l)
{
	wr(&l->s, SLOTS + MASTER + TRIGGER + FIRED, 1);
	if (rd(&l->s, SLOTS + MASTER + TRIGGER + ARMED) != 0)
		wr(&l->s, BELL2, 0);
}

static void run(Link *l, uint64_t ns)
{
	event_queue_run_until(&l->s.m->events, l->s.m->events.now + ns);
}

/* Send, signal, and let the VideoCore answer. */
static void arm_send(Link *l, uint32_t msgid, uint32_t size,
                     const uint32_t *data)
{
	arm_put(l, msgid, size, data);
	arm_signal(l);
	run(l, LATENCY);
}

static void arm_open(Link *l, unsigned port, uint32_t fourcc)
{
	uint32_t payload[3] = {fourcc, 1234, 3u << 16 | 3};

	arm_send(l, MSG(OPEN, port, 0), 12, payload);
}

/* The VideoCore's messages sent so far. */
static unsigned vc_sent(Link *l)
{
	return rd(&l->s, SLOTS + MASTER + TX_POS) / 8;
}

/* The VideoCore's next message, which must be a header alone. */
static uint32_t vc_next(Link *l)
{
	uint32_t pos = 8 * l->vc_read++;
	uint32_t slot = rd(&l->s, SLOTS + MASTER + QUEUE + 4 * ((pos / SLOT) & 63));
	uint32_t at = SLOTS + slot * SLOT + (pos & (SLOT - 1));

	CHECK(slot >= 2 && slot <= 32);
	CHECK_EQ(rd(&l->s, at + 4), 0);
	return rd(&l->s, at);
}

/* Whether the VideoCore rang the Arm; reading BELL0 clears it. */
static bool vc_rang(Link *l)
{
	bool level = irq_level(&l->irq);

	CHECK_EQ(rd(&l->s, BELL0) != 0, level);
	CHECK(!irq_level(&l->irq));
	return level;
}

/* The Arm's CONNECT, which the VideoCore answers after its latency. */
static void test_connect(void)
{
	Link l;

	link_up(&l, true);
	arm_put(&l, MSG(CONNECT, 0, 0), 0, NULL);
	arm_signal(&l);
	run(&l, LATENCY - 1);
	CHECK_EQ(vc_sent(&l), 0);
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TRIGGER + FIRED), 1);
	CHECK(!irq_level(&l.irq));
	run(&l, 1);
	CHECK_EQ(vc_sent(&l), 1);
	CHECK_EQ(vc_next(&l), MSG(CONNECT, 0, 0));
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TRIGGER + FIRED), 0);
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TRIGGER + ARMED), 1);
	CHECK_EQ(rd(&l.s, SLOTS + SLAVE + TRIGGER + FIRED), 1);
	CHECK(vc_rang(&l));
	/* Its slot not read to the end, the Arm's is not handed back. */
	CHECK_EQ(rd(&l.s, SLOTS + SLAVE + QUEUE_RECYCLE), ARM_SLOTS);
	CHECK_EQ(rd(&l.s, SLOTS + SLAVE + RECYCLE + FIRED), 0);
	CHECK_EQ(l.warnings, 0);

	/* A signal with nothing new: nothing. */
	arm_signal(&l);
	run(&l, LATENCY);
	CHECK_EQ(vc_sent(&l), 1);
	CHECK(!vc_rang(&l));

	/* Two rings in the latency: answered once, a latency after the first. */
	arm_put(&l, MSG(OPEN, 1, 0), 12, NULL);
	arm_signal(&l);
	run(&l, LATENCY / 2);
	arm_put(&l, MSG(OPEN, 2, 0), 12, NULL);
	arm_signal(&l);
	run(&l, LATENCY / 2);
	CHECK_EQ(vc_sent(&l), 3);
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 1));
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 2));

	/* The Arm not waiting on its trigger: fired, but no ring. */
	CHECK(vc_rang(&l));
	wr(&l.s, SLOTS + SLAVE + TRIGGER + ARMED, 0);
	wr(&l.s, SLOTS + SLAVE + TRIGGER + FIRED, 0);
	arm_open(&l, 3, AUDS);
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 3));
	CHECK_EQ(rd(&l.s, SLOTS + SLAVE + TRIGGER + FIRED), 1);
	CHECK(!vc_rang(&l));
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	link_down(&l);
}

/*
 * OPEN: no service of the VideoCore's is emulated; each is refused with
 * a CLOSE from port 0 to the Arm's port, and named in a warning once.
 */
static void test_open(void)
{
	Link l;
	uint32_t odd[3] = {0x01417f20u, 0, 0};

	link_up(&l, true);
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_next(&l), MSG(CONNECT, 0, 0));
	arm_open(&l, 5, AUDS);
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 5));
	CHECK_EQ(l.warnings, 1);
	CHECK(strcmp(l.warning, "VCHIQ: the VideoCore's service 'AUDS' is not "
	                        "emulated: it refuses the Arm's OPEN") == 0);
	arm_open(&l, 6, AUDS);
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 6));
	CHECK_EQ(l.warnings, 1);
	arm_open(&l, 0xfff, MMAL);
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 0xfff));
	CHECK_EQ(l.warnings, 2);
	CHECK(strstr(l.warning, "service 'mmal' is not") != NULL);
	/* What is not text shows as '?'. */
	arm_send(&l, MSG(OPEN, 7, 0), 12, odd);
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 7));
	CHECK(strstr(l.warning, "service '?A? ' is not") != NULL);
	/* An OPEN too short to name a service: refused, no warning. */
	arm_send(&l, MSG(OPEN, 8, 0), 11, NULL);
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 8));
	CHECK_EQ(l.warnings, 3);
	/* The Arm's CLOSE and PADDING: no answer. */
	arm_send(&l, MSG(CLOSE, 9, 0), 0, NULL);
	arm_send(&l, MSG(PADDING, 0, 0), 16, NULL);
	CHECK_EQ(vc_sent(&l), 6);
	/* Sixteen services are remembered; the rest are named each time. */
	for (uint32_t i = 0; i < 13; i++)
		arm_open(&l, 10 + i, 0x41414141u + i);
	CHECK_EQ(l.warnings, 16);
	arm_open(&l, 30, 0x42424242u);
	arm_open(&l, 31, 0x42424242u);
	CHECK_EQ(l.warnings, 18);
	arm_open(&l, 32, 0x41414141u + 12);
	CHECK_EQ(l.warnings, 18);
	CHECK_EQ(vc_sent(&l), 22);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	link_down(&l);

	/* Without warnings. */
	link_up(&l, false);
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	arm_open(&l, 5, AUDS);
	CHECK_EQ(vc_sent(&l), 2);
	CHECK_EQ(l.warnings, 0);
	link_down(&l);
}

/*
 * A reset of the board, then Linux's VCHIQ_INIT again on a new slot
 * zero: the VideoCore starts over, positions, connection and warnings.
 */
static void test_restart(void)
{
	uint32_t edges[3] = {0x1f207e7fu, 0, 0};
	Link l;

	link_up(&l, true);
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	arm_open(&l, 5, AUDS);
	arm_send(&l, MSG(OPEN, 6, 0), 2000, NULL); /* service '????' */
	CHECK_EQ(vc_sent(&l), 3);
	CHECK_EQ(l.warnings, 2);
	raspi3b_vchiq_reset(l.s.v);
	link_start(&l);
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	CHECK_EQ(vc_sent(&l), 1);
	CHECK_EQ(rd(&l.s, SLOTS + 2 * SLOT), MSG(CONNECT, 0, 0));
	arm_open(&l, 7, AUDS);
	CHECK_EQ(vc_next(&l), MSG(CONNECT, 0, 0));
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 7));
	CHECK_EQ(l.warnings, 3); /* AUDS again: forgotten */
	/* The edges of text: 0x1f and 0x7f are not, 0x20 and 0x7e are. */
	arm_send(&l, MSG(OPEN, 8, 0), 12, edges);
	CHECK(strstr(l.warning, "service '? ~?' is not") != NULL);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	link_down(&l);
}

/*
 * The Arm's slots: read to its end, a slot goes back to the Arm, in its
 * slot queue at slot_queue_recycle, with its recycle event; a message
 * that does not fit leaves padding before the next slot.
 */
static void test_arm_slots(void)
{
	static const uint32_t nine[3] = {AUDS, 0, MSG(DATA, 1, 2)};
	uint32_t slave = SLOTS + SLAVE;
	Link l;

	link_up(&l, true);
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK(vc_rang(&l));
	/* 8 + 2008 + 2072: 8 bytes left, a message of 0 fits exactly. */
	arm_send(&l, MSG(OPEN, 1, 0), 2000, NULL);
	arm_send(&l, MSG(OPEN, 2, 0), 2064, NULL);
	CHECK_EQ(rd(&l.s, slave + QUEUE_RECYCLE), ARM_SLOTS);
	CHECK(vc_rang(&l));
	arm_send(&l, MSG(CLOSE, 3, 0), 0, NULL);
	CHECK_EQ(l.tx_pos, SLOT);
	CHECK_EQ(rd(&l.s, slave + QUEUE_RECYCLE), ARM_SLOTS + 1);
	CHECK_EQ(rd(&l.s, slave + QUEUE + 4 * ARM_SLOTS), 34);
	CHECK_EQ(rd(&l.s, slave + RECYCLE + FIRED), 1);
	CHECK(vc_rang(&l)); /* the Arm waits on its recycle event */
	/* A message that does not fit: the Arm pads, the VideoCore skips. */
	arm_send(&l, MSG(OPEN, 4, 0), 4000, NULL);
	arm_send(&l, MSG(OPEN, 5, 0), 100, NULL);
	CHECK_EQ(l.tx_pos, 2 * SLOT + 112);
	CHECK_EQ(rd(&l.s, slave + QUEUE_RECYCLE), ARM_SLOTS + 2);
	CHECK_EQ(rd(&l.s, slave + QUEUE + 4 * (ARM_SLOTS + 1)), 35);
	CHECK_EQ(vc_sent(&l), 5);
	/* The Arm not waiting on its recycle event: no ring for it. */
	vc_rang(&l);
	wr(&l.s, slave + RECYCLE + ARMED, 0);
	wr(&l.s, slave + TRIGGER + ARMED, 0);
	arm_send(&l, MSG(OPEN, 6, 0), SLOT - 112 - 8, NULL);
	CHECK_EQ(l.tx_pos, 3 * SLOT);
	CHECK_EQ(rd(&l.s, slave + QUEUE + 4 * (ARM_SLOTS + 2)), 36);
	CHECK(!vc_rang(&l));
	/* The largest message, a whole slot. */
	arm_send(&l, MSG(OPEN, 7, 0), SLOT - 8, NULL);
	CHECK_EQ(rd(&l.s, slave + QUEUE + 4 * (ARM_SLOTS + 3)), 37);
	CHECK_EQ(vc_sent(&l), 7);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	/*
	 * 9 bytes take 24, header and padding, before the next; what is
	 * past the 9 bytes, here a DATA's msgid, is not a message.
	 */
	arm_put(&l, MSG(OPEN, 20, 0), 9, nine);
	arm_put(&l, MSG(OPEN, 21, 0), 12, NULL);
	arm_signal(&l);
	run(&l, LATENCY);
	l.vc_read = 7;
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 20));
	CHECK_EQ(vc_next(&l), MSG(CLOSE, 0, 21));
	CHECK_EQ(l.tx_pos, 4 * SLOT + 24 + 24);
	arm_send(&l, MSG(OPEN, 22, 0), SLOT - 48 - 8, NULL);
	CHECK_EQ(rd(&l.s, slave + QUEUE + 4 * (ARM_SLOTS + 4)), 38);
	CHECK_EQ(vc_sent(&l), 10);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);

	/*
	 * Round the Arm's 31 slots, many times, a message a slot: each goes
	 * back once the padding after it is read, all but the last.
	 */
	for (unsigned i = 0; i < 200; i++)
		arm_send(&l, MSG(OPEN, 8, 0), 3000, NULL);
	CHECK_EQ(rd(&l.s, slave + QUEUE_RECYCLE), ARM_SLOTS + 5 + 199);
	CHECK_EQ(vc_sent(&l), 210);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	link_down(&l);
}

/*
 * The VideoCore's slots: 512 messages each, the next once the Arm has
 * recycled one; none free stops the machine.
 */
static void test_vc_slots(void)
{
	uint32_t master = SLOTS + MASTER;
	Link l;

	link_up(&l, true);
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	/* 31 slots of 512 messages; 100 OPENs a signal. */
	for (unsigned sent = 1; sent < VC_SLOTS * 512;) {
		for (unsigned i = 0; i < 100 && sent < VC_SLOTS * 512; i++, sent++)
			arm_put(&l, MSG(OPEN, 1, 0), 12, NULL);
		arm_signal(&l);
		run(&l, LATENCY);
		CHECK_EQ(vc_sent(&l), sent);
	}
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT);
	CHECK_EQ(rd(&l.s, SLOTS + 2 * SLOT + SLOT - 8), MSG(CLOSE, 0, 1));
	CHECK_EQ(rd(&l.s, SLOTS + 32 * SLOT + SLOT - 8), MSG(CLOSE, 0, 1));
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	/* None free. */
	arm_open(&l, 2, AUDS);
	check_stop(&l.s, "the VideoCore has no free slot to send in: waiting "
	                 "for the Arm to recycle one is not emulated");
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT);
	/* A slot recycled that is not the VideoCore's. */
	wr(&l.s, master + QUEUE + 4 * VC_SLOTS, 33);
	wr(&l.s, master + QUEUE_RECYCLE, VC_SLOTS + 1);
	wr(&l.s, master + RECYCLE + FIRED, 1);
	wr(&l.s, BELL2, 0);
	run(&l, LATENCY);
	CHECK_EQ(rd(&l.s, master + RECYCLE + FIRED), 0);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE); /* no message read */
	arm_signal(&l);
	run(&l, LATENCY);
	check_stop(&l.s, "the VideoCore's slot queue names slot 33, not one "
	                 "of its own, which is not emulated");
	wr(&l.s, master + QUEUE + 4 * VC_SLOTS, 1);
	arm_signal(&l);
	run(&l, LATENCY);
	check_stop(&l.s, "names slot 1, not one");
	/* Slot 2 back: the OPEN, read again, is answered in it. */
	wr(&l.s, master + QUEUE + 4 * VC_SLOTS, 2);
	arm_signal(&l);
	run(&l, LATENCY);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT + 8);
	CHECK_EQ(rd(&l.s, SLOTS + 2 * SLOT), MSG(CLOSE, 0, 2));
	CHECK_EQ(rd(&l.s, SLOTS + 2 * SLOT + 4), 0);
	link_down(&l);
}

/* A recycle event alone: the Arm's messages wait for its trigger. */
static void test_events(void)
{
	uint32_t master = SLOTS + MASTER;
	Link l;

	link_up(&l, true);
	arm_put(&l, MSG(CONNECT, 0, 0), 0, NULL);
	wr(&l.s, master + RECYCLE + FIRED, 1);
	wr(&l.s, BELL2, 0);
	run(&l, LATENCY);
	CHECK_EQ(rd(&l.s, master + RECYCLE + FIRED), 0);
	CHECK_EQ(vc_sent(&l), 0);
	arm_signal(&l);
	run(&l, LATENCY);
	CHECK_EQ(vc_sent(&l), 1);
	CHECK_EQ(rd(&l.s, master + RECYCLE + FIRED), 0);

	/* A reset drops the answer on its way. */
	arm_put(&l, MSG(OPEN, 1, 0), 12, NULL);
	arm_signal(&l);
	raspi3b_vchiq_reset(l.s.v);
	run(&l, 10 * LATENCY);
	CHECK_EQ(vc_sent(&l), 1);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	wr(&l.s, BELL2, 0);
	check_stop(&l.s, "the Arm rang the VideoCore's doorbell before "
	                 "VCHIQ_INIT, which is not emulated");
	run(&l, 10 * LATENCY); /* nor does it answer later */
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TRIGGER + FIRED), 1);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	link_down(&l);

	/* Destroyed with an answer on its way. */
	link_up(&l, true);
	arm_put(&l, MSG(CONNECT, 0, 0), 0, NULL);
	arm_signal(&l);
	link_down(&l);
}

/* What the VideoCore does not handle stops the machine. */
static void test_message_stops(void)
{
	static const struct {
		uint32_t msgid, size;
		const char *message;
	} stops[] = {
		{MSG(OPENACK, 1, 2), 2, "the Arm's OPENACK message is not emulated"},
		{MSG(DATA, 1, 2), 4, "the Arm's DATA message is not emulated"},
		{MSG(6, 1, 2), 0, "the Arm's BULK_RX message"},
		{MSG(7, 1, 2), 0, "the Arm's BULK_TX message"},
		{MSG(8, 1, 2), 0, "the Arm's BULK_RX_DONE message"},
		{MSG(9, 1, 2), 0, "the Arm's BULK_TX_DONE message"},
		{MSG(10, 0, 0), 0, "the Arm's PAUSE message"},
		{MSG(11, 0, 0), 0, "the Arm's RESUME message"},
		{MSG(12, 0, 0), 0, "the Arm's REMOTE_USE message"},
		{MSG(13, 0, 0), 0, "the Arm's REMOTE_RELEASE message"},
		{MSG(14, 0, 0), 0, "the Arm's REMOTE_USE_ACTIVE message"},
		{MSG(15, 0, 0), 0, "the Arm's message 0x0f000000 has no type"},
		{0xff000001u, 0, "the Arm's message 0xff000001 has no type"},
		{MSG(CONNECT, 0, 0), 0, "a second CONNECT is not emulated"},
	};
	Link l;

	for (size_t i = 0; i < ARRAY_SIZE(stops); i++) {
		link_up(&l, true);
		arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
		arm_send(&l, stops[i].msgid, stops[i].size, NULL);
		check_stop(&l.s, stops[i].message);
		/* Not read: the next signal stops again. */
		arm_signal(&l);
		run(&l, LATENCY);
		CHECK_EQ(l.s.m->stop, MACHINE_STOP_ERROR);
		link_down(&l);
	}

	/* A message past the end of its slot: 4080 bytes fit after 8. */
	for (uint32_t size = SLOT - 16; size <= SLOT - 15; size++) {
		link_up(&l, true);
		arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
		wr(&l.s, SLOTS + 34 * SLOT + 8, MSG(OPEN, 1, 0));
		wr(&l.s, SLOTS + 34 * SLOT + 12, size);
		wr(&l.s, SLOTS + SLAVE + TX_POS, SLOT);
		arm_signal(&l);
		run(&l, LATENCY);
		if (size == SLOT - 16) {
			CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
			CHECK_EQ(vc_sent(&l), 2);
			CHECK_EQ(rd(&l.s, SLOTS + SLAVE + QUEUE_RECYCLE), ARM_SLOTS + 1);
		} else {
			check_stop(&l.s, "the Arm's message 0x02001000 of 4081 bytes "
			                 "goes past the end of its slot");
			CHECK_EQ(vc_sent(&l), 1);
		}
		link_down(&l);
	}

	/* An OPEN before CONNECT. */
	link_up(&l, true);
	arm_open(&l, 1, AUDS);
	check_stop(&l.s, "an OPEN before CONNECT is not emulated");
	CHECK_EQ(vc_sent(&l), 0);
	arm_signal(&l); /* not read: it stops again */
	run(&l, LATENCY);
	check_stop(&l.s, "an OPEN before CONNECT is not emulated");
	link_down(&l);

	/* The Arm's slot queue names a slot that is not the Arm's. */
	for (uint32_t bad = 32; bad <= 65; bad += 33) {
		link_up(&l, true);
		wr(&l.s, SLOTS + SLAVE + QUEUE, bad == 32 ? 33 : 65);
		wr(&l.s, SLOTS + SLAVE + TX_POS, 8);
		arm_signal(&l);
		run(&l, LATENCY);
		check_stop(&l.s, bad == 32 ? "the Arm's slot queue names slot 33, "
		                             "not one of its own"
		                           : "the Arm's slot queue names slot 65");
		arm_signal(&l); /* not read: it stops again */
		run(&l, LATENCY);
		check_stop(&l.s, "the Arm's slot queue names slot");
		link_down(&l);
	}

	/* Synchronous messages: the rest waits. */
	link_up(&l, true);
	arm_put(&l, MSG(CONNECT, 0, 0), 0, NULL);
	wr(&l.s, SLOTS + MASTER + SYNC_TRIGGER + FIRED, 1);
	arm_signal(&l);
	run(&l, LATENCY);
	check_stop(&l.s, "the Arm's synchronous messages are not emulated");
	CHECK_EQ(vc_sent(&l), 0);
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TRIGGER + FIRED), 1);
	link_down(&l);

	/* The doorbell before VCHIQ_INIT. */
	{
		Setup s;

		setup(&s);
		raspi3b_vchiq_doorbell(s.v);
		check_stop(&s, "the Arm rang the VideoCore's doorbell before "
		               "VCHIQ_INIT, which is not emulated");
		teardown(&s);
	}
	raspi3b_vchiq_destroy(NULL);
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
	RUN_TEST(test_connect);
	RUN_TEST(test_open);
	RUN_TEST(test_restart);
	RUN_TEST(test_arm_slots);
	RUN_TEST(test_vc_slots);
	RUN_TEST(test_events);
	RUN_TEST(test_message_stops);
	RUN_TEST(test_tag);
	return test_summary();
}
