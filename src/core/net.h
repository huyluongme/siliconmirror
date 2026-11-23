/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Network links: the interface between a device with an Ethernet port,
 * such as the LAN9514 behind the raspi3b's RJ45 jack, and the host side
 * of its cable (src/backend/net/): a TAP device, or a capture file.
 *
 * Frames cross the interface as a host's network stack and pcap files
 * carry them: from the destination address to the end of the data, with
 * no preamble and no FCS, and not padded to the 60 bytes that a frame
 * has at least on the cable. net_to_wire() and net_from_wire() turn
 * them into the frames on the cable that a device's MAC sends and
 * takes, and back.
 *
 * A call fails only when the host does; the back end then writes why in
 * error, and the board stops the machine.
 */

#ifndef CORE_NET_H
#define CORE_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The longest frame a back end passes, without its FCS: the largest MTU
 * that Linux allows, 65535, and the Ethernet header.
 */
#define NET_MAX_FRAME (65535 + 14)

/* A frame on the cable has at least 60 bytes and its 4-byte FCS. */
#define NET_MIN_WIRE 64
#define NET_FCS_SIZE 4

typedef struct NetLink NetLink;

typedef struct NetLinkOps {
	/*
	 * The device sent a frame, at virtual time now, in nanoseconds from
	 * power on (a capture keeps it). A frame the host has no room for is
	 * lost, as on a busy link; that is no failure.
	 */
	bool (*send)(NetLink *n, uint64_t now, const uint8_t *frame, size_t len);

	/*
	 * The next frame the host has for the device, into frame, which holds
	 * NET_MAX_FRAME bytes, and its length in len: 0 when there is none
	 * yet. It never waits. The board takes one at a time, at the pace of
	 * the cable; the others wait on the host, as at the partner's port.
	 * now is the virtual time, as for send: a back end that keeps timers,
	 * such as the user network's, counts them in it.
	 */
	bool (*receive)(NetLink *n, uint64_t now, uint8_t *frame, size_t *len);

	void (*destroy)(NetLink *n);
} NetLinkOps;

/* Each back end embeds a NetLink in its own state struct. */
struct NetLink {
	const NetLinkOps *ops;
	char error[160]; /* why the last call failed */
};

static inline void net_destroy(NetLink *n)
{
	if (n != NULL)
		n->ops->destroy(n);
}

/*
 * The frame on the cable for a frame from the host, into wire: padded
 * with zeros to 60 bytes, then its FCS. wire holds at least the larger
 * of len and 60, plus NET_FCS_SIZE; returns the length written.
 */
size_t net_to_wire(const uint8_t *frame, size_t len, uint8_t *wire);

/*
 * The length of the frame for the host in a frame from the cable: all
 * but its FCS. 0 when the partner's MAC drops it: shorter than 64 bytes,
 * or with a wrong FCS.
 */
size_t net_from_wire(const uint8_t *wire, size_t len);

#endif /* CORE_NET_H */
