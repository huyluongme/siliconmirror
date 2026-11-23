/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The user network's DHCP server (RFC 2131, options from RFC 2132), at
 * the router, 10.0.2.2. The link has one guest, and every client on it
 * is given 10.0.2.15 for a day: a binding the server keeps for any
 * client, so that one that asks again after a restart (INIT-REBOOT) for
 * that address gets it, and one that asks for another is told no.
 * BOOTP clients (no DHCP message type) and relays (giaddr) are not
 * served: there is no relay on this link.
 */

#include "backend/net/user_int.h"

#include <string.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

/* The fixed part of a message (RFC 2131 section 2, figure 1). */
#define OP 0
#define HTYPE 1
#define HLEN 2
#define XID 4
#define FLAGS 10
#define CIADDR 12
#define YIADDR 16
#define GIADDR 24
#define CHADDR 28
#define COOKIE 236
#define OPTIONS 240

#define BOOTREQUEST 1
#define BOOTREPLY 2
#define FLAG_BROADCAST 0x8000
#define MAGIC_COOKIE 0x63825363 /* 99.130.83.99, RFC 2132 section 2 */

/* The DHCP message types, RFC 2132 section 9.6. */
#define DHCPDISCOVER 1
#define DHCPOFFER 2
#define DHCPREQUEST 3
#define DHCPACK 5
#define DHCPNAK 6
#define DHCPINFORM 8

/* The options used, RFC 2132. */
#define OPT_PAD 0         /* 3.1 */
#define OPT_SUBNET_MASK 1 /* 3.3 */
#define OPT_ROUTER 3      /* 3.5 */
#define OPT_DNS 6         /* 3.8 */
#define OPT_REQUESTED 50  /* 9.1 */
#define OPT_LEASE 51      /* 9.2 */
#define OPT_TYPE 53       /* 9.6 */
#define OPT_SERVER 54     /* 9.7 */
#define OPT_MESSAGE 56    /* 9.9 */
#define OPT_END 255       /* 3.2 */

/* The lease: a day, our choice (the client renews it at half). */
#define LEASE_SECONDS 86400

/*
 * A reply is 300 bytes, the size of a BOOTP message (RFC 951, a vendor
 * area of 64 bytes), which old clients take at least: the server's
 * options, 34 bytes at most, fit in those 64 with padding after them.
 */
#define REPLY 300

/* What the server reads of the client's options. */
typedef struct Request {
	uint8_t type; /* 0: none */
	bool has_requested, has_server;
	uint32_t requested, server;
} Request;

/*
 * The options field (RFC 2131 section 4.1), into r, which starts zeroed:
 * each option is its code, its length and its data, but pad and end,
 * which are one byte. The 'sname' and 'file' fields are not read: a
 * client puts options there only when they do not fit (option
 * overload), not the few the server reads.
 */
static void read_options(const uint8_t *msg, size_t len, Request *r)
{
	size_t i = OPTIONS;

	while (i < len && msg[i] != OPT_END) {
		uint8_t code = msg[i], size;

		if (code == OPT_PAD) {
			i++;
			continue;
		}
		if (i + 2 > len || i + 2 + msg[i + 1] > len)
			return; /* cut short: what came before counts */
		size = msg[i + 1];
		if (code == OPT_TYPE && size == 1)
			r->type = msg[i + 2];
		else if (code == OPT_REQUESTED && size == 4) {
			r->has_requested = true;
			r->requested = user_get32(msg + i + 2);
		} else if (code == OPT_SERVER && size == 4) {
			r->has_server = true;
			r->server = user_get32(msg + i + 2);
		}
		i += 2 + (size_t)size;
	}
}

static uint8_t *put_option(uint8_t *p, uint8_t code, uint8_t size)
{
	p[0] = code;
	p[1] = size;
	return p + 2;
}

static uint8_t *put_address(uint8_t *p, uint8_t code, uint32_t address)
{
	p = put_option(p, code, 4);
	user_put32(p, address);
	return p + 4;
}

/*
 * Send the client a reply of type: the fields of RFC 2131 section 4.3.1,
 * table 3, with the network's settings unless it is a DHCPNAK, and the
 * lease (with the client's 'ciaddr' in a DHCPACK) when an address is
 * given, yiaddr. It goes where section 4.1 says, for a message with no
 * relay: a DHCPNAK broadcast, the others to 'ciaddr' when the client has
 * one, broadcast when it asks for it, else to the address given and the
 * client's hardware address.
 */
static void reply(UserNet *u, const uint8_t eth_src[6], const uint8_t *msg,
                  uint8_t type, uint32_t yiaddr)
{
	static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff,
	                                         0xff, 0xff, 0xff};
	uint8_t *udp = u->datagram;
	uint8_t *out = udp + UDP_HEADER;
	uint8_t *p = out + OPTIONS;
	uint32_t ciaddr = user_get32(msg + CIADDR);
	uint32_t to = yiaddr;
	const uint8_t *mac = msg + CHADDR;
	static const char nak[] = "wrong address";

	memset(out, 0, REPLY);
	out[OP] = BOOTREPLY;
	out[HTYPE] = 1; /* Ethernet */
	out[HLEN] = 6;
	memcpy(out + XID, msg + XID, 4);
	memcpy(out + FLAGS, msg + FLAGS, 2);
	if (type == DHCPACK && yiaddr != 0)
		user_put32(out + CIADDR, ciaddr);
	user_put32(out + YIADDR, yiaddr);
	memcpy(out + CHADDR, msg + CHADDR, 16);
	user_put32(out + COOKIE, MAGIC_COOKIE);

	p = put_option(p, OPT_TYPE, 1);
	*p++ = type;
	p = put_address(p, OPT_SERVER, USER_GATEWAY);
	if (type == DHCPNAK) {
		p = put_option(p, OPT_MESSAGE, sizeof(nak) - 1);
		memcpy(p, nak, sizeof(nak) - 1);
		p += sizeof(nak) - 1;
	} else {
		if (yiaddr != 0) {
			p = put_option(p, OPT_LEASE, 4);
			user_put32(p, LEASE_SECONDS);
			p += 4;
		}
		p = put_address(p, OPT_SUBNET_MASK, USER_MASK);
		p = put_address(p, OPT_ROUTER, USER_GATEWAY);
		p = put_address(p, OPT_DNS, USER_DNS);
	}
	*p = OPT_END;

	if (ciaddr != 0 && type != DHCPNAK) {
		to = ciaddr;
		mac = eth_src;
	} else if (type == DHCPNAK || to == 0 ||
	           (user_get16(msg + FLAGS) & FLAG_BROADCAST) != 0) {
		to = USER_BROADCAST;
		mac = broadcast_mac;
	}
	user_udp_output(u, mac, USER_GATEWAY, DHCP_SERVER_PORT, to,
	                DHCP_CLIENT_PORT, udp, REPLY);
}

/*
 * RFC 2131 section 4.3.2: a request that answers the server's offer
 * (with its server identifier) or checks an address the client has
 * (with the address, after a restart; or in 'ciaddr', to renew).
 */
static void request(UserNet *u, const uint8_t eth_src[6], const uint8_t *msg,
                    const Request *r)
{
	uint32_t wanted;

	if (r->has_server && r->server != USER_GATEWAY)
		return; /* the client took another server's offer */
	if (r->has_server || r->has_requested)
		wanted = r->requested; /* 0 when there is none */
	else if (user_get32(msg + CIADDR) != 0)
		wanted = user_get32(msg + CIADDR);
	else
		return;
	if (wanted == USER_GUEST)
		reply(u, eth_src, msg, DHCPACK, USER_GUEST);
	else
		reply(u, eth_src, msg, DHCPNAK, 0);
}

void user_dhcp_input(UserNet *u, const uint8_t eth_src[6], const uint8_t *msg,
                     size_t len)
{
	Request r = {0};

	if (len < OPTIONS || msg[OP] != BOOTREQUEST || msg[HTYPE] != 1 ||
	    msg[HLEN] != 6 || user_get32(msg + GIADDR) != 0 ||
	    user_get32(msg + COOKIE) != MAGIC_COOKIE)
		return;
	read_options(msg, len, &r);
	switch (r.type) {
	case DHCPDISCOVER:
		reply(u, eth_src, msg, DHCPOFFER, USER_GUEST);
		break;
	case DHCPREQUEST:
		request(u, eth_src, msg, &r);
		break;
	case DHCPINFORM:
		/* Section 4.3.5: no address and no lease, to 'ciaddr'. */
		reply(u, eth_src, msg, DHCPACK, 0);
		break;
	default:
		/*
		 * DHCPDECLINE and DHCPRELEASE: the server has no other address to
		 * give, so 10.0.2.15 stays the one it offers. Others are not for
		 * a server.
		 */
		break;
	}
}
