/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The cable between a board's Ethernet port and the host. A frame's time
 * on the cable (IEEE 802.3 clause 4): 8 bytes of preamble and start
 * delimiter, the frame with its FCS, and the 96 bit times of the gap
 * before the next, at 10 or 100 Mb/s.
 */

#include "machine/net_cable.h"

#include <stdio.h>
#include <stdlib.h>

#include "core/event.h"

#define PREAMBLE_BYTES 8 /* 7 of preamble, the start frame delimiter */
#define GAP_BYTES 12     /* the interpacket gap, 96 bit times */

/* The most frames dropped at once while there is no link. */
#define MAX_DROPPED 64

struct NetCable {
	Machine *machine;
	NetLink *link;
	NetCablePort port;
	Event arrive;    /* the frame on the cable has come in */
	bool busy;       /* a frame is on the cable */
	size_t wire_len; /* its length, FCS included */
	uint8_t frame[NET_MAX_FRAME];
	uint8_t wire[NET_MAX_FRAME + NET_FCS_SIZE];
};

static uint64_t now(const NetCable *c)
{
	return event_queue_now(&c->machine->events);
}

static void link_failed(NetCable *c)
{
	machine_stop(c->machine, MACHINE_STOP_ERROR, "the host's network: %s",
	             c->link->error);
}

/*
 * Put the host's next frame on the cable, if the cable is free. With no
 * link, the partner's frames are lost, as on a real cable.
 */
static void take(NetCable *c)
{
	for (unsigned dropped = 0; !c->busy && dropped < MAX_DROPPED; dropped++) {
		size_t len;
		unsigned speed;

		if (!c->link->ops->receive(c->link, c->frame, &len)) {
			link_failed(c);
			return;
		}
		if (len == 0)
			return;
		speed = c->port.speed(c->port.ctx);
		if (speed == 0)
			continue;
		c->wire_len = net_to_wire(c->frame, len, c->wire);
		c->busy = true;
		event_schedule(
			&c->machine->events, &c->arrive,
			now(c) + (uint64_t)(PREAMBLE_BYTES + c->wire_len + GAP_BYTES) *
						 8000 / speed);
	}
}

static void arrive(void *ctx)
{
	NetCable *c = ctx;

	c->busy = false;
	c->port.receive(c->port.ctx, c->wire, c->wire_len);
	take(c);
}

static void poll_link(void *ctx)
{
	take(ctx);
}

/* The host's frames come at the polls; waiting for them never blocks. */
static bool wait_link(void *ctx)
{
	(void)ctx;
	return false;
}

NetCable *net_cable_create(Machine *m, NetLink *link, const NetCablePort *port,
                           char *error, size_t error_size)
{
	NetCable *c = calloc(1, sizeof(*c));

	if (c == NULL) {
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	c->machine = m;
	c->link = link;
	c->port = *port;
	event_init(&c->arrive, arrive, c);
	if (!machine_add_input(m, poll_link, wait_link, c)) {
		snprintf(error, error_size, "the machine takes no more inputs");
		free(c);
		return NULL;
	}
	return c;
}

void net_cable_destroy(NetCable *c)
{
	if (c == NULL)
		return;
	machine_remove_input(c->machine, c);
	event_cancel(&c->machine->events, &c->arrive);
	free(c);
}

void net_cable_send(void *ctx, const uint8_t *frame, size_t len)
{
	NetCable *c = ctx;
	size_t host_len = net_from_wire(frame, len);

	if (host_len == 0)
		return;
	if (!c->link->ops->send(c->link, now(c), frame, host_len))
		link_failed(c);
}
