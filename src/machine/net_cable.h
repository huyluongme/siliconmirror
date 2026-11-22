/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The cable between a board's Ethernet port and the host's side of the
 * link (core/net.h): the frames the port sends reach the host, and the
 * host's frames reach the port one at a time, each when its last bit
 * would have crossed the cable at the link's speed.
 *
 * The port's MAC sends and takes frames as they are on the cable, FCS
 * included; the partner's MAC, the host's side, drops what the port
 * sends shorter than 64 bytes or with a wrong FCS, as IEEE 802.3 clause
 * 4 has a MAC do, and pads and checksums what the host sends.
 */

#ifndef MACHINE_NET_CABLE_H
#define MACHINE_NET_CABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/machine.h"
#include "core/net.h"

/* The board's port, as the cable sees it. */
typedef struct NetCablePort {
	/* A frame from the cable, FCS included; false when the MAC drops it. */
	bool (*receive)(void *ctx, const uint8_t *frame, size_t len);
	/* The link's speed in Mb/s; 0 when there is no link. */
	unsigned (*speed)(void *ctx);
	void *ctx;
} NetCablePort;

typedef struct NetCable NetCable;

/*
 * Plug link into port on m: the cable takes the host's frames from then
 * on. link stays the caller's, and outlives the cable. NULL with a
 * message in error when memory runs out, or m takes no more inputs.
 */
NetCable *net_cable_create(Machine *m, NetLink *link, const NetCablePort *port,
                           char *error, size_t error_size);

/* Unplug and free the cable. */
void net_cable_destroy(NetCable *c);

/*
 * A frame that the port sends, FCS included; ctx is the cable. Its type
 * is that of a device's send callback, such as Lan95xxSend.
 */
void net_cable_send(void *ctx, const uint8_t *frame, size_t len);

#endif /* MACHINE_NET_CABLE_H */
