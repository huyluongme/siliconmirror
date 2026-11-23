/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The parts of the user network (user.h): user.c has the link, Ethernet,
 * ARP, IPv4 and ICMP; user_dhcp.c the DHCP server; user_udp.c UDP and
 * its NAT through the host's sockets.
 */

#ifndef BACKEND_NET_USER_INT_H
#define BACKEND_NET_USER_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "backend/net/user.h"

/* The addresses, as numbers in host order. */
#define USER_NET 0x0a000200u     /* 10.0.2.0 */
#define USER_MASK 0xffffff00u    /* 255.255.255.0 */
#define USER_GATEWAY 0x0a000202u /* 10.0.2.2 */
#define USER_DNS 0x0a000203u     /* 10.0.2.3 */
#define USER_GUEST 0x0a00020fu   /* 10.0.2.15 */
#define USER_BROADCAST 0xffffffffu
#define USER_LOOPBACK 0x7f000001u /* 127.0.0.1, the host's own */

#define ETH_HEADER 14
#define IP_HEADER 20 /* without options, as the router sends them */
#define UDP_HEADER 8
#define USER_MTU 1500 /* Ethernet's (RFC 894) */

#define IP_ICMP 1
#define IP_TCP 6
#define IP_UDP 17

/*
 * The frames waiting for the guest. A UDP datagram of 65535 bytes is 45
 * fragments; the queue holds a few of them.
 */
#define USER_QUEUE 160
#define USER_MAX_FRAGMENTS 45

/* The UDP mappings at once; the least used goes when one more is needed. */
#define USER_UDP_FLOWS 64

/*
 * A UDP mapping (RFC 4787's "NAT mapping"): the guest's address and port
 * to one destination, through a socket of the host connected to it, so
 * that only that destination's answers come back (an "address and port
 * dependent" filter, section 5).
 */
typedef struct UserFlow {
	int fd; /* -1 when unused */
	uint32_t guest_ip;
	uint16_t guest_port;
	uint32_t ip; /* the destination, as the guest sees it */
	uint16_t port;
	uint64_t last; /* virtual time of the last datagram */
} UserFlow;

typedef struct UserNet {
	NetLink link; /* first, so that a NetLink * is a UserNet * */
	uint8_t guest_mac[6];
	uint16_t ip_id; /* the identification of the next datagram sent */
	uint64_t now;   /* the virtual time of the call being made */

	/* The frames for the guest: a ring. */
	uint8_t queue[USER_QUEUE][ETH_HEADER + USER_MTU];
	uint16_t queue_len[USER_QUEUE];
	unsigned head, count;

	/* The host's name server. */
	struct sockaddr_storage dns;
	socklen_t dns_len;

	UserFlow udp[USER_UDP_FLOWS];

	uint8_t datagram[65536]; /* a datagram from the host */
} UserNet;

/* The router's own Ethernet address (user.c). */
extern const uint8_t user_mac[6];

/*
 * The Internet checksum (RFC 791 section 3.1, RFC 768) of data, added to
 * sum, the sum of the words before it; and the sum of the pseudo header
 * of UDP and TCP.
 */
uint16_t user_checksum(uint32_t sum, const uint8_t *data, size_t len);
uint32_t user_pseudo_sum(uint32_t src, uint32_t dst, uint8_t protocol,
                         size_t len);

/* Free places in the queue for the guest. */
unsigned user_queue_free(const UserNet *u);

/*
 * Send the guest an IPv4 datagram of protocol from src to dst, of len
 * bytes, at least 1, in as many fragments as the MTU needs, in Ethernet
 * frames to eth_dst. When the queue has no room for it all, it is lost.
 */
void user_send_ip(UserNet *u, const uint8_t eth_dst[6], uint8_t protocol,
                  uint32_t src, uint32_t dst, const uint8_t *data, size_t len);

/* A UDP datagram from the guest, its IP header and data (user_udp.c). */
void user_udp_input(UserNet *u, const uint8_t eth_src[6], uint32_t src,
                    uint32_t dst, const uint8_t *data, size_t len);

/*
 * Send the guest a UDP datagram of len bytes of data, which udp holds
 * after UDP_HEADER bytes for the header that this makes.
 */
void user_udp_output(UserNet *u, const uint8_t eth_dst[6], uint32_t src,
                     uint16_t sport, uint32_t dst, uint16_t dport, uint8_t *udp,
                     size_t len);

/* Take what the host's sockets have for the guest; close idle mappings. */
void user_udp_poll(UserNet *u);

/* No mappings yet, and the host's name server from config. */
void user_udp_init(UserNet *u, const NetUserConfig *config);
void user_udp_close(UserNet *u);

/* A DHCP message from the guest, the data of its UDP datagram. */
void user_dhcp_input(UserNet *u, const uint8_t eth_src[6], const uint8_t *msg,
                     size_t len);

/* Big-endian access to the frames' fields. */
static inline uint16_t user_get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t user_get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
	       p[3];
}

static inline void user_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void user_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

#endif /* BACKEND_NET_USER_INT_H */
