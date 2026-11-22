/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The Arm's end of VCHIQ, as Linux's vchiq_core plays it, for the unit
 * tests of the VideoCore's end and its services. Slot zero is laid out
 * as Linux's vchiq_init_slots (vchiq_core.c, Raspberry Pi's Linux at
 * cff533a) lays it out in the 65 slots that vchiq_arm.c allocates: the
 * master's synchronous slot 1 and slots 2 to 32, the slave's 33 and 34
 * to 64.
 */

#ifndef TESTS_UNIT_VCHIQ_LINK_H
#define TESTS_UNIT_VCHIQ_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/bus.h"
#include "core/event.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/misc/bcm2835_doorbell.h"
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

static inline void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	CHECK_EQ(bus_map_ram(s->m->bus, "ram", 0, RAM_SIZE), BUS_OK);
	s->v = raspi3b_vchiq_create(s->m, RAM_SIZE);
	CHECK(s->v != NULL);
}

static inline void teardown(Setup *s)
{
	raspi3b_vchiq_destroy(s->v);
	machine_destroy(s->m);
}

static inline uint32_t rd(Setup *s, uint64_t address)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, address, 4, &value), BUS_OK);
	return (uint32_t)value;
}

static inline void wr(Setup *s, uint64_t address, uint32_t value)
{
	CHECK_EQ(bus_write(s->m->bus, address, 4, value), BUS_OK);
}

/*
 * Slot zero at at, as Linux's vchiq_init_slots leaves it, and the rest of
 * the slots that the master's half points to filled with FILL.
 */
static inline void slot_zero(Setup *s, uint32_t at)
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

/* The machine stopped, with a message from who that has message in it. */
static inline void check_stop_by(Setup *s, const char *who, const char *message)
{
	CHECK_EQ(s->m->stop, MACHINE_STOP_ERROR);
	CHECK(strncmp(s->m->stop_message, who, strlen(who)) == 0);
	CHECK(strstr(s->m->stop_message, message) != NULL);
	s->m->stop = MACHINE_STOP_NONE;
}

static inline void check_stop(Setup *s, const char *message)
{
	check_stop_by(s, "firmware: VCHIQ: ", message);
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
	uint32_t vc_pos;  /* where vc_message reads the next */
	unsigned warnings;
	char warning[256];
} Link;

static inline void collect(void *ctx, const char *message)
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
static inline void link_start(Link *l)
{
	uint32_t slave = SLOTS + SLAVE;

	l->tx_pos = 0;
	l->vc_read = 0;
	l->vc_pos = 0;
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

static inline void link_up(Link *l, bool warn)
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

static inline void link_down(Link *l)
{
	bcm2835_doorbell_destroy(l->db);
	teardown(&l->s);
}

/*
 * reserve_space and queue_message: the Arm's message, its data words
 * (NULL: zeros), padding the rest of a slot that it does not fit.
 */
static inline void arm_put(Link *l, uint32_t msgid, uint32_t size,
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
static inline void arm_signal(Link *l)
{
	wr(&l->s, SLOTS + MASTER + TRIGGER + FIRED, 1);
	if (rd(&l->s, SLOTS + MASTER + TRIGGER + ARMED) != 0)
		wr(&l->s, BELL2, 0);
}

static inline void run(Link *l, uint64_t ns)
{
	event_queue_run_until(&l->s.m->events, l->s.m->events.now + ns);
}

/* Send, signal, and let the VideoCore answer. */
static inline void arm_send(Link *l, uint32_t msgid, uint32_t size,
                            const uint32_t *data)
{
	arm_put(l, msgid, size, data);
	arm_signal(l);
	run(l, LATENCY);
}

static inline void arm_open(Link *l, unsigned port, uint32_t fourcc)
{
	uint32_t payload[3] = {fourcc, 1234, 3u << 16 | 3};

	arm_send(l, MSG(OPEN, port, 0), 12, payload);
}

/* The VideoCore's messages sent so far. */
static inline unsigned vc_sent(Link *l)
{
	return rd(&l->s, SLOTS + MASTER + TX_POS) / 8;
}

/* The VideoCore's next message, which must be a header alone. */
static inline uint32_t vc_next(Link *l)
{
	uint32_t pos = 8 * l->vc_read++;
	uint32_t slot = rd(&l->s, SLOTS + MASTER + QUEUE + 4 * ((pos / SLOT) & 63));
	uint32_t at = SLOTS + slot * SLOT + (pos & (SLOT - 1));

	CHECK(slot >= 2 && slot <= 32);
	CHECK_EQ(rd(&l->s, at + 4), 0);
	return rd(&l->s, at);
}

/* Whether the VideoCore rang the Arm; reading BELL0 clears it. */
static inline bool vc_rang(Link *l)
{
	bool level = irq_level(&l->irq);

	CHECK_EQ(rd(&l->s, BELL0) != 0, level);
	CHECK(!irq_level(&l->irq));
	return level;
}

/*
 * The VideoCore's next message from vc_pos, padding too: its msgid,
 * and its size and data (NULL: not read).
 */
static inline uint32_t vc_message(Link *l, uint32_t *size, uint8_t *data)
{
	uint32_t pos = l->vc_pos;
	uint32_t slot = rd(&l->s, SLOTS + MASTER + QUEUE + 4 * ((pos / SLOT) & 63));
	uint32_t at = SLOTS + slot * SLOT + (pos & (SLOT - 1));

	CHECK(slot >= 2 && slot <= 32);
	*size = rd(&l->s, at + 4);
	if (data != NULL)
		CHECK_EQ(bus_read_block(l->s.m->bus, at + 8, data, *size), BUS_OK);
	l->vc_pos += (*size + 15) & ~7u;
	return rd(&l->s, at);
}

/* The VideoCore's next message, a header alone, from vc_pos. */
static inline uint32_t vc_header(Link *l)
{
	uint32_t size;
	uint32_t msgid = vc_message(l, &size, NULL);

	CHECK_EQ(size, 0);
	return msgid;
}

#endif /* TESTS_UNIT_VCHIQ_LINK_H */
