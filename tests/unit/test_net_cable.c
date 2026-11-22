/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the cable between a board's Ethernet port and the host
 * (src/machine/net_cable.c): what the partner's MAC drops, the frames
 * one at a time at the link's speed (IEEE 802.3 clause 4: preamble and
 * delimiter, 8 bytes, and a gap of 12 count with the frame), and the
 * host's failures.
 */

#include <stdint.h>
#include <string.h>

#include "core/event.h"
#include "core/net.h"
#include "machine/net_cable.h"
#include "test.h"

#define QUEUE 80

/* The host's side: frames waiting for the guest, and what it got. */
typedef struct FakeLink {
	NetLink link;
	uint8_t queue[QUEUE][1600];
	size_t queue_len[QUEUE];
	unsigned head, count;
	bool fail_send, fail_receive;
	bool big; /* the next frame is the longest */
	unsigned sent;
	uint64_t sent_at;
	size_t sent_len;
	uint8_t sent_frame[1600];
} FakeLink;

static bool fake_send(NetLink *n, uint64_t now, const uint8_t *frame,
                      size_t len)
{
	FakeLink *f = (FakeLink *)n;

	if (f->fail_send) {
		strcpy(n->error, "send broke");
		return false;
	}
	f->sent++;
	f->sent_at = now;
	f->sent_len = len;
	memcpy(f->sent_frame, frame, len);
	return true;
}

static bool fake_receive(NetLink *n, uint8_t *frame, size_t *len)
{
	FakeLink *f = (FakeLink *)n;

	*len = 0;
	if (f->fail_receive) {
		*len = 100; /* what a failure leaves is no frame */
		strcpy(n->error, "receive broke");
		return false;
	}
	if (f->big) {
		f->big = false;
		memset(frame, 0x77, NET_MAX_FRAME);
		*len = NET_MAX_FRAME;
		return true;
	}
	if (f->count == 0)
		return true;
	*len = f->queue_len[f->head];
	memcpy(frame, f->queue[f->head], *len);
	f->head = (f->head + 1) % QUEUE;
	f->count--;
	return true;
}

static void fake_destroy(NetLink *n)
{
	(void)n;
}

static const NetLinkOps fake_ops = {fake_send, fake_receive, fake_destroy};

/* The board's port. */
typedef struct FakePort {
	unsigned speed;
	bool drop;
	unsigned got;
	uint64_t got_at[8];
	size_t got_len[8];
	uint8_t got_frame[8][1600];
} FakePort;

static size_t big_len;
static bool big_ok;

static bool port_receive(void *ctx, const uint8_t *frame, size_t len)
{
	FakePort *p = ctx;

	if (len > 1600) {
		big_len = len;
		big_ok = frame[len - 5] == 0x77 && net_from_wire(frame, len) == len - 4;
		p->got++;
		return true;
	}
	if (p->got < 8) {
		p->got_len[p->got] = len;
		memcpy(p->got_frame[p->got], frame, len);
	}
	p->got++;
	return !p->drop;
}

static unsigned port_speed(void *ctx)
{
	return ((FakePort *)ctx)->speed;
}

typedef struct Setup {
	Machine *m;
	FakeLink link;
	FakePort port;
	NetCable *c;
} Setup;

/* What the port's receive saw, with the time. */
static Setup *current;

static bool timed_receive(void *ctx, const uint8_t *frame, size_t len)
{
	FakePort *p = ctx;

	if (p->got < 8)
		p->got_at[p->got] = event_queue_now(&current->m->events);
	return port_receive(ctx, frame, len);
}

static void setup(Setup *s)
{
	char error[100];
	NetCablePort port = {timed_receive, port_speed, &s->port};

	memset(s, 0, sizeof(*s));
	current = s;
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->link.link.ops = &fake_ops;
	s->port.speed = 100;
	s->c = net_cable_create(s->m, &s->link.link, &port, error, sizeof(error));
	CHECK(s->c != NULL);
	CHECK_EQ(s->m->num_inputs, 1);
}

static void teardown(Setup *s)
{
	net_cable_destroy(s->c);
	CHECK_EQ(s->m->num_inputs, 0);
	machine_destroy(s->m);
}

static void queue(Setup *s, size_t len, uint8_t fill)
{
	unsigned at = (s->link.head + s->link.count) % QUEUE;

	memset(s->link.queue[at], fill, len);
	s->link.queue_len[at] = len;
	s->link.count++;
}

static void poll(Setup *s)
{
	s->m->inputs[0].poll(s->m->inputs[0].ctx);
}

static void run_until(Setup *s, uint64_t t)
{
	event_queue_run_until(&s->m->events, t);
}

/* The port's frames: the FCS off; runts and wrong FCSs dropped. */
static void test_send(void)
{
	Setup s;
	uint8_t frame[60], wire[64];

	setup(&s);
	for (int i = 0; i < 60; i++)
		frame[i] = (uint8_t)(i + 3);
	CHECK_EQ(net_to_wire(frame, 60, wire), 64);
	run_until(&s, 777);
	net_cable_send(s.c, wire, 64);
	CHECK_EQ(s.link.sent, 1);
	CHECK_EQ(s.link.sent_at, 777);
	CHECK_EQ(s.link.sent_len, 60);
	CHECK(memcmp(s.link.sent_frame, frame, 60) == 0);
	net_cable_send(s.c, wire, 63);
	wire[10] ^= 1;
	net_cable_send(s.c, wire, 64);
	CHECK_EQ(s.link.sent, 1);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);

	/* The host fails: the machine stops, saying why. */
	wire[10] ^= 1;
	s.link.fail_send = true;
	net_cable_send(s.c, wire, 64);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strcmp(s.m->stop_message, "the host's network: send broke") == 0);
	teardown(&s);
}

/* The host's frames, one after another, at 100 and at 10 Mb/s. */
static void test_receive_paced(void)
{
	Setup s;

	setup(&s);
	run_until(&s, 1000);
	queue(&s, 42, 0x11);   /* 64 bytes on the cable */
	queue(&s, 1000, 0x22); /* 1004 */
	poll(&s);
	CHECK_EQ(s.link.count, 1); /* one on the cable */
	poll(&s);
	CHECK_EQ(s.link.count, 1); /* the cable is busy */
	/* (8 + 64 + 12) bytes, 80 ns each. */
	run_until(&s, 1000 + 84 * 80 - 1);
	CHECK_EQ(s.port.got, 0);
	run_until(&s, 1000 + 84 * 80);
	CHECK_EQ(s.port.got, 1);
	CHECK_EQ(s.port.got_at[0], 1000 + 84 * 80);
	CHECK_EQ(s.port.got_len[0], 64);
	CHECK_EQ(s.port.got_frame[0][0], 0x11);
	CHECK_EQ(s.port.got_frame[0][41], 0x11);
	CHECK_EQ(s.port.got_frame[0][42], 0); /* the pad */
	CHECK_EQ(net_from_wire(s.port.got_frame[0], 64), 60);
	/* The next at once after it: (8 + 1004 + 12) bytes. */
	CHECK_EQ(s.link.count, 0);
	run_until(&s, 1000 + 84 * 80 + 1024 * 80);
	CHECK_EQ(s.port.got, 2);
	CHECK_EQ(s.port.got_at[1], 1000 + 84 * 80 + 1024 * 80);
	CHECK_EQ(s.port.got_len[1], 1004);
	CHECK_EQ(net_from_wire(s.port.got_frame[1], 1004), 1000);

	/* At 10 Mb/s, 800 ns a byte; a port that drops it changes nothing. */
	s.port.speed = 10;
	s.port.drop = true;
	queue(&s, 100, 0x33);
	queue(&s, 100, 0x44);
	uint64_t t = event_queue_now(&s.m->events);
	poll(&s);
	run_until(&s, t + 124 * 800 - 1);
	CHECK_EQ(s.port.got, 2);
	run_until(&s, t + 124 * 800);
	CHECK_EQ(s.port.got, 3);
	run_until(&s, t + 2 * 124 * 800);
	CHECK_EQ(s.port.got, 4);
	CHECK_EQ(s.port.got_frame[3][0], 0x44);
	CHECK_EQ(event_queue_next(&s.m->events), EVENT_NEVER);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* With no link the host's frames are lost, 64 at a poll at most. */
static void test_no_link(void)
{
	Setup s;

	setup(&s);
	s.port.speed = 0;
	for (int i = 0; i < 70; i++)
		queue(&s, 60, (uint8_t)i);
	poll(&s);
	CHECK_EQ(s.link.count, 6);
	CHECK_EQ(event_queue_next(&s.m->events), EVENT_NEVER);
	/* The link up: the next goes on the cable. */
	s.port.speed = 100;
	poll(&s);
	CHECK_EQ(s.link.count, 5);
	run_until(&s, 84 * 80);
	CHECK_EQ(s.port.got, 1);
	CHECK_EQ(s.port.got_frame[0][0], 64);
	CHECK_EQ(s.link.count, 4);
	teardown(&s);
}

/* The longest frame the host passes, whole with its FCS. */
static void test_receive_longest(void)
{
	Setup s;

	setup(&s);
	s.link.big = true;
	poll(&s);
	run_until(&s, (uint64_t)(8 + NET_MAX_FRAME + 4 + 12) * 80);
	CHECK_EQ(s.port.got, 1);
	CHECK_EQ(big_len, NET_MAX_FRAME + 4);
	CHECK(big_ok);
	teardown(&s);
}

/* The host fails to give a frame: the machine stops, saying why. */
static void test_receive_fails(void)
{
	Setup s;

	setup(&s);
	CHECK(!s.m->inputs[0].wait(s.m->inputs[0].ctx)); /* never blocks */
	s.link.fail_receive = true;
	poll(&s);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strcmp(s.m->stop_message, "the host's network: receive broke") == 0);
	CHECK_EQ(event_queue_next(&s.m->events), EVENT_NEVER);
	teardown(&s);
}

/* Unplugged with a frame on the cable: it never comes. */
static void test_destroy(void)
{
	Setup s;

	setup(&s);
	queue(&s, 60, 1);
	poll(&s);
	CHECK(event_queue_next(&s.m->events) != EVENT_NEVER);
	net_cable_destroy(s.c);
	CHECK_EQ(event_queue_next(&s.m->events), EVENT_NEVER);
	CHECK_EQ(s.m->num_inputs, 0);
	net_cable_destroy(NULL);
	machine_destroy(s.m);
}

/* A machine that takes no more inputs. */
static void test_full(void)
{
	char error[100];
	Machine *m = machine_create();
	FakeLink link = {.link.ops = &fake_ops};
	FakePort fp = {0};
	NetCablePort port = {port_receive, port_speed, &fp};

	CHECK(m != NULL);
	for (unsigned i = 0; i < MACHINE_MAX_INPUTS; i++)
		CHECK(machine_add_input(m, NULL, NULL, &fp));
	CHECK(net_cable_create(m, &link.link, &port, error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "the machine takes no more inputs") == 0);
	machine_destroy(m);
}

int main(void)
{
	RUN_TEST(test_send);
	RUN_TEST(test_receive_paced);
	RUN_TEST(test_no_link);
	RUN_TEST(test_receive_longest);
	RUN_TEST(test_receive_fails);
	RUN_TEST(test_destroy);
	RUN_TEST(test_full);
	return test_summary();
}
