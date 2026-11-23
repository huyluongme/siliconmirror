/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The user network's router (user.h): Ethernet (RFC 894), ARP (RFC 826),
 * IPv4 (RFC 791) and ICMP (RFC 792). The router answers ARP and ping for
 * its two addresses, 10.0.2.2 and 10.0.2.3, with one Ethernet address,
 * as one box with two addresses would; the guest's Ethernet address is
 * the source of its last frame to the router.
 */

#include "backend/net/user_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_ARP 0x0806

/*
 * The router's Ethernet address: locally administered (the second bit of
 * the first octet, IEEE 802 clause 8.2), unicast, then "SM" for smirror
 * and the router's address, 0.2.2. Our choice.
 */
const uint8_t user_mac[6] = {0x02, 0x53, 0x4d, 0x00, 0x02, 0x02};

static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* RFC 791 section 3.1: the time to live of the router's datagrams. */
#define TTL 64

/* The most data in a fragment: the MTU less the header, in 8-byte units. */
#define FRAGMENT_DATA ((USER_MTU - IP_HEADER) & ~7u)

uint16_t user_checksum(uint32_t sum, const uint8_t *data, size_t len)
{
	size_t i;

	for (i = 0; i + 1 < len; i += 2)
		sum += user_get16(data + i);
	if (i < len)
		sum += (uint32_t)data[i] << 8; /* the odd byte, padded with 0 */
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

uint32_t user_pseudo_sum(uint32_t src, uint32_t dst, uint8_t protocol,
                         size_t len)
{
	return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) +
	       protocol + (uint32_t)len;
}

unsigned user_queue_free(const UserNet *u)
{
	return USER_QUEUE - u->count;
}

/* The next free frame of the queue, of len bytes, to fill. */
static uint8_t *queue_push(UserNet *u, size_t len)
{
	unsigned slot = (u->head + u->count) % USER_QUEUE;

	u->queue_len[slot] = (uint16_t)len;
	u->count++;
	return u->queue[slot];
}

static void put_eth(uint8_t *frame, const uint8_t dst[6], uint16_t type)
{
	memcpy(frame, dst, 6);
	memcpy(frame + 6, user_mac, 6);
	user_put16(frame + 12, type);
}

void user_send_ip(UserNet *u, const uint8_t eth_dst[6], uint8_t protocol,
                  uint32_t src, uint32_t dst, const uint8_t *data, size_t len)
{
	size_t fragments = (len + FRAGMENT_DATA - 1) / FRAGMENT_DATA;
	uint16_t id = u->ip_id++;
	size_t offset = 0;

	if (fragments > user_queue_free(u))
		return;
	for (size_t i = 0; i < fragments; i++) {
		size_t part =
			len - offset < FRAGMENT_DATA ? len - offset : FRAGMENT_DATA;
		uint8_t *frame = queue_push(u, ETH_HEADER + IP_HEADER + part);
		uint8_t *ip = frame + ETH_HEADER;
		uint16_t flags = (uint16_t)(offset / 8);

		if (offset + part < len)
			flags |= 0x2000; /* more fragments */
		put_eth(frame, eth_dst, ETHERTYPE_IP);
		ip[0] = 0x45; /* version 4, 5 words of header */
		ip[1] = 0;    /* type of service */
		user_put16(ip + 2, (uint16_t)(IP_HEADER + part));
		user_put16(ip + 4, id);
		user_put16(ip + 6, flags);
		ip[8] = TTL;
		ip[9] = protocol;
		user_put16(ip + 10, 0);
		user_put32(ip + 12, src);
		user_put32(ip + 16, dst);
		user_put16(ip + 10, user_checksum(0, ip, IP_HEADER));
		memcpy(ip + IP_HEADER, data + offset, part);
		offset += part;
	}
}

/* RFC 826, "Packet Reception": answer who has 10.0.2.2 or 10.0.2.3. */
static void arp_input(UserNet *u, const uint8_t *arp, size_t len)
{
	uint8_t *reply;
	uint32_t target;

	if (len < 28 || user_get16(arp) != 1 /* Ethernet */ ||
	    user_get16(arp + 2) != ETHERTYPE_IP || arp[4] != 6 || arp[5] != 4 ||
	    user_get16(arp + 6) != 1 /* request */)
		return;
	target = user_get32(arp + 24);
	if (target != USER_GATEWAY && target != USER_DNS)
		return;
	if (user_queue_free(u) == 0)
		return;
	reply = queue_push(u, ETH_HEADER + 28);
	put_eth(reply, arp + 8, ETHERTYPE_ARP);
	memcpy(reply + ETH_HEADER, arp, 6);    /* hardware and protocol */
	user_put16(reply + ETH_HEADER + 6, 2); /* reply */
	memcpy(reply + ETH_HEADER + 8, user_mac, 6);
	memcpy(reply + ETH_HEADER + 14, arp + 24, 4);
	memcpy(reply + ETH_HEADER + 18, arp + 8, 10); /* the asker's */
}

/* An ICMP message to the router: it answers an echo request (ping). */
static void icmp_input(UserNet *u, const uint8_t eth_src[6], uint32_t src,
                       uint32_t dst, const uint8_t *data, size_t len)
{
	uint8_t reply[USER_MTU - IP_HEADER];

	/* A larger echo comes in fragments, which are dropped. */
	if (len < 8 || len > sizeof(reply) || user_checksum(0, data, len) != 0)
		return;
	if (data[0] != 8 || data[1] != 0) /* echo request, RFC 792 page 14 */
		return;
	memcpy(reply, data, len);
	reply[0] = 0; /* echo reply */
	user_put16(reply + 2, 0);
	user_put16(reply + 2, user_checksum(0, reply, len));
	user_send_ip(u, eth_src, IP_ICMP, dst, src, reply, len);
}

/*
 * RFC 792 "Time Exceeded Message": a datagram the router would pass on
 * with a time to live of 1 or less is dropped, and the sender told, with
 * its header and the first 8 bytes of its data (what traceroute's first
 * hop needs).
 */
static void time_exceeded(UserNet *u, const uint8_t eth_src[6],
                          const uint8_t *ip, size_t header, size_t len)
{
	/* Type 11, time exceeded; code 0, in transit; 4 unused bytes. */
	uint8_t msg[8 + 60 + 8] = {11, 0};
	size_t quoted = header + (len - header < 8 ? len - header : 8);

	memcpy(msg + 8, ip, quoted);
	user_put16(msg + 2, user_checksum(0, msg, 8 + quoted));
	user_send_ip(u, eth_src, IP_ICMP, USER_GATEWAY, user_get32(ip + 12), msg,
	             8 + quoted);
}

/*
 * Where a datagram to dst goes: to the router, broadcast on the link, to
 * nobody (another address of 10.0.2.0/24, or one no host may send to,
 * RFC 1122 section 3.2.1.3), or out.
 */
typedef enum { TO_ROUTER, TO_BROADCAST, TO_NOBODY, TO_OUTSIDE } Route;

static Route route(uint32_t dst)
{
	uint8_t first = (uint8_t)(dst >> 24);

	if (dst == USER_GATEWAY || dst == USER_DNS)
		return TO_ROUTER;
	if (dst == USER_BROADCAST || dst == (USER_NET | ~USER_MASK))
		return TO_BROADCAST;
	if ((dst & USER_MASK) == USER_NET)
		return TO_NOBODY;
	/* 0.0.0.0/8, 127.0.0.0/8, and multicast and above (224 to 255). */
	if (first == 0 || first == 127 || first >= 224)
		return TO_NOBODY;
	return TO_OUTSIDE;
}

/* RFC 791 section 3.1, and RFC 1122 section 3.2.1 for what is dropped. */
static void ip_input(UserNet *u, const uint8_t eth_src[6], const uint8_t *ip,
                     size_t len)
{
	size_t header, total;
	uint32_t src, dst;
	Route to;

	if (len < IP_HEADER || ip[0] >> 4 != 4)
		return;
	header = (size_t)(ip[0] & 0xf) * 4;
	total = user_get16(ip + 2);
	if (header < IP_HEADER || total < header || total > len ||
	    user_checksum(0, ip, header) != 0)
		return;
	/* Fragments, the first or the others, are not put together. */
	if ((user_get16(ip + 6) & 0x3fff) != 0)
		return;
	src = user_get32(ip + 12);
	dst = user_get32(ip + 16);
	to = route(dst);
	if (to == TO_OUTSIDE && ip[9] == IP_UDP && ip[8] <= 1)
		time_exceeded(u, eth_src, ip, header, total);
	else if (to == TO_ROUTER && ip[9] == IP_ICMP)
		icmp_input(u, eth_src, src, dst, ip + header, total - header);
	else if (to != TO_NOBODY && ip[9] == IP_UDP)
		user_udp_input(u, eth_src, src, dst, ip + header, total - header);
}

static bool user_send(NetLink *n, uint64_t now, const uint8_t *frame,
                      size_t len)
{
	UserNet *u = (UserNet *)n;
	uint16_t type;

	u->now = now;
	if (len < ETH_HEADER)
		return true;
	if (memcmp(frame, user_mac, 6) != 0 && memcmp(frame, broadcast_mac, 6) != 0)
		return true;
	type = user_get16(frame + 12);
	/* A unicast source is the guest's address (IEEE 802 clause 8.2). */
	if ((frame[6] & 1) == 0)
		memcpy(u->guest_mac, frame + 6, 6);
	if (type == ETHERTYPE_ARP)
		arp_input(u, frame + ETH_HEADER, len - ETH_HEADER);
	else if (type == ETHERTYPE_IP)
		ip_input(u, frame + 6, frame + ETH_HEADER, len - ETH_HEADER);
	return true;
}

static bool user_receive(NetLink *n, uint64_t now, uint8_t *frame, size_t *len)
{
	UserNet *u = (UserNet *)n;

	u->now = now;
	user_udp_poll(u);
	*len = 0;
	if (u->count == 0)
		return true;
	*len = u->queue_len[u->head];
	memcpy(frame, u->queue[u->head], *len);
	u->head = (u->head + 1) % USER_QUEUE;
	u->count--;
	return true;
}

static void user_destroy(NetLink *n)
{
	UserNet *u = (UserNet *)n;

	user_udp_close(u);
	free(u);
}

static const NetLinkOps user_ops = {
	.send = user_send,
	.receive = user_receive,
	.destroy = user_destroy,
};

NetLink *net_user_open_config(const NetUserConfig *config, char *error,
                              size_t error_size)
{
	UserNet *u = calloc(1, sizeof(*u));

	if (u == NULL) {
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	u->link.ops = &user_ops;
	user_udp_init(u, config);
	return &u->link;
}

NetLink *net_user_open(const char *options, char *error, size_t error_size)
{
	static const NetUserConfig config = {"/etc/resolv.conf", 53};

	if (options[0] != '\0') {
		snprintf(error, error_size, "user: unknown option '%s'", options);
		return NULL;
	}
	return net_user_open_config(&config, error, error_size);
}
