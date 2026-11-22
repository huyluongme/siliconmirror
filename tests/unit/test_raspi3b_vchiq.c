/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the VideoCore's end of VCHIQ
 * (src/machine/raspi3b_vchiq.c), against the Arm's end of
 * vchiq_link.h.
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
#include "vchiq_link.h"

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
 * A link whose VideoCore has filled its slots, with the answer to OPEN
 * 4 waiting and OPEN 5, which ends one of the Arm's slots, not read;
 * the Arm's slots recycled so far.
 */
static uint32_t fill_paused(Link *l)
{
	uint32_t space, recycled;

	link_up(l, true);
	arm_send(l, MSG(CONNECT, 0, 0), 0, NULL);
	for (unsigned sent = 1; sent < VC_SLOTS * 512;) {
		for (unsigned i = 0; i < 100 && sent < VC_SLOTS * 512; i++, sent++)
			arm_put(l, MSG(OPEN, 1, 0), 12, NULL);
		arm_signal(l);
		run(l, LATENCY);
	}
	space = SLOT - (l->tx_pos & (SLOT - 1));
	CHECK(space >= 48);
	arm_put(l, MSG(OPEN, 4, 0), 12, NULL);
	arm_put(l, MSG(OPEN, 5, 0), space - 24 - 8, NULL);
	CHECK_EQ(l->tx_pos & (SLOT - 1), 0);
	recycled = rd(&l->s, SLOTS + SLAVE + QUEUE_RECYCLE);
	arm_signal(l);
	run(l, LATENCY);
	CHECK_EQ(vc_sent(l), VC_SLOTS * 512);
	/* Its slot is not back: OPEN 5 waits. */
	CHECK_EQ(rd(&l->s, SLOTS + SLAVE + QUEUE_RECYCLE), recycled);
	return recycled;
}

/*
 * The VideoCore's slots: 512 messages each, the next once the Arm has
 * recycled one; with none free, its messages wait, and the Arm's too.
 */
static void test_vc_slots(void)
{
	uint32_t master = SLOTS + MASTER;
	uint32_t recycled;
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
	/* None free: the answer waits, and so does the next OPEN. */
	arm_open(&l, 2, AUDS);
	arm_open(&l, 3, AUDS);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT);
	CHECK_EQ(rd(&l.s, master + TRIGGER + FIRED), 1); /* not taken */
	/* A slot recycled that is not the VideoCore's. */
	wr(&l.s, master + QUEUE + 4 * VC_SLOTS, 33);
	wr(&l.s, master + QUEUE_RECYCLE, VC_SLOTS + 1);
	wr(&l.s, master + RECYCLE + FIRED, 1);
	wr(&l.s, BELL2, 0);
	run(&l, LATENCY);
	CHECK_EQ(rd(&l.s, master + RECYCLE + FIRED), 0);
	check_stop(&l.s, "the VideoCore's slot queue names slot 33, not one "
	                 "of its own, which is not emulated");
	wr(&l.s, master + QUEUE + 4 * VC_SLOTS, 1);
	wr(&l.s, BELL2, 0);
	run(&l, LATENCY);
	check_stop(&l.s, "names slot 1, not one");
	/*
	 * Slot 2 back: the answer goes in it; then, the trigger taken, the
	 * OPEN that waited is read and answered.
	 */
	wr(&l.s, master + QUEUE + 4 * VC_SLOTS, 2);
	wr(&l.s, BELL2, 0);
	run(&l, LATENCY);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT + 16);
	CHECK_EQ(rd(&l.s, SLOTS + 2 * SLOT), MSG(CLOSE, 0, 2));
	CHECK_EQ(rd(&l.s, SLOTS + 2 * SLOT + 4), 0);
	CHECK_EQ(rd(&l.s, SLOTS + 2 * SLOT + 8), MSG(CLOSE, 0, 3));
	CHECK_EQ(rd(&l.s, master + TRIGGER + FIRED), 0);
	link_down(&l);

	/*
	 * Paused with the trigger taken: the recycle event alone, with the
	 * slot back, sends the answer and reads on.
	 */
	recycled = fill_paused(&l);
	CHECK_EQ(rd(&l.s, master + TRIGGER + FIRED), 0);
	wr(&l.s, master + QUEUE + 4 * VC_SLOTS, 2);
	wr(&l.s, master + QUEUE_RECYCLE, VC_SLOTS + 1);
	wr(&l.s, master + RECYCLE + FIRED, 1);
	wr(&l.s, BELL2, 0);
	run(&l, LATENCY);
	CHECK_EQ(rd(&l.s, SLOTS + 2 * SLOT), MSG(CLOSE, 0, 4));
	CHECK_EQ(rd(&l.s, SLOTS + 2 * SLOT + 8), MSG(CLOSE, 0, 5));
	CHECK_EQ(rd(&l.s, SLOTS + SLAVE + QUEUE_RECYCLE), recycled + 1);
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT + 16);
	/* Read to the end: a recycle event alone reads nothing more. */
	arm_put(&l, MSG(OPEN, 6, 0), 12, NULL);
	wr(&l.s, master + RECYCLE + FIRED, 1);
	wr(&l.s, BELL2, 0);
	run(&l, LATENCY);
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT + 16);
	link_down(&l);

	/* Paused, then the firmware starts again: it is paused no more. */
	fill_paused(&l);
	raspi3b_vchiq_reset(l.s.v);
	link_start(&l);
	arm_put(&l, MSG(CONNECT, 0, 0), 0, NULL);
	wr(&l.s, master + RECYCLE + FIRED, 1);
	wr(&l.s, BELL2, 0);
	run(&l, LATENCY);
	CHECK_EQ(rd(&l.s, master + TX_POS), 0);
	arm_signal(&l);
	run(&l, LATENCY);
	CHECK_EQ(rd(&l.s, master + TX_POS), 8);
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

#define SMEM 0x534d454du /* 'SMEM' */

/* A service of the VideoCore's, as the tests play it. */
typedef struct Svc {
	Link *l;
	unsigned opens, datas, closes, resets;
	unsigned port; /* the last call's */
	uint32_t size;
	uint8_t bytes[SLOT];
	bool echo; /* sends back what it gets */
	bool fail; /* stops the machine on DATA */
} Svc;

static void svc_open(void *ctx, unsigned port)
{
	Svc *v = ctx;

	v->opens++;
	v->port = port;
}

static bool svc_data(void *ctx, unsigned port, const uint8_t *data,
                     uint32_t size)
{
	Svc *v = ctx;

	v->datas++;
	v->port = port;
	v->size = size;
	memcpy(v->bytes, data, size);
	if (v->fail) {
		machine_stop(v->l->s.m, MACHINE_STOP_ERROR, "svc");
		return false;
	}
	return !v->echo || raspi3b_vchiq_send(v->l->s.v, port, data, size);
}

static void svc_close(void *ctx, unsigned port)
{
	Svc *v = ctx;

	v->closes++;
	v->port = port;
}

static void svc_reset(void *ctx)
{
	Svc *v = ctx;

	v->resets++;
}

/* A link with the CONNECTs done, and SMEM version 3, from 2. */
static void svc_up(Link *l, Svc *v)
{
	Raspi3bVchiqService s = {SMEM,     3,         2, svc_open,
	                         svc_data, svc_close, v, svc_reset};

	link_up(l, true);
	memset(v, 0, sizeof(*v));
	v->l = l;
	CHECK(raspi3b_vchiq_add_service(l->s.v, &s));
	arm_send(l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(l), MSG(CONNECT, 0, 0));
}

static void arm_open_v(Link *l, unsigned port, uint32_t fourcc,
                       uint16_t version, uint16_t version_min)
{
	uint32_t payload[3] = {fourcc, 99, (uint32_t)version_min << 16 | version};

	arm_send(l, MSG(OPEN, port, 0), 12, payload);
}

/* The VideoCore's next message is the OPENACK from port to arm_port. */
static void check_openack(Link *l, unsigned port, unsigned arm_port)
{
	uint32_t size;
	uint8_t ack[SLOT];

	CHECK_EQ(vc_message(l, &size, ack), MSG(OPENACK, port, arm_port));
	CHECK_EQ(size, 2);
	CHECK_EQ(ack[0] | ack[1] << 8, 3); /* the service's version */
}

/*
 * parse_open: a service the VideoCore has, whose versions meet the
 * Arm's, is acknowledged from its lowest free port; the others are
 * refused.
 */
static void test_service_open(void)
{
	static const struct {
		uint16_t version, version_min;
		bool meets;
	} versions[] = {
		{2, 0, true}, /* the Arm's version the VideoCore's oldest */
		{3, 3, true}, /* the Arm's oldest the VideoCore's version */
		{9, 1, true},
		{1, 0, false},      /* older than the VideoCore's oldest */
		{9, 4, false},      /* the Arm's oldest newer than the VideoCore's */
		{0xffff, 0, false}, /* -1 */
		{9, 0xffff, true},  /* from -1 */
	};
	Link l;
	Svc v;

	for (size_t i = 0; i < ARRAY_SIZE(versions); i++) {
		svc_up(&l, &v);
		arm_open_v(&l, 7, SMEM, versions[i].version, versions[i].version_min);
		if (versions[i].meets) {
			check_openack(&l, 1, 7);
			CHECK_EQ(v.opens, 1);
			CHECK_EQ(v.port, 1);
			CHECK_EQ(l.warnings, 0);
		} else {
			CHECK_EQ(vc_header(&l), MSG(CLOSE, 0, 7));
			CHECK_EQ(v.opens, 0);
			CHECK_EQ(l.warnings, 1);
		}
		link_down(&l);
	}
	svc_up(&l, &v);
	arm_open_v(&l, 7, SMEM, 1, 0);
	CHECK(strcmp(l.warning, "VCHIQ: the Arm's 'SMEM' version 1 (from 0) does "
	                        "not meet the VideoCore's, 3 (from 2): it refuses "
	                        "the OPEN") == 0);
	arm_open_v(&l, 8, SMEM, 1, 0);
	CHECK_EQ(l.warnings, 2); /* each time */
	/* The lowest free port each time; the Arm's ports as they come. */
	arm_open_v(&l, 0x123, SMEM, 3, 2);
	arm_open_v(&l, 0xfff, SMEM, 3, 2);
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 0, 7));
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 0, 8));
	check_openack(&l, 1, 0x123);
	check_openack(&l, 2, 0xfff);
	CHECK_EQ(v.opens, 2);
	CHECK_EQ(v.port, 2);
	/*
	 * An OPENACK with no slot to go in, the slot queue broken: the
	 * service is open, and the OPENACK waits; it goes once mended.
	 */
	wr(&l.s, SLOTS + MASTER + QUEUE, 33);
	arm_open_v(&l, 0x124, SMEM, 3, 2);
	check_stop(&l.s, "the VideoCore's slot queue names slot 33");
	CHECK_EQ(v.opens, 3);
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TX_POS), l.vc_pos);
	wr(&l.s, SLOTS + MASTER + QUEUE, 2);
	arm_signal(&l);
	run(&l, LATENCY);
	check_openack(&l, 3, 0x124);
	CHECK_EQ(v.opens, 3);
	arm_send(&l, MSG(CLOSE, 0x124, 3), 0, NULL);
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 3, 0x124));
	/* What it does not have is refused, and named, as before. */
	arm_open_v(&l, 9, MMAL, 3, 2);
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 0, 9));
	CHECK(strstr(l.warning, "service 'mmal' is not emulated") != NULL);
	/* Too short to name one. */
	arm_send(&l, MSG(OPEN, 10, 0), 11, (uint32_t[3]){SMEM, 0, 0x00020003});
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 0, 10));
	CHECK_EQ(l.warnings, 3);
	/* 32 ports: the 33rd OPEN stops. */
	for (unsigned port = 3; port <= 32; port++) {
		arm_open_v(&l, 100 + port, SMEM, 3, 2);
		check_openack(&l, port, 100 + port);
	}
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	/* Port 32, the last, is one like the others. */
	CHECK(raspi3b_vchiq_send(l.s.v, 32, "x", 1));
	CHECK_EQ(vc_message(&l, &(uint32_t){0}, NULL), MSG(DATA, 32, 132));
	arm_send(&l, MSG(DATA, 132, 32), 4, NULL);
	CHECK_EQ(v.datas, 1);
	CHECK_EQ(v.port, 32);
	arm_send(&l, MSG(CLOSE, 132, 0), 0, NULL);
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 32, 132));
	arm_send(&l, MSG(CLOSE, 0x123, 0), 0, NULL);
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 1, 0x123));
	CHECK_EQ(v.closes, 3); /* with port 3 before */
	arm_open_v(&l, 300, SMEM, 3, 2);
	arm_open_v(&l, 301, SMEM, 3, 2);
	check_openack(&l, 1, 300);
	check_openack(&l, 32, 301);
	arm_open_v(&l, 200, SMEM, 3, 2);
	check_stop(&l.s, "more than 32 of the VideoCore's services open is "
	                 "not emulated");
	/* Not read: the next signal stops again. */
	arm_signal(&l);
	run(&l, LATENCY);
	check_stop(&l.s, "more than 32 of the VideoCore's services open");
	/* The firmware starts again: the 32 close. */
	raspi3b_vchiq_reset(l.s.v);
	CHECK_EQ(v.closes, 3 + 32);
	link_down(&l);

	/* Without warnings, and with no callbacks. */
	link_up(&l, false);
	CHECK(raspi3b_vchiq_add_service(
		l.s.v,
		&(Raspi3bVchiqService){SMEM, 3, 2, NULL, NULL, NULL, NULL, NULL}));
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&l), MSG(CONNECT, 0, 0));
	arm_open_v(&l, 7, SMEM, 1, 0);
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 0, 7));
	arm_open_v(&l, 7, SMEM, 3, 2);
	check_openack(&l, 1, 7);
	arm_send(&l, MSG(DATA, 7, 1), 4, NULL);
	arm_send(&l, MSG(CLOSE, 7, 1), 0, NULL);
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 1, 7));
	CHECK_EQ(l.warnings, 0);
	arm_open_v(&l, 8, SMEM, 3, 2);
	check_openack(&l, 1, 8);
	raspi3b_vchiq_reset(l.s.v); /* nothing to call */
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	link_down(&l);

	/* Eight services, then no room. */
	link_up(&l, false);
	for (uint32_t i = 0; i < 8; i++)
		CHECK(raspi3b_vchiq_add_service(
			l.s.v, &(Raspi3bVchiqService){SMEM + i, 1, 1, NULL, NULL, NULL,
		                                  NULL, NULL}));
	CHECK(!raspi3b_vchiq_add_service(
		l.s.v,
		&(Raspi3bVchiqService){MMAL, 1, 1, NULL, NULL, NULL, NULL, NULL}));
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&l), MSG(CONNECT, 0, 0));
	arm_open_v(&l, 7, SMEM + 7, 1, 1);
	CHECK_EQ(vc_message(&l, &(uint32_t){0}, NULL), MSG(OPENACK, 1, 7));
	link_down(&l);
}

/* DATA, both ways, between the Arm's port and the service it opened. */
static void test_service_data(void)
{
	uint32_t words[3] = {0x04030201u, 0x08070605u, 0x0c0b0a09u};
	uint8_t big[SLOT];
	uint32_t size;
	Link l;
	Svc v;

	svc_up(&l, &v);
	arm_open_v(&l, 7, SMEM, 3, 2);
	arm_open_v(&l, 8, SMEM, 3, 2);
	check_openack(&l, 1, 7);
	check_openack(&l, 2, 8);
	arm_send(&l, MSG(DATA, 8, 2), 9, words);
	CHECK_EQ(v.datas, 1);
	CHECK_EQ(v.port, 2);
	CHECK_EQ(v.size, 9);
	CHECK(memcmp(v.bytes, words, 9) == 0);
	arm_send(&l, MSG(DATA, 7, 1), 0, NULL);
	CHECK_EQ(v.datas, 2);
	CHECK_EQ(v.port, 1);
	CHECK_EQ(v.size, 0);
	/* Dropped: from another Arm port, to no port, port 0, past 32. */
	arm_send(&l, MSG(DATA, 8, 1), 4, words);
	arm_send(&l, MSG(DATA, 7, 3), 4, words);
	arm_send(&l, MSG(DATA, 7, 0), 4, words);
	arm_send(&l, MSG(DATA, 7, 33), 4, words);
	arm_send(&l, MSG(DATA, 7, 0x801), 4, words);
	CHECK_EQ(v.datas, 2);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);

	/* The service's DATA to the Arm's port, from its own. */
	v.echo = true;
	arm_send(&l, MSG(DATA, 8, 2), 9, words);
	CHECK_EQ(vc_message(&l, &size, big), MSG(DATA, 2, 8));
	CHECK_EQ(size, 9);
	CHECK(memcmp(big, words, 9) == 0);
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TX_POS), l.vc_pos);
	/* The largest, a whole slot: the rest of slot 2 is padding. */
	for (unsigned i = 0; i < SLOT; i++)
		big[i] = (uint8_t)(i * 7);
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, RASPI3B_VCHIQ_MSG_MAX));
	CHECK_EQ(vc_message(&l, &size, NULL), MSG(PADDING, 0, 0));
	CHECK_EQ(size, SLOT - 64 - 8); /* 64 bytes sent before it */
	CHECK_EQ(l.vc_pos, SLOT);
	{
		uint8_t back[SLOT];

		CHECK_EQ(vc_message(&l, &size, back), MSG(DATA, 1, 7));
		CHECK_EQ(size, RASPI3B_VCHIQ_MSG_MAX);
		CHECK(memcmp(back, big, size) == 0);
	}
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TX_POS), 2 * SLOT);
	/* Exactly the rest of a slot: no padding. */
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, 4000));
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, SLOT - 4008 - 16));
	CHECK(raspi3b_vchiq_send(l.s.v, 2, big, 0));
	CHECK_EQ(vc_message(&l, &size, NULL), MSG(DATA, 1, 7));
	CHECK_EQ(vc_message(&l, &size, NULL), MSG(DATA, 1, 7));
	CHECK_EQ(size, SLOT - 4008 - 16);
	CHECK_EQ(l.vc_pos, 3 * SLOT - 8);
	CHECK_EQ(vc_header(&l), MSG(DATA, 2, 8));
	CHECK_EQ(l.vc_pos, 3 * SLOT);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	/* One byte. */
	CHECK(raspi3b_vchiq_send(l.s.v, 2, "\xa5", 1));
	CHECK_EQ(vc_message(&l, &size, big), MSG(DATA, 2, 8));
	CHECK_EQ(size, 1);
	CHECK_EQ(big[0], 0xa5);
	/*
	 * The slot that padding goes in, broken in the slot queue: it stops,
	 * and the message waits; mended, it goes before the next.
	 */
	wr(&l.s, SLOTS + MASTER + QUEUE + 4 * 3, 33);
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, RASPI3B_VCHIQ_MSG_MAX));
	check_stop(&l.s, "the VideoCore's slot queue names slot 33");
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TX_POS), 3 * SLOT + 16);
	wr(&l.s, SLOTS + MASTER + QUEUE + 4 * 3, 5);
	CHECK(raspi3b_vchiq_send(l.s.v, 2, big, 0));
	CHECK_EQ(vc_message(&l, &size, NULL), MSG(PADDING, 0, 0));
	CHECK_EQ(vc_message(&l, &size, NULL), MSG(DATA, 1, 7));
	CHECK_EQ(size, RASPI3B_VCHIQ_MSG_MAX);
	CHECK_EQ(vc_header(&l), MSG(DATA, 2, 8));
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TX_POS), 5 * SLOT + 8);
	/* What does not go stops. */
	CHECK(!raspi3b_vchiq_send(l.s.v, 1, big, RASPI3B_VCHIQ_MSG_MAX + 1));
	check_stop(&l.s, "a message of 4089 bytes does not fit a slot");
	CHECK(!raspi3b_vchiq_send(l.s.v, 3, big, 4));
	check_stop(&l.s, "a service sends from port 3, which is not open");
	CHECK(!raspi3b_vchiq_send(l.s.v, 0, big, 4));
	check_stop(&l.s, "from port 0, which");
	CHECK(!raspi3b_vchiq_send(l.s.v, 33, big, 4));
	check_stop(&l.s, "from port 33, which");
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TX_POS), 5 * SLOT + 8);
	/* A service that stops: the DATA is read again at the next signal. */
	v.echo = false;
	v.fail = true;
	arm_send(&l, MSG(DATA, 7, 1), 4, words);
	CHECK_EQ(v.datas, 4);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_ERROR);
	l.s.m->stop = MACHINE_STOP_NONE;
	arm_signal(&l);
	run(&l, LATENCY);
	CHECK_EQ(v.datas, 5);
	link_down(&l);
}

/*
 * vchiq_close_service_internal, open and CLOSE received: the service
 * closes, answers with a CLOSE, and its port is free again.
 */
static void test_service_close(void)
{
	Link l;
	Svc v;

	svc_up(&l, &v);
	arm_open_v(&l, 7, SMEM, 3, 2);
	arm_open_v(&l, 8, SMEM, 3, 2);
	arm_open_v(&l, 9, SMEM, 3, 2);
	check_openack(&l, 1, 7);
	check_openack(&l, 2, 8);
	check_openack(&l, 3, 9);
	/* Not the Arm's port that opened it, nor an open port: dropped. */
	arm_send(&l, MSG(CLOSE, 8, 1), 0, NULL);
	arm_send(&l, MSG(CLOSE, 7, 4), 0, NULL);
	arm_send(&l, MSG(CLOSE, 7, 33), 0, NULL);
	arm_send(&l, MSG(CLOSE, 10, 0), 0, NULL);
	CHECK_EQ(v.closes, 0);
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TX_POS), l.vc_pos);
	arm_send(&l, MSG(CLOSE, 8, 2), 0, NULL);
	CHECK_EQ(v.closes, 1);
	CHECK_EQ(v.port, 2);
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 2, 8));
	/* Closed: its DATA and a second CLOSE are dropped. */
	arm_send(&l, MSG(DATA, 8, 2), 0, NULL);
	arm_send(&l, MSG(CLOSE, 8, 2), 0, NULL);
	CHECK_EQ(v.datas, 0);
	CHECK_EQ(v.closes, 1);
	/* To port 0: the port the Arm's opened. */
	arm_send(&l, MSG(CLOSE, 9, 0), 0, NULL);
	CHECK_EQ(v.port, 3);
	CHECK_EQ(vc_header(&l), MSG(CLOSE, 3, 9));
	CHECK_EQ(rd(&l.s, SLOTS + MASTER + TX_POS), l.vc_pos);
	/* Port 2 is the lowest free again. */
	arm_open_v(&l, 11, SMEM, 3, 2);
	check_openack(&l, 2, 11);
	CHECK_EQ(v.opens, 4);
	/*
	 * The firmware starts again: what is open closes, then each service
	 * resets, a second one too.
	 */
	{
		Svc w = {.l = &l};

		CHECK(raspi3b_vchiq_add_service(
			l.s.v, &(Raspi3bVchiqService){MMAL, 1, 1, NULL, NULL, NULL, &w,
		                                  svc_reset}));
		CHECK_EQ(v.resets, 0);
		raspi3b_vchiq_reset(l.s.v);
		CHECK_EQ(v.closes, 4);
		CHECK_EQ(v.resets, 1);
		CHECK_EQ(w.resets, 1);
	}
	link_start(&l);
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&l), MSG(CONNECT, 0, 0));
	arm_send(&l, MSG(CLOSE, 7, 1), 0, NULL);
	CHECK_EQ(v.closes, 4);
	arm_open_v(&l, 12, SMEM, 3, 2);
	check_openack(&l, 1, 12);
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	link_down(&l);
}

/* The messages waiting for slots: in order, and at most 64 KB. */
static void test_service_waiting(void)
{
	uint32_t master = SLOTS + MASTER;
	static uint8_t big[SLOT];
	Link l;
	Svc v;

	svc_up(&l, &v);
	arm_open_v(&l, 7, SMEM, 3, 2);
	check_openack(&l, 1, 7);
	/* 24 bytes so far; fill the 31 slots, less 8 bytes. */
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, SLOT - 24 - 8));
	for (unsigned i = 1; i < VC_SLOTS - 1; i++)
		CHECK(raspi3b_vchiq_send(l.s.v, 1, big, SLOT - 8));
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, SLOT - 16));
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT - 8);
	/* A header alone fits; the next waits, the padding not yet sent. */
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, 0));
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, 100));
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT);
	/* 64 KB wait, in all. */
	for (unsigned i = 0; i < 16; i++)
		CHECK(raspi3b_vchiq_send(l.s.v, 1, big, SLOT - 8));
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, 65536 - 100 - 16 * (SLOT - 8)));
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, 0));
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	CHECK(!raspi3b_vchiq_send(l.s.v, 1, big, 1));
	check_stop(&l.s, "more than 64 KB of the VideoCore's messages waiting "
	                 "for the Arm to recycle slots is not emulated");
	/*
	 * A slot back that is not the VideoCore's: what waits stops at it,
	 * and a header alone still fits the 64 KB.
	 */
	wr(&l.s, master + QUEUE + 4 * VC_SLOTS, 33);
	wr(&l.s, master + QUEUE_RECYCLE, VC_SLOTS + 1);
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, 0));
	check_stop(&l.s, "the VideoCore's slot queue names slot 33");
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT);
	/* Two slots back: the 100 bytes, then the first that fill one. */
	wr(&l.s, master + QUEUE + 4 * VC_SLOTS, 5);
	wr(&l.s, master + QUEUE + 4 * (VC_SLOTS + 1), 9);
	wr(&l.s, master + QUEUE_RECYCLE, VC_SLOTS + 2);
	wr(&l.s, master + RECYCLE + FIRED, 1);
	wr(&l.s, BELL2, 0);
	run(&l, LATENCY);
	CHECK_EQ(rd(&l.s, master + TX_POS), (VC_SLOTS + 1) * SLOT + SLOT);
	CHECK_EQ(rd(&l.s, SLOTS + 5 * SLOT), MSG(DATA, 1, 7));
	CHECK_EQ(rd(&l.s, SLOTS + 5 * SLOT + 4), 100);
	CHECK_EQ(rd(&l.s, SLOTS + 5 * SLOT + 112), MSG(PADDING, 0, 0));
	CHECK_EQ(rd(&l.s, SLOTS + 5 * SLOT + 116), SLOT - 112 - 8);
	CHECK_EQ(rd(&l.s, SLOTS + 9 * SLOT + 4), SLOT - 8);
	/* Room again: a byte more waits behind them. */
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, 1));
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	/* A reset drops what waits. */
	raspi3b_vchiq_reset(l.s.v);
	link_start(&l);
	CHECK_EQ(rd(&l.s, master + TX_POS), 0);
	arm_send(&l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(rd(&l.s, master + TX_POS), 8);
	/* The 64 KB are whole again. */
	arm_open_v(&l, 7, SMEM, 3, 2);
	CHECK_EQ(rd(&l.s, master + TX_POS), 24);
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, SLOT - 24 - 8));
	for (unsigned i = 1; i < VC_SLOTS; i++)
		CHECK(raspi3b_vchiq_send(l.s.v, 1, big, SLOT - 8));
	CHECK_EQ(rd(&l.s, master + TX_POS), VC_SLOTS * SLOT);
	for (unsigned i = 0; i < 16; i++)
		CHECK(raspi3b_vchiq_send(l.s.v, 1, big, SLOT - 8));
	CHECK(raspi3b_vchiq_send(l.s.v, 1, big, 65536 - 16 * (SLOT - 8)));
	CHECK_EQ(l.s.m->stop, MACHINE_STOP_NONE);
	CHECK(!raspi3b_vchiq_send(l.s.v, 1, big, 1));
	check_stop(&l.s, "more than 64 KB");
	link_down(&l);

	/* Destroyed with messages waiting. */
	svc_up(&l, &v);
	arm_open_v(&l, 7, SMEM, 3, 2);
	for (unsigned i = 0; i < VC_SLOTS + 2; i++)
		CHECK(raspi3b_vchiq_send(l.s.v, 1, big, SLOT - 8));
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
	RUN_TEST(test_service_open);
	RUN_TEST(test_service_data);
	RUN_TEST(test_service_close);
	RUN_TEST(test_service_waiting);
	RUN_TEST(test_message_stops);
	RUN_TEST(test_tag);
	return test_summary();
}
