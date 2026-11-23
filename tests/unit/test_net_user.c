/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The user network (backend/net/user.h): the frames the guest sends, and
 * what the router answers, checked byte by byte with a checksum of the
 * test's own; UDP through sockets of the host on its loopback.
 */

#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "backend/net/user.h"
#include "test.h"

#define GATEWAY 0x0a000202u
#define DNS 0x0a000203u
#define GUEST 0x0a00020fu
#define OUTSIDE 0xc0000201u /* 192.0.2.1, TEST-NET-1 (RFC 5737) */

#define SECOND 1000000000ull
#define RESOLV "test_net_user.resolv"

static const uint8_t router_mac[6] = {0x02, 0x53, 0x4d, 0x00, 0x02, 0x02};
static const uint8_t bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t guest_mac[6] = {0xb8, 0x27, 0xeb, 0x11, 0x22, 0x33};

static uint8_t frame[NET_MAX_FRAME], got[NET_MAX_FRAME];
static uint64_t now;

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)get16(p) << 16 | get16(p + 2);
}

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v >> 16);
	put16(p + 2, v);
}

/* The one's complement sum, folded; data with a right checksum sums to 0xffff.
 */
static uint32_t sum(uint32_t s, const uint8_t *p, size_t len)
{
	for (size_t i = 0; i < len; i++)
		s += i % 2 == 0 ? (uint32_t)p[i] << 8 : p[i];
	while (s > 0xffff)
		s = (s >> 16) + (s & 0xffff);
	return s;
}

static uint32_t pseudo(uint32_t src, uint32_t dst, uint8_t proto, size_t len)
{
	uint8_t p[12];

	put32(p, src);
	put32(p + 4, dst);
	p[8] = 0;
	p[9] = proto;
	put16(p + 10, (uint32_t)len);
	return sum(0, p, 12);
}

/* An Ethernet frame with an IPv4 header of 5 words, its checksum right. */
static size_t ip_frame(uint8_t *f, const uint8_t dst_mac[6], uint8_t proto,
                       uint32_t src, uint32_t dst, uint8_t ttl,
                       const uint8_t *data, size_t len)
{
	uint8_t *ip = f + 14;

	memcpy(f, dst_mac, 6);
	memcpy(f + 6, guest_mac, 6);
	put16(f + 12, 0x0800);
	memset(ip, 0, 20);
	ip[0] = 0x45;
	put16(ip + 2, (uint32_t)(20 + len));
	put16(ip + 4, 0x1234);
	put16(ip + 6, 0x4000); /* don't fragment, as Linux sends */
	ip[8] = ttl;
	ip[9] = proto;
	put32(ip + 12, src);
	put32(ip + 16, dst);
	put16(ip + 10, ~sum(0, ip, 20));
	memcpy(ip + 20, data, len);
	return 14 + 20 + len;
}

static size_t udp_frame(uint8_t *f, const uint8_t dst_mac[6], uint32_t src,
                        uint16_t sport, uint32_t dst, uint16_t dport,
                        const uint8_t *data, size_t len)
{
	static uint8_t udp[65536];

	put16(udp, sport);
	put16(udp + 2, dport);
	put16(udp + 4, (uint32_t)(8 + len));
	put16(udp + 6, 0);
	memcpy(udp + 8, data, len);
	put16(udp + 6, ~sum(pseudo(src, dst, 17, 8 + len), udp, 8 + len));
	return ip_frame(f, dst_mac, 17, src, dst, 64, udp, 8 + len);
}

static size_t icmp_frame(uint8_t *f, uint32_t dst, uint8_t type,
                         const uint8_t *data, size_t len)
{
	uint8_t icmp[1600];

	icmp[0] = type;
	icmp[1] = 0;
	put16(icmp + 2, 0);
	put16(icmp + 4, 0x0042); /* identifier */
	put16(icmp + 6, 7);      /* sequence */
	memcpy(icmp + 8, data, len);
	put16(icmp + 2, ~sum(0, icmp, 8 + len));
	return ip_frame(f, router_mac, 1, GUEST, dst, 64, icmp, 8 + len);
}

static NetLink *open_user(uint16_t dns_port)
{
	NetUserConfig config = {RESOLV, dns_port};
	char error[200];
	NetLink *n = net_user_open_config(&config, error, sizeof(error));

	CHECK(n != NULL);
	return n;
}

static void send_frame(NetLink *n, size_t len)
{
	CHECK(n->ops->send(n, now, frame, len));
}

/* The next frame for the guest; 0 when there is none. */
static size_t take(NetLink *n)
{
	size_t len = 12345;

	CHECK(n->ops->receive(n, now, got, &len));
	return len;
}

/* The next frame, and no other after it. */
static size_t take1(NetLink *n)
{
	static uint8_t more[NET_MAX_FRAME];
	size_t len = take(n), extra = 1;

	CHECK(n->ops->receive(n, now, more, &extra));
	CHECK_EQ(extra, 0);
	return len;
}

/* A frame from the router, IPv4 with a right header: its header length. */
static void check_ip(size_t len, const uint8_t dst_mac[6], uint8_t proto,
                     uint32_t src, uint32_t dst)
{
	const uint8_t *ip = got + 14;

	CHECK(len >= 34);
	CHECK(memcmp(got, dst_mac, 6) == 0);
	CHECK(memcmp(got + 6, router_mac, 6) == 0);
	CHECK_EQ(get16(got + 12), 0x0800);
	CHECK_EQ(ip[0], 0x45);
	CHECK_EQ(ip[1], 0);
	CHECK_EQ(get16(ip + 2), len - 14);
	CHECK_EQ(ip[8], 64);
	CHECK_EQ(ip[9], proto);
	CHECK_EQ(get32(ip + 12), src);
	CHECK_EQ(get32(ip + 16), dst);
	CHECK_EQ(sum(0, ip, 20), 0xffff);
}

/* A UDP datagram from the router in got, whole: its data length. */
static size_t check_udp(size_t len, const uint8_t dst_mac[6], uint32_t src,
                        uint16_t sport, uint32_t dst, uint16_t dport)
{
	const uint8_t *udp = got + 34;
	size_t ulen;

	check_ip(len, dst_mac, 17, src, dst);
	CHECK_EQ(get16(got + 14 + 6), 0); /* not a fragment */
	ulen = get16(udp + 4);
	CHECK_EQ(ulen, len - 34);
	CHECK_EQ(get16(udp), sport);
	CHECK_EQ(get16(udp + 2), dport);
	CHECK(get16(udp + 6) != 0);
	CHECK_EQ(sum(pseudo(src, dst, 17, ulen), udp, ulen), 0xffff);
	return ulen - 8;
}

static void test_open(void)
{
	char error[200];
	NetLink *n = net_user_open("", error, sizeof(error));

	CHECK(n != NULL);
	net_destroy(n);
	CHECK(net_user_open("hostfwd", error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "user: unknown option 'hostfwd'") == 0);
}

static size_t arp_frame(uint8_t *f, const uint8_t dst_mac[6], uint16_t op,
                        uint32_t target)
{
	memcpy(f, dst_mac, 6);
	memcpy(f + 6, guest_mac, 6);
	put16(f + 12, 0x0806);
	put16(f + 14, 1);
	put16(f + 16, 0x0800);
	f[18] = 6;
	f[19] = 4;
	put16(f + 20, op);
	memcpy(f + 22, guest_mac, 6);
	put32(f + 28, GUEST);
	memset(f + 32, 0, 6);
	put32(f + 38, target);
	memset(f + 42, 0, 18); /* Ethernet's padding to 60 */
	return 60;
}

static void test_arp(void)
{
	NetLink *n = open_user(53);
	static const uint32_t ours[2] = {GATEWAY, DNS};

	CHECK_EQ(take(n), 0);
	for (int i = 0; i < 2; i++) {
		send_frame(n, arp_frame(frame, bcast_mac, 1, ours[i]));
		CHECK_EQ(take(n), 42);
		CHECK(memcmp(got, guest_mac, 6) == 0);
		CHECK(memcmp(got + 6, router_mac, 6) == 0);
		CHECK_EQ(get16(got + 12), 0x0806);
		CHECK_EQ(get16(got + 14), 1);
		CHECK_EQ(get16(got + 16), 0x0800);
		CHECK_EQ(got[18], 6);
		CHECK_EQ(got[19], 4);
		CHECK_EQ(get16(got + 20), 2);
		CHECK(memcmp(got + 22, router_mac, 6) == 0);
		CHECK_EQ(get32(got + 28), ours[i]);
		CHECK(memcmp(got + 32, guest_mac, 6) == 0);
		CHECK_EQ(get32(got + 38), GUEST);
		CHECK_EQ(take(n), 0);
	}
	/* Sent to the router itself, as when Linux checks its entry. */
	send_frame(n, arp_frame(frame, router_mac, 1, GATEWAY));
	CHECK_EQ(take(n), 42);

	/* Nobody else answers: the guest's own address, others, a reply. */
	send_frame(n, arp_frame(frame, bcast_mac, 1, GUEST));
	send_frame(n, arp_frame(frame, bcast_mac, 1, 0x0a000204));
	send_frame(n, arp_frame(frame, bcast_mac, 1, 0x0a000201));
	send_frame(n, arp_frame(frame, bcast_mac, 2, GATEWAY));
	CHECK_EQ(take(n), 0);

	/* Not for the router, not Ethernet and IPv4, short. */
	static const uint8_t other_mac[6] = {0x02, 0x53, 0x4d, 0x00, 0x02, 0x03};
	static const uint8_t near_bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xfe};
	static const int bad[] = {14, 16, 18, 19};
	send_frame(n, arp_frame(frame, other_mac, 1, GATEWAY));
	send_frame(n, arp_frame(frame, near_bcast, 1, GATEWAY));
	CHECK_EQ(take(n), 0);
	/* The answer goes to the sender's hardware address in the message. */
	arp_frame(frame, bcast_mac, 1, GATEWAY);
	frame[27] = 0x99;
	send_frame(n, 60);
	CHECK_EQ(take(n), 42);
	CHECK_EQ(got[5], 0x99);
	CHECK_EQ(got[37], 0x99);
	for (int i = 0; i < 4; i++) {
		arp_frame(frame, bcast_mac, 1, GATEWAY);
		frame[bad[i] + (bad[i] < 18)]++; /* the low byte of each field */
		send_frame(n, 60);
		CHECK_EQ(take(n), 0);
	}
	arp_frame(frame, bcast_mac, 1, GATEWAY);
	send_frame(n, 14 + 27);
	CHECK_EQ(take(n), 0);
	send_frame(n, 14 + 28);
	CHECK_EQ(take(n), 42);
	/* No frame shorter than a header, nor another type. */
	send_frame(n, 13);
	put16(frame + 12, 0x86dd);
	send_frame(n, 60);
	CHECK_EQ(take(n), 0);
	net_destroy(n);
}

static void test_queue_full(void)
{
	NetLink *n = open_user(53);
	uint8_t data[8] = {0};
	unsigned count = 0;

	for (int i = 0; i < 159; i++)
		send_frame(n, arp_frame(frame, bcast_mac, 1, GATEWAY));
	/* The last place takes a ping's answer; then no room: lost. */
	send_frame(n, icmp_frame(frame, GATEWAY, 8, data, sizeof(data)));
	send_frame(n, arp_frame(frame, bcast_mac, 1, GATEWAY));
	send_frame(n, icmp_frame(frame, GATEWAY, 8, data, sizeof(data)));
	while (count < 200 && take(n) == 42)
		count++;
	CHECK_EQ(count, 159);
	CHECK_EQ(got[34], 0); /* the echo reply */
	CHECK_EQ(take(n), 0);
	/* A place that held an ARP reply holds a datagram right. */
	send_frame(n, icmp_frame(frame, GATEWAY, 8, data, sizeof(data)));
	check_ip(take(n), guest_mac, 1, GATEWAY, GUEST);
	net_destroy(n);
}

static void test_ping(void)
{
	NetLink *n = open_user(53);
	uint8_t data[57];
	size_t len;

	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(0xa0 + i);
	/* An odd length, so the last byte's checksum counts. */
	send_frame(n, icmp_frame(frame, GATEWAY, 8, data, sizeof(data)));
	len = take(n);
	CHECK_EQ(len, 14 + 20 + 8 + sizeof(data));
	check_ip(len, guest_mac, 1, GATEWAY, GUEST);
	CHECK_EQ(get16(got + 20), 0); /* not a fragment, may be */
	CHECK_EQ(got[34], 0);         /* echo reply */
	CHECK_EQ(got[35], 0);
	CHECK_EQ(sum(0, got + 34, len - 34), 0xffff);
	CHECK_EQ(get16(got + 38), 0x0042);
	CHECK_EQ(get16(got + 40), 7);
	CHECK(memcmp(got + 42, data, sizeof(data)) == 0);
	uint16_t id = get16(got + 18);

	/* The name server's address answers too; each datagram its own id. */
	send_frame(n, icmp_frame(frame, DNS, 8, data, 1));
	len = take(n);
	check_ip(len, guest_mac, 1, DNS, GUEST);
	CHECK_EQ(get16(got + 18), (uint16_t)(id + 1));
	CHECK_EQ(sum(0, got + 34, len - 34), 0xffff);

	/* Not an echo request; a wrong checksum; elsewhere. */
	send_frame(n, icmp_frame(frame, GATEWAY, 0, data, 8));
	icmp_frame(frame, GATEWAY, 8, data, 8);
	frame[35] = 1; /* a code */
	send_frame(n, 14 + 20 + 16);
	icmp_frame(frame, GATEWAY, 8, data, 8);
	frame[45]++;
	send_frame(n, 14 + 20 + 16);
	send_frame(n, icmp_frame(frame, OUTSIDE, 8, data, 8));
	send_frame(n, icmp_frame(frame, GUEST, 8, data, 8));
	send_frame(n, icmp_frame(frame, 0x0a0002ff, 8, data, 8));
	send_frame(n, icmp_frame(frame, GATEWAY, 8, data, 0) - 1); /* short */
	CHECK_EQ(take(n), 0);

	/* The largest echo that fits the MTU. */
	static uint8_t big[1600];
	send_frame(n, icmp_frame(frame, GATEWAY, 8, big, 1472));
	CHECK_EQ(take(n), 1514);
	net_destroy(n);
}

static void test_ip_header(void)
{
	NetLink *n = open_user(53);
	uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	size_t len = icmp_frame(frame, GATEWAY, 8, data, sizeof(data));
	uint8_t *ip = frame + 14;

	/* Ethernet's padding after the datagram is not part of it. */
	memset(frame + len, 0x55, 10);
	send_frame(n, len + 10);
	CHECK_EQ(take(n), len);

	/* A header with options: 6 words. */
	memmove(ip + 24, ip + 20, 16);
	memset(ip + 20, 1, 4); /* no-operation options */
	ip[0] = 0x46;
	put16(ip + 2, 40);
	put16(ip + 10, 0);
	put16(ip + 10, ~sum(0, ip, 24));
	send_frame(n, 14 + 40);
	CHECK_EQ(take(n), len);
	CHECK_EQ(memcmp(got + 42, data, 8), 0);

	/* What is dropped: each a copy of the good one, changed. */
	for (int i = 0; i < 9; i++) {
		size_t flen = icmp_frame(frame, GATEWAY, 8, data, sizeof(data));

		switch (i) {
		case 0:
			ip[0] = 0x65;
			break; /* IPv6 */
		case 1:
			ip[0] = 0x44;
			break; /* 4 words */
		case 2:
			put16(ip + 2, 37);
			break; /* longer than the frame */
		case 3:
			put16(ip + 2, 19);
			break; /* shorter than its header */
		case 4:
			ip[10]++;
			break; /* the header's checksum */
		case 5:
			put16(ip + 6, 0x2000);
			break; /* more fragments */
		case 6:
			put16(ip + 6, 0x0001);
			break; /* a later fragment */
		case 7:
			flen = 14 + 19;
			break; /* too short for a header */
		case 8:
			ip[0] = 0x4f;
			break; /* longer than the datagram */
		}
		if (i != 4 && i != 7) {
			put16(ip + 10, 0);
			put16(ip + 10, ~sum(0, ip, i == 1 ? 16 : 20));
		}
		send_frame(n, flen);
		CHECK_EQ(take(n), 0);
	}
	/* An ICMP message shorter than an echo's header, its checksum right. */
	icmp_frame(frame, GATEWAY, 8, data, 0);
	put16(ip + 2, 27);
	put16(ip + 10, 0);
	put16(ip + 10, ~sum(0, ip, 20));
	memset(ip + 20, 0, 7);
	ip[20] = 8;
	put16(ip + 22, 0xf7ff); /* 0x0800 + 0xf7ff: all ones */
	send_frame(n, 14 + 27);
	CHECK_EQ(take(n), 0);
	/* An echo with no data is answered. */
	send_frame(n, icmp_frame(frame, GATEWAY, 8, data, 0));
	CHECK_EQ(take(n), 42);
	/* One larger than the MTU, from a guest whose MTU is larger: not. */
	static uint8_t big[1500];
	send_frame(n, icmp_frame(frame, GATEWAY, 8, big, 1473));
	CHECK_EQ(take(n), 0);
	net_destroy(n);
}

static void test_time_exceeded(void)
{
	NetLink *n = open_user(53);
	uint8_t data[20] = {9, 9, 9, 9};
	size_t len;

	for (int ttl = 0; ttl <= 1; ttl++) {
		udp_frame(frame, router_mac, GUEST, 33434, OUTSIDE, 33435, data,
		          sizeof(data));
		frame[14 + 8] = (uint8_t)ttl;
		put16(frame + 14 + 10, 0);
		put16(frame + 14 + 10, ~sum(0, frame + 14, 20));
		send_frame(n, 14 + 20 + 8 + sizeof(data));
		len = take(n);
		CHECK_EQ(len, 14 + 20 + 8 + 20 + 8);
		check_ip(len, guest_mac, 1, GATEWAY, GUEST);
		CHECK_EQ(got[34], 11);
		CHECK_EQ(got[35], 0);
		CHECK_EQ(get32(got + 38), 0);
		CHECK_EQ(sum(0, got + 34, len - 34), 0xffff);
		CHECK(memcmp(got + 42, frame + 14, 28) == 0);
	}
	/* A datagram shorter than 8 bytes of data is quoted whole. */
	udp_frame(frame, router_mac, GUEST, 1, OUTSIDE, 2, data, 0);
	frame[14 + 8] = 1;
	put16(frame + 14 + 2, 27); /* a UDP header cut to 7 bytes */
	put16(frame + 14 + 10, 0);
	put16(frame + 14 + 10, ~sum(0, frame + 14, 20));
	send_frame(n, 14 + 27);
	len = take(n);
	CHECK_EQ(len, 14 + 20 + 8 + 27);
	CHECK_EQ(sum(0, got + 34, len - 34), 0xffff);
	/* A header of 15 words (options) and its 8 bytes. */
	udp_frame(frame, router_mac, GUEST, 1, OUTSIDE, 2, data, 0);
	memmove(frame + 14 + 60, frame + 14 + 20, 8);
	memset(frame + 14 + 20, 1, 40);
	frame[14] = 0x4f;
	frame[14 + 8] = 1;
	put16(frame + 14 + 2, 68);
	put16(frame + 14 + 10, 0);
	put16(frame + 14 + 10, ~sum(0, frame + 14, 60));
	send_frame(n, 14 + 68);
	len = take(n);
	CHECK_EQ(len, 14 + 20 + 8 + 68);
	CHECK_EQ(sum(0, got + 34, len - 34), 0xffff);
	CHECK(memcmp(got + 42, frame + 14, 68) == 0);
	/* Not with a time to live of 2. */
	udp_frame(frame, router_mac, GUEST, 1, OUTSIDE, 9, data, 0);
	frame[14 + 8] = 2;
	put16(frame + 14 + 10, 0);
	put16(frame + 14 + 10, ~sum(0, frame + 14, 20));
	send_frame(n, 14 + 28);
	CHECK_EQ(take(n), 0);
	/* Not for UDP to the router, nor for ICMP out. */
	udp_frame(frame, router_mac, GUEST, 1, DNS, 9, data, 0);
	frame[14 + 8] = 1;
	put16(frame + 14 + 10, 0);
	put16(frame + 14 + 10, ~sum(0, frame + 14, 20));
	send_frame(n, 14 + 28);
	icmp_frame(frame, OUTSIDE, 8, data, 0);
	frame[14 + 8] = 1;
	put16(frame + 14 + 10, 0);
	put16(frame + 14 + 10, ~sum(0, frame + 14, 20));
	send_frame(n, 14 + 28);
	CHECK_EQ(take(n), 0);
	net_destroy(n);
}

/*
 * Where the router sends a datagram, seen with a time to live of 1: an
 * address outside gets a time exceeded, one that goes nowhere nothing.
 */
static void test_route(void)
{
	static const struct {
		uint32_t dst;
		bool outside;
	} dsts[] = {
		{0x00010203, false},                     /* 0.1.2.3 */
		{0x01020304, true},  {0x7e000001, true}, /* 126.0.0.1 */
		{0x7f000001, false}, {0x7f123456, false}, {0x80000001, true},
		{0xdfffffff, true},  /* 223.255.255.255 */
		{0xe0000001, false}, /* multicast */
		{0xe1000001, false}, {0xf0000001, false}, {0xff010101, false},
		{0x0a000209, false}, /* another address of the network */
		{0x0a000301, true},  /* 10.0.3.1: another network */
		{0x0a010202, true},  {0x0b000202, true},
	};
	NetLink *n = open_user(53);
	uint8_t data[4] = {0};

	for (size_t i = 0; i < sizeof(dsts) / sizeof(dsts[0]); i++) {
		udp_frame(frame, router_mac, GUEST, 1, dsts[i].dst, 9, data, 4);
		frame[14 + 8] = 1;
		put16(frame + 14 + 10, 0);
		put16(frame + 14 + 10, ~sum(0, frame + 14, 20));
		send_frame(n, 14 + 32);
		CHECK_EQ(take(n), dsts[i].outside ? 14 + 20 + 8 + 28 : 0);
	}
	net_destroy(n);
}

/* A DHCP message from the guest, with its options and the end. */
static size_t dhcp_msg(uint8_t *m, uint8_t type, uint32_t ciaddr,
                       uint16_t flags, const uint8_t *opts, size_t olen)
{
	size_t len = 240;

	memset(m, 0, 240);
	m[0] = 1;
	m[1] = 1;
	m[2] = 6;
	put32(m + 4, 0xdeadbeef);
	put16(m + 8, 0x0103); /* secs */
	put16(m + 10, flags);
	put32(m + 12, ciaddr);
	memcpy(m + 28, guest_mac, 6);
	for (int i = 6; i < 16; i++)
		m[28 + i] = (uint8_t)(0xa0 + i); /* what the server copies back */
	m[44] = 's';                         /* a server name, not copied */
	put32(m + 236, 0x63825363);
	if (type != 0) {
		m[len++] = 53;
		m[len++] = 1;
		m[len++] = type;
	}
	if (olen > 0)
		memcpy(m + len, opts, olen);
	len += olen;
	m[len++] = 255;
	return len;
}

static size_t dhcp_frame(uint8_t *f, uint32_t src, uint32_t dst,
                         const uint8_t *m, size_t len)
{
	return udp_frame(f, dst == 0xffffffff ? bcast_mac : router_mac, src, 68,
	                 dst, 67, m, len);
}

/* The option code of a reply in got, or NULL. */
static const uint8_t *option(size_t len, uint8_t code)
{
	const uint8_t *m = got + 42;

	for (size_t i = 240; i + 1 < len - 42 && m[i] != 255; i += 2 + m[i + 1])
		if (m[i] == code)
			return m + i;
	return NULL;
}

/* A reply of type to the guest's message, sent to ip and mac. */
static void check_reply(size_t len, uint8_t type, const uint8_t mac[6],
                        uint32_t ip, uint32_t ciaddr, uint32_t yiaddr)
{
	const uint8_t *m = got + 42, *o;

	CHECK_EQ(check_udp(len, mac, GATEWAY, 67, ip, 68), 300);
	CHECK_EQ(m[0], 2);
	CHECK_EQ(m[1], 1);
	CHECK_EQ(m[2], 6);
	CHECK_EQ(m[3], 0);
	CHECK_EQ(get32(m + 4), 0xdeadbeef);
	CHECK_EQ(get16(m + 8), 0);
	CHECK_EQ(get32(m + 12), ciaddr);
	CHECK_EQ(get32(m + 16), yiaddr);
	CHECK_EQ(get32(m + 20), 0);
	CHECK_EQ(get32(m + 24), 0);
	CHECK(memcmp(m + 28, guest_mac, 6) == 0);
	for (int i = 6; i < 16; i++)
		CHECK_EQ(m[28 + i], 0xa0 + i);
	for (int i = 44; i < 236; i++)
		CHECK_EQ(m[i], 0); /* sname and file */
	CHECK_EQ(get32(m + 236), 0x63825363);
	CHECK_EQ(m[240], 53); /* the type first */
	CHECK_EQ(m[241], 1);
	CHECK_EQ(m[242], type);
	o = option(len, 54);
	CHECK(o != NULL && o[1] == 4 && get32(o + 2) == GATEWAY);
	/* The options end with END, then pads. */
	size_t i = 240;
	while (i < 300 && m[i] != 255)
		i += 2 + m[i + 1];
	CHECK(i < 300 && m[i] == 255);
	for (i++; i < 300; i++)
		CHECK_EQ(m[i], 0);
}

static void check_settings(size_t len, bool lease)
{
	const uint8_t *o = option(len, 51);

	if (lease)
		CHECK(o != NULL && o[1] == 4 && get32(o + 2) == 86400);
	else
		CHECK(o == NULL);
	o = option(len, 1);
	CHECK(o != NULL && o[1] == 4 && get32(o + 2) == 0xffffff00);
	o = option(len, 3);
	CHECK(o != NULL && o[1] == 4 && get32(o + 2) == GATEWAY);
	o = option(len, 6);
	CHECK(o != NULL && o[1] == 4 && get32(o + 2) == DNS);
	CHECK(option(len, 56) == NULL);
}

static void check_nak(size_t len)
{
	const uint8_t *o = option(len, 56);

	check_reply(len, 6, bcast_mac, 0xffffffff, 0, 0);
	CHECK(o != NULL && o[1] == 13 && memcmp(o + 2, "wrong address", 13) == 0);
	CHECK(o != NULL && o[15] == 255);
	CHECK(option(len, 51) == NULL);
	CHECK(option(len, 1) == NULL);
	CHECK(option(len, 3) == NULL);
}

static void test_dhcp(void)
{
	NetLink *n = open_user(53);
	uint8_t m[600];
	uint8_t opts[32];
	size_t len;

	/* DISCOVER: unicast to the address offered, then broadcast. */
	len = dhcp_msg(m, 1, 0, 0, NULL, 0);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	len = take1(n);
	check_reply(len, 2, guest_mac, GUEST, 0, GUEST);
	check_settings(len, true);
	CHECK_EQ(get16(got + 42 + 10), 0);
	len = dhcp_msg(m, 1, 0, 0x8000, NULL, 0);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	len = take1(n);
	check_reply(len, 2, bcast_mac, 0xffffffff, 0, GUEST);
	CHECK_EQ(get16(got + 42 + 10), 0x8000);

	/* REQUEST, selecting: the server's id and the address offered. */
	uint8_t sel[] = {0, 54, 4, 10, 0, 2, 2, 50, 4, 10, 0, 2, 15};
	len = dhcp_msg(m, 3, 0, 0, sel, sizeof(sel));
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	len = take1(n);
	check_reply(len, 5, guest_mac, GUEST, 0, GUEST);
	check_settings(len, true);
	/* Another address: no. */
	sel[12] = 16;
	len = dhcp_msg(m, 3, 0, 0, sel, sizeof(sel));
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	check_nak(take1(n));
	/* The server's id without an address. */
	len = dhcp_msg(m, 3, 0, 0, sel, 7);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	check_nak(take1(n));
	/* Another server's offer taken: silence. */
	sel[6] = 1;
	sel[12] = 15;
	len = dhcp_msg(m, 3, 0, 0, sel, sizeof(sel));
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	CHECK_EQ(take1(n), 0);

	/* INIT-REBOOT: the address alone. */
	len = dhcp_msg(m, 3, 0, 0, sel + 7, 6);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	check_reply(take1(n), 5, guest_mac, GUEST, 0, GUEST);
	memcpy(opts, sel + 7, 6);
	opts[5] = 99;
	len = dhcp_msg(m, 3, 0, 0, opts, 6);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	check_nak(take1(n));

	/* RENEWING: 'ciaddr', unicast to the server, the answer to it. */
	len = dhcp_msg(m, 3, GUEST, 0, NULL, 0);
	send_frame(n, dhcp_frame(frame, GUEST, GATEWAY, m, len));
	len = take1(n);
	check_reply(len, 5, guest_mac, GUEST, GUEST, GUEST);
	check_settings(len, true);
	/* REBINDING, broadcast, with the broadcast flag: still to 'ciaddr'. */
	len = dhcp_msg(m, 3, GUEST, 0x8000, NULL, 0);
	send_frame(n, dhcp_frame(frame, GUEST, 0x0a0002ff, m, len));
	check_reply(take1(n), 5, guest_mac, GUEST, GUEST, GUEST);
	len = dhcp_msg(m, 3, 0x0a000210, 0, NULL, 0);
	send_frame(n, dhcp_frame(frame, 0x0a000210, GATEWAY, m, len));
	check_nak(take1(n));
	/* A request with nothing to check. */
	len = dhcp_msg(m, 3, 0, 0, NULL, 0);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	CHECK_EQ(take1(n), 0);

	/* INFORM: settings, no address nor lease, to 'ciaddr'. */
	len = dhcp_msg(m, 8, GUEST, 0, NULL, 0);
	send_frame(n, dhcp_frame(frame, GUEST, GATEWAY, m, len));
	len = take1(n);
	check_reply(len, 5, guest_mac, GUEST, 0, 0);
	check_settings(len, false);
	len = dhcp_msg(m, 8, 0, 0, NULL, 0);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	check_reply(take1(n), 5, bcast_mac, 0xffffffff, 0, 0);

	/* Only the broadcast flag is the flag: the others are copied back. */
	len = dhcp_msg(m, 1, 0, 0x0001, NULL, 0);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	len = take1(n);
	check_reply(len, 2, guest_mac, GUEST, 0, GUEST);
	CHECK_EQ(get16(got + 42 + 10), 0x0001);
	/* A DISCOVER with the address it wants is offered it, once. */
	len = dhcp_msg(m, 1, 0, 0, sel + 7, 6);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	check_reply(take1(n), 2, guest_mac, GUEST, 0, GUEST);
	/*
	 * To the client's hardware address with no 'ciaddr', to the frame's
	 * source with one.
	 */
	static const uint8_t chaddr[6] = {0x02, 0, 0, 0, 0, 0x77};
	len = dhcp_msg(m, 1, 0, 0, NULL, 0);
	memcpy(m + 28, chaddr, 6);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	len = take1(n);
	CHECK(memcmp(got, chaddr, 6) == 0);
	len = dhcp_msg(m, 3, GUEST, 0, NULL, 0);
	memcpy(m + 28, chaddr, 6);
	send_frame(n, dhcp_frame(frame, GUEST, GATEWAY, m, len));
	len = take1(n);
	CHECK(memcmp(got, guest_mac, 6) == 0);

	/* Silence: DECLINE, RELEASE, a server's message, BOOTP. */
	static const uint8_t quiet[] = {4, 7, 2, 5, 6, 0};
	for (int i = 0; i < 6; i++) {
		len = dhcp_msg(m, quiet[i], 0, 0, NULL, 0);
		send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
		CHECK_EQ(take1(n), 0);
	}
	net_destroy(n);
}

static void test_dhcp_malformed(void)
{
	NetLink *n = open_user(53);
	uint8_t m[600];
	size_t len;

	/* Each fixed field the server checks, wrong. */
	static const int field[] = {0, 1, 2, 24, 236};
	for (int i = 0; i < 5; i++) {
		len = dhcp_msg(m, 1, 0, 0, NULL, 0);
		m[field[i] + (field[i] >= 24 ? 3 : 0)] ^= 0x10;
		send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
		CHECK_EQ(take1(n), 0);
	}
	/* No room for the options. */
	len = dhcp_msg(m, 0, 0, 0, NULL, 0);
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, 239));
	CHECK_EQ(take1(n), 0);

	/* Pads before the type, and unknown options, are passed over. */
	static const uint8_t lead[] = {0, 0, 12, 3, 'p', 'i', '3'};
	len = dhcp_msg(m, 0, 0, 0, lead, sizeof(lead));
	m[len - 1] = 53;
	m[len++] = 1;
	m[len++] = 1;
	m[len++] = 255;
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	CHECK(take1(n) > 0);
	/* A type of the wrong size is no type. */
	static const uint8_t two[] = {53, 2, 1, 1};
	len = dhcp_msg(m, 0, 0, 0, two, sizeof(two));
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	CHECK_EQ(take1(n), 0);
	/* After the end, nothing counts. */
	static const uint8_t after[] = {255, 0, 53, 1, 1};
	len = dhcp_msg(m, 0, 0, 0, after, sizeof(after));
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	CHECK_EQ(take1(n), 0);
	/* Options cut short: what came before counts, what is cut does not. */
	len = dhcp_msg(m, 1, 0, 0, NULL, 0) - 1; /* no end */
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	CHECK(take1(n) > 0);
	m[len++] = 54; /* a code and no length */
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	CHECK(take1(n) > 0);
	static const uint8_t cut[] = {53, 1, 1, 50, 4, 10, 0, 2};
	len = dhcp_msg(m, 0, 0, 0, cut, sizeof(cut)) - 1;
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	CHECK(take1(n) > 0);
	/*
	 * A requested address cut by its last byte, which follows in the
	 * frame after the UDP data: not read, so the request checks nothing.
	 */
	static const uint8_t req[] = {53, 1, 3, 50, 4, 10, 0, 2, 15};
	static const uint32_t to[2] = {0xffffffff, GATEWAY};
	for (int i = 0; i < 2; i++) {
		len = dhcp_msg(m, 0, 0, 0, req, sizeof(req)) - 1; /* no end */
		dhcp_frame(frame, 0, to[i], m, len);
		put16(frame + 38, (uint32_t)(8 + len - 1));
		put16(frame + 40, 0);
		send_frame(n, 14 + 20 + 8 + len);
		CHECK_EQ(take1(n), 0);
	}
	/* A requested address or server id of the wrong size is ignored. */
	static const uint8_t odd[] = {53, 1, 3,  50, 3, 10, 0, 2,
	                              54, 5, 10, 0,  2, 2,  0};
	len = dhcp_msg(m, 0, 0, 0, odd, sizeof(odd));
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	CHECK_EQ(take1(n), 0);
	static const uint8_t odd2[] = {53, 1, 3,  54, 5,  10, 0, 2,
	                               3,  0, 50, 4,  10, 0,  2, 15};
	len = dhcp_msg(m, 0, 0, 0, odd2, sizeof(odd2));
	send_frame(n, dhcp_frame(frame, 0, 0xffffffff, m, len));
	check_reply(take1(n), 5, guest_mac, GUEST, 0, GUEST);

	/* DHCP to the name server's address, or to another port: not served. */
	len = dhcp_msg(m, 1, 0, 0, NULL, 0);
	send_frame(n, dhcp_frame(frame, 0, DNS, m, len));
	send_frame(n, udp_frame(frame, bcast_mac, 0, 68, 0xffffffff, 68, m, len));
	send_frame(n, udp_frame(frame, bcast_mac, 0, 68, 0x0a0002ff, 66, m, len));
	CHECK_EQ(take1(n), 0);
	net_destroy(n);
}

/* A UDP socket of the test on the host's loopback, and its port. */
static int host_socket(int family, uint16_t *port)
{
	struct sockaddr_storage a;
	socklen_t alen = sizeof(a);
	int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	memset(&a, 0, sizeof(a));
	if (family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in *)&a;

		in->sin_family = AF_INET;
		in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		alen = sizeof(*in);
	} else {
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&a;

		in6->sin6_family = AF_INET6;
		in6->sin6_addr = in6addr_loopback;
		alen = sizeof(*in6);
	}
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, alen) != 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	alen = sizeof(a);
	CHECK(getsockname(fd, (struct sockaddr *)&a, &alen) == 0);
	*port = ntohs(family == AF_INET ? ((struct sockaddr_in *)&a)->sin_port
	                                : ((struct sockaddr_in6 *)&a)->sin6_port);
	return fd;
}

static void write_resolv(const char *text)
{
	FILE *f = fopen(RESOLV, "w");

	CHECK(f != NULL);
	fputs(text, f);
	fclose(f);
}

/*
 * The guest sends data from sport to dst:dport; the host's socket fd
 * gets it: returns the length, and where from in *from.
 */
static ssize_t host_got(int fd, uint8_t *buf, size_t size,
                        struct sockaddr_storage *from)
{
	socklen_t flen = sizeof(*from);

	return recvfrom(fd, buf, size, MSG_DONTWAIT, (struct sockaddr *)from,
	                &flen);
}

static void test_udp_loopback(void)
{
	uint16_t port;
	int fd = host_socket(AF_INET, &port);
	NetLink *n = open_user(53);
	uint8_t buf[100];
	struct sockaddr_storage from, from2;
	size_t len;

	CHECK(fd >= 0);
	/* To the router's address: the host's loopback. */
	send_frame(n, udp_frame(frame, router_mac, GUEST, 5000, GATEWAY, port,
	                        (const uint8_t *)"hello", 5));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), 5);
	CHECK(memcmp(buf, "hello", 5) == 0);
	CHECK_EQ(take(n), 0);
	/* The answer, to the guest's port, from the router's. */
	CHECK_EQ(sendto(fd, "world!", 6, 0, (struct sockaddr *)&from,
	                sizeof(struct sockaddr_in)),
	         6);
	len = take(n);
	CHECK_EQ(check_udp(len, guest_mac, GATEWAY, port, GUEST, 5000), 6);
	CHECK(memcmp(got + 42, "world!", 6) == 0);
	CHECK_EQ(take(n), 0);

	/* The same mapping again: the same port of the host. */
	send_frame(n, udp_frame(frame, router_mac, GUEST, 5000, GATEWAY, port,
	                        (const uint8_t *)"a", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from2), 1);
	CHECK(memcmp(&from, &from2, sizeof(struct sockaddr_in)) == 0);
	/* Another port of the guest: another of the host. */
	send_frame(n, udp_frame(frame, router_mac, GUEST, 5001, GATEWAY, port,
	                        (const uint8_t *)"b", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from2), 1);
	CHECK(memcmp(&from, &from2, sizeof(struct sockaddr_in)) != 0);
	CHECK_EQ(sendto(fd, "B", 1, 0, (struct sockaddr *)&from2,
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK_EQ(check_udp(take(n), guest_mac, GATEWAY, port, GUEST, 5001), 1);
	/* Another address of the guest, with no checksum (0: none). */
	udp_frame(frame, router_mac, 0x0a000220, 5000, GATEWAY, port,
	          (const uint8_t *)"c", 1);
	put16(frame + 40, 0);
	send_frame(n, 14 + 20 + 9);
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from2), 1);
	CHECK(memcmp(&from, &from2, sizeof(struct sockaddr_in)) != 0);
	CHECK_EQ(sendto(fd, "C", 1, 0, (struct sockaddr *)&from2,
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK_EQ(check_udp(take(n), guest_mac, GATEWAY, port, 0x0a000220, 5000), 1);

	/* Not sent: a wrong checksum, a length too long or too short. */
	for (int i = 0; i < 4; i++) {
		size_t flen = udp_frame(frame, router_mac, GUEST, 5000, GATEWAY, port,
		                        (const uint8_t *)"xy", 2);

		if (i == 0)
			frame[40]++;
		else if (i == 1)
			put16(frame + 38, 11);
		else if (i == 2)
			put16(frame + 38, 7);
		if (i == 1 || i == 2)
			put16(frame + 40, 0); /* no checksum: the length alone */
		else
			flen = 14 + 20 + 7;
		if (i == 3) {
			put16(frame + 16, 27);
			put16(frame + 24, 0);
			put16(frame + 24, ~sum(0, frame + 14, 20));
		}
		send_frame(n, flen);
		CHECK_EQ(host_got(fd, buf, sizeof(buf), &from2), -1);
	}
	/* A length shorter than the datagram: what is after it is not sent. */
	udp_frame(frame, router_mac, GUEST, 5000, GATEWAY, port,
	          (const uint8_t *)"xyz", 3);
	put16(frame + 38, 10);
	put16(frame + 40, 0);
	send_frame(n, 14 + 20 + 11);
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from2), 2);
	/* To the host's own loopback address, from the guest: nowhere. */
	send_frame(n, udp_frame(frame, router_mac, GUEST, 5000, 0x7f000001, port,
	                        (const uint8_t *)"z", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from2), -1);
	/* Broadcast, but for DHCP, goes nowhere. */
	send_frame(n, udp_frame(frame, bcast_mac, GUEST, 5000, 0xffffffff, port,
	                        (const uint8_t *)"z", 1));
	send_frame(n, udp_frame(frame, bcast_mac, GUEST, 5000, 0x0a0002ff, port,
	                        (const uint8_t *)"z", 1));
	send_frame(n, udp_frame(frame, router_mac, GUEST, 5000, 0x0a000209, port,
	                        (const uint8_t *)"z", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from2), -1);
	CHECK_EQ(take(n), 0);
	net_destroy(n);
	close(fd);
}

static void test_udp_guest_mac(void)
{
	uint16_t port;
	int fd = host_socket(AF_INET, &port);
	NetLink *n = open_user(53);
	uint8_t buf[10];
	struct sockaddr_storage from;

	send_frame(n, udp_frame(frame, router_mac, GUEST, 7, GATEWAY, port,
	                        (const uint8_t *)"m", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), 1);
	/* A multicast source is no guest's address; a new one is. */
	arp_frame(frame, bcast_mac, 2, GUEST);
	frame[6] = 0x01;
	send_frame(n, 60);
	CHECK_EQ(sendto(fd, "1", 1, 0, (struct sockaddr *)&from,
	                sizeof(struct sockaddr_in)),
	         1);
	check_udp(take(n), guest_mac, GATEWAY, port, GUEST, 7);
	arp_frame(frame, bcast_mac, 2, GUEST);
	frame[11] = 0x44;
	send_frame(n, 60);
	CHECK_EQ(sendto(fd, "2", 1, 0, (struct sockaddr *)&from,
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK(take(n) > 0);
	CHECK_EQ(got[5], 0x44);
	net_destroy(n);
	close(fd);
}

static void test_udp_fragments(void)
{
	uint16_t port;
	int fd = host_socket(AF_INET, &port);
	NetLink *n = open_user(53);
	static uint8_t big[4000], whole[4100];
	struct sockaddr_storage from;
	uint8_t buf[4];
	size_t offset = 0;
	uint16_t id = 0;

	send_frame(n, udp_frame(frame, router_mac, GUEST, 9, GATEWAY, port,
	                        (const uint8_t *)"q", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), 1);
	for (size_t i = 0; i < sizeof(big); i++)
		big[i] = (uint8_t)(i * 7);
	CHECK_EQ(sendto(fd, big, sizeof(big), 0, (struct sockaddr *)&from,
	                sizeof(struct sockaddr_in)),
	         sizeof(big));
	/* 4008 bytes of UDP: 1480, 1480 and 1048. */
	for (int i = 0; i < 3; i++) {
		size_t len = take(n);
		size_t part = i < 2 ? 1480 : 1048;

		CHECK_EQ(len, 34 + part);
		check_ip(len, guest_mac, 17, GATEWAY, GUEST);
		if (i == 0)
			id = get16(got + 18);
		CHECK_EQ(get16(got + 18), id);
		CHECK_EQ(get16(got + 20), (i < 2 ? 0x2000 : 0) | offset / 8);
		if (len == 34 + part)
			memcpy(whole + offset, got + 34, part);
		offset += part;
	}
	CHECK_EQ(take(n), 0);
	CHECK_EQ(get16(whole + 4), 4008);
	CHECK_EQ(sum(pseudo(GATEWAY, GUEST, 17, 4008), whole, 4008), 0xffff);
	CHECK(memcmp(whole + 8, big, sizeof(big)) == 0);
	/* An empty datagram is one frame. */
	CHECK_EQ(sendto(fd, big, 0, 0, (struct sockaddr *)&from,
	                sizeof(struct sockaddr_in)),
	         0);
	CHECK_EQ(check_udp(take(n), guest_mac, GATEWAY, port, GUEST, 9), 0);
	/* The checksum that sums to 0 is sent as all ones (RFC 768). */
	net_destroy(n);
	close(fd);
}

static void test_udp_zero_sum(void)
{
	uint16_t port;
	int fd = host_socket(AF_INET, &port);
	NetLink *n = open_user(53);
	struct sockaddr_storage from;
	uint8_t data[2], buf[4];
	uint32_t s;

	send_frame(n, udp_frame(frame, router_mac, GUEST, 9, GATEWAY, port,
	                        (const uint8_t *)"q", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), 1);
	/* Two bytes of data that make the sum, checksum 0, all ones. */
	uint8_t hdr[8];
	put16(hdr, port);
	put16(hdr + 2, 9);
	put16(hdr + 4, 10);
	put16(hdr + 6, 0);
	s = sum(pseudo(GATEWAY, GUEST, 17, 10), hdr, 8);
	put16(data, 0xffff - s);
	CHECK_EQ(sendto(fd, data, 2, 0, (struct sockaddr *)&from,
	                sizeof(struct sockaddr_in)),
	         2);
	CHECK_EQ(take(n), 44);
	CHECK_EQ(get16(got + 40), 0xffff);
	net_destroy(n);
	close(fd);
}

static void test_udp_many(void)
{
	uint16_t port;
	int fd = host_socket(AF_INET, &port);
	NetLink *n = open_user(53);
	struct sockaddr_storage from;
	uint8_t buf[4];
	unsigned count = 0;

	send_frame(n, udp_frame(frame, router_mac, GUEST, 9, GATEWAY, port,
	                        (const uint8_t *)"q", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), 1);
	/* More than the queue holds: they wait on the host, none is lost. */
	for (unsigned i = 0; i < 130; i++) {
		buf[0] = (uint8_t)i;
		CHECK_EQ(sendto(fd, buf, 1, 0, (struct sockaddr *)&from,
		                sizeof(struct sockaddr_in)),
		         1);
	}
	for (unsigned i = 0; i < 250 && take(n) == 43; i++) {
		CHECK_EQ(got[42], (uint8_t)count);
		count++;
	}
	CHECK_EQ(count, 130);
	net_destroy(n);
	close(fd);
}

static void test_udp_icmp_error(void)
{
	uint16_t port, closed;
	int fd = host_socket(AF_INET, &port);
	int gone = host_socket(AF_INET, &closed);
	NetLink *n = open_user(53);
	struct sockaddr_storage from;
	uint8_t buf[4];

	close(gone);
	/* To a port nobody listens on: the host's ICMP error is passed over. */
	send_frame(n, udp_frame(frame, router_mac, GUEST, 9, GATEWAY, closed,
	                        (const uint8_t *)"q", 1));
	CHECK_EQ(take(n), 0);
	send_frame(n, udp_frame(frame, router_mac, GUEST, 9, GATEWAY, port,
	                        (const uint8_t *)"q", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), 1);
	CHECK_EQ(sendto(fd, "r", 1, 0, (struct sockaddr *)&from,
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK_EQ(take(n), 43);
	net_destroy(n);
	close(fd);
}

static void test_udp_idle(void)
{
	uint16_t port;
	int fd = host_socket(AF_INET, &port);
	NetLink *n = open_user(53);
	struct sockaddr_storage from, from2;
	uint8_t buf[4];

	now = 1000;
	send_frame(n, udp_frame(frame, router_mac, GUEST, 9, GATEWAY, port,
	                        (const uint8_t *)"q", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), 1);
	/* Five minutes idle: kept; an answer keeps it five more. */
	for (int i = 0; i < 2; i++) {
		now += 300 * SECOND;
		CHECK_EQ(sendto(fd, "r", 1, 0, (struct sockaddr *)&from,
		                sizeof(struct sockaddr_in)),
		         1);
		CHECK_EQ(take(n), 43);
	}
	/* One nanosecond more: closed, its answers lost; a new port. */
	now += 300 * SECOND + 1;
	CHECK_EQ(take(n), 0);
	CHECK_EQ(sendto(fd, "r", 1, 0, (struct sockaddr *)&from,
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK_EQ(take(n), 0);
	send_frame(n, udp_frame(frame, router_mac, GUEST, 9, GATEWAY, port,
	                        (const uint8_t *)"q", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from2), 1);
	CHECK(memcmp(&from, &from2, sizeof(struct sockaddr_in)) != 0);
	now = 0;
	net_destroy(n);
	close(fd);
}

static void test_udp_evict(void)
{
	uint16_t port;
	int fd = host_socket(AF_INET, &port);
	NetLink *n = open_user(53);
	struct sockaddr_storage from[66], again;
	uint8_t buf[4];

	/* 64 mappings, the first used again last: the second goes first. */
	for (int i = 0; i < 64; i++) {
		now = 10 + (uint64_t)i;
		send_frame(n, udp_frame(frame, router_mac, GUEST, (uint16_t)(100 + i),
		                        GATEWAY, port, (const uint8_t *)"e", 1));
		CHECK_EQ(host_got(fd, buf, sizeof(buf), &from[i]), 1);
	}
	now = 100;
	send_frame(n, udp_frame(frame, router_mac, GUEST, 100, GATEWAY, port,
	                        (const uint8_t *)"e", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &again), 1);
	CHECK(memcmp(&again, &from[0], sizeof(struct sockaddr_in)) == 0);
	now = 101;
	send_frame(n, udp_frame(frame, router_mac, GUEST, 200, GATEWAY, port,
	                        (const uint8_t *)"e", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from[64]), 1);
	/* The second's answer is lost; the first's and the newest's come. */
	CHECK_EQ(sendto(fd, "1", 1, 0, (struct sockaddr *)&from[1],
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK_EQ(take(n), 0);
	CHECK_EQ(sendto(fd, "0", 1, 0, (struct sockaddr *)&from[0],
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK_EQ(check_udp(take(n), guest_mac, GATEWAY, port, GUEST, 100), 1);
	CHECK_EQ(sendto(fd, "n", 1, 0, (struct sockaddr *)&from[64],
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK_EQ(check_udp(take(n), guest_mac, GATEWAY, port, GUEST, 200), 1);
	/* The third is the next to go. */
	now = 102;
	send_frame(n, udp_frame(frame, router_mac, GUEST, 201, GATEWAY, port,
	                        (const uint8_t *)"e", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from[65]), 1);
	CHECK_EQ(sendto(fd, "2", 1, 0, (struct sockaddr *)&from[2],
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK_EQ(sendto(fd, "3", 1, 0, (struct sockaddr *)&from[3],
	                sizeof(struct sockaddr_in)),
	         1);
	CHECK_EQ(check_udp(take(n), guest_mac, GATEWAY, port, GUEST, 103), 1);
	CHECK_EQ(got[42], '3');
	CHECK_EQ(take(n), 0);
	now = 0;
	net_destroy(n);
	close(fd);
}

/*
 * Whether the host's port at a is closed: a datagram to it from fd, the
 * mapping's peer, gets the host's port unreachable back (IP_RECVERR puts
 * it in fd's error queue). An open one takes it: its guest's frame.
 */
static bool port_closed(int fd, const struct sockaddr_storage *a)
{
	int on = 1;
	char c, control[256];
	struct iovec v = {.iov_base = &c, .iov_len = 1};
	struct msghdr m = {.msg_iov = &v,
	                   .msg_iovlen = 1,
	                   .msg_control = control,
	                   .msg_controllen = sizeof(control)};
	struct pollfd p = {.fd = fd};

	CHECK(setsockopt(fd, SOL_IP, IP_RECVERR, &on, sizeof(on)) == 0);
	CHECK_EQ(sendto(fd, "x", 1, 0, (const struct sockaddr *)a,
	                sizeof(struct sockaddr_in)),
	         1);
	poll(&p, 1, 100);
	return recvmsg(fd, &m, MSG_ERRQUEUE | MSG_DONTWAIT) >= 0;
}

/* The guest's datagram to port from sport at 10.0.2.2; where it came from. */
static void guest_to_host(NetLink *n, int fd, uint16_t port, uint16_t sport,
                          struct sockaddr_storage *from)
{
	uint8_t buf[4];

	send_frame(n, udp_frame(frame, router_mac, GUEST, sport, GATEWAY, port,
	                        (const uint8_t *)"s", 1));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), from), 1);
}

/* The host's sockets close with the link, at eviction and when idle. */
static void test_udp_closed(void)
{
	uint16_t port;
	int fd = host_socket(AF_INET, &port);
	NetLink *n = open_user(53);
	struct sockaddr_storage first, second, other;

	now = 0;
	guest_to_host(n, fd, port, 1, &first);
	guest_to_host(n, fd, port, 2, &second);
	CHECK(!port_closed(fd, &first));
	CHECK(!port_closed(fd, &second));
	net_destroy(n);
	CHECK(port_closed(fd, &first));
	CHECK(port_closed(fd, &second));

	/* The one evicted is closed. */
	n = open_user(53);
	guest_to_host(n, fd, port, 1, &first);
	for (int i = 0; i < 64; i++) {
		now = 1 + (uint64_t)i;
		guest_to_host(n, fd, port, (uint16_t)(100 + i), &other);
	}
	CHECK(port_closed(fd, &first));
	CHECK(!port_closed(fd, &other));
	/* Idle ones are closed. */
	now += 301 * SECOND;
	CHECK_EQ(take(n), 0);
	CHECK(port_closed(fd, &other));
	now = 0;
	net_destroy(n);
	close(fd);
}

/* Datagrams of every size the host can give, and what UDP allows. */
static void test_udp_sizes(void)
{
	uint16_t port, port6;
	int fd = host_socket(AF_INET, &port);
	int fd6 = host_socket(AF_INET6, &port6);
	NetLink *n = open_user(53);
	static uint8_t big[65600];
	struct sockaddr_storage from;
	uint8_t buf[4];
	size_t len;
	unsigned frames;

	/* An empty datagram from the guest is sent. */
	send_frame(n,
	           udp_frame(frame, router_mac, GUEST, 9, GATEWAY, port, big, 0));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), 0);
	/* 1472 bytes fill one frame; 1473 take two; 65507, the most, 45. */
	static const size_t sizes[] = {1472, 1473, 65507};
	static const unsigned want[] = {1, 2, 45};
	for (int i = 0; i < 3; i++) {
		CHECK_EQ(sendto(fd, big, sizes[i], 0, (struct sockaddr *)&from,
		                sizeof(struct sockaddr_in)),
		         sizes[i]);
		frames = 0;
		while (frames < 50 && (len = take(n)) != 0) {
			check_ip(len, guest_mac, 17, GATEWAY, GUEST);
			frames++;
		}
		CHECK_EQ(frames, want[i]);
		CHECK_EQ(get16(got + 20) & 0x2000, 0); /* the last fragment */
	}
	/* Over IPv6 the host takes 65508 bytes, more than IPv4 carries. */
	if (fd6 >= 0) {
		char text[100];

		snprintf(text, sizeof(text), "nameserver ::1\n");
		write_resolv(text);
		net_destroy(n);
		n = open_user(port6);
		send_frame(n, udp_frame(frame, router_mac, GUEST, 4, DNS, 53, big, 1));
		CHECK_EQ(host_got(fd6, buf, sizeof(buf), &from), 1);
		CHECK_EQ(sendto(fd6, big, 65508, 0, (struct sockaddr *)&from,
		                sizeof(struct sockaddr_in6)),
		         65508);
		CHECK_EQ(sendto(fd6, big, 3, 0, (struct sockaddr *)&from,
		                sizeof(struct sockaddr_in6)),
		         3);
		CHECK_EQ(check_udp(take(n), guest_mac, DNS, 53, GUEST, 4), 3);
		CHECK_EQ(take(n), 0);
		unlink(RESOLV);
		close(fd6);
	}
	net_destroy(n);
	close(fd);
}

/* A name server the host cannot connect to: the datagram is lost. */
static void test_dns_unreachable(void)
{
	NetLink *n;

	/* A link-local address with no interface: connect() fails. */
	write_resolv("nameserver fe80::1\n");
	n = open_user(53);
	send_frame(n, udp_frame(frame, router_mac, GUEST, 4, DNS, 53,
	                        (const uint8_t *)"q", 1));
	send_frame(n, udp_frame(frame, router_mac, GUEST, 4, DNS, 53,
	                        (const uint8_t *)"q", 1));
	CHECK_EQ(take(n), 0);
	net_destroy(n);
	unlink(RESOLV);
}

/* The name server: the guest asks 10.0.2.3, the host's server answers. */
static void dns_round_trip(int fd, uint16_t dns_port)
{
	NetLink *n = open_user(dns_port);
	struct sockaddr_storage from;
	uint8_t buf[16];

	send_frame(n, udp_frame(frame, router_mac, GUEST, 4444, DNS, 53,
	                        (const uint8_t *)"query", 5));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), 5);
	CHECK_EQ(sendto(fd, "answer", 6, 0, (struct sockaddr *)&from,
	                from.ss_family == AF_INET ? sizeof(struct sockaddr_in)
	                                          : sizeof(struct sockaddr_in6)),
	         6);
	CHECK_EQ(check_udp(take(n), guest_mac, DNS, 53, GUEST, 4444), 6);
	CHECK(memcmp(got + 42, "answer", 6) == 0);
	/* Another port of the name server's address: nowhere. */
	send_frame(n, udp_frame(frame, router_mac, GUEST, 4444, DNS, dns_port,
	                        (const uint8_t *)"query", 5));
	CHECK_EQ(host_got(fd, buf, sizeof(buf), &from), -1);
	net_destroy(n);
}

static void test_dns(void)
{
	uint16_t port, port6;
	int fd = host_socket(AF_INET, &port);
	int fd6 = host_socket(AF_INET6, &port6);
	char text[300];

	/* No file: the host itself. */
	unlink(RESOLV);
	dns_round_trip(fd, port);
	/* No nameserver line that counts: the host itself. */
	write_resolv("# nameserver 192.0.2.9\n; nameserver 192.0.2.9\n"
	             " nameserver 192.0.2.9\nnameserver192.0.2.9\n"
	             "nameserver not-an-address\nsearch example\n");
	dns_round_trip(fd, port);
	/* The first that names an address, after a tab. */
	write_resolv("search example\nnameserver\t127.0.0.1\n"
	             "nameserver 192.0.2.9\n");
	dns_round_trip(fd, port);
	/* IPv6, when the host has it; a host name is no address. */
	if (fd6 >= 0) {
		write_resolv("nameserver localhost\nnameserver ::1\n");
		dns_round_trip(fd6, port6);
		snprintf(text, sizeof(text), "nameserver ::1\nnameserver 127.0.0.1\n");
		write_resolv(text);
		dns_round_trip(fd6, port6);
		close(fd6);
	}
	unlink(RESOLV);
	close(fd);
}

int main(void)
{
	alarm(20);
	RUN_TEST(test_open);
	RUN_TEST(test_arp);
	RUN_TEST(test_queue_full);
	RUN_TEST(test_ping);
	RUN_TEST(test_ip_header);
	RUN_TEST(test_time_exceeded);
	RUN_TEST(test_route);
	RUN_TEST(test_dhcp);
	RUN_TEST(test_dhcp_malformed);
	RUN_TEST(test_udp_loopback);
	RUN_TEST(test_udp_guest_mac);
	RUN_TEST(test_udp_fragments);
	RUN_TEST(test_udp_zero_sum);
	RUN_TEST(test_udp_many);
	RUN_TEST(test_udp_icmp_error);
	RUN_TEST(test_udp_idle);
	RUN_TEST(test_udp_evict);
	RUN_TEST(test_udp_closed);
	RUN_TEST(test_udp_sizes);
	RUN_TEST(test_dns_unreachable);
	RUN_TEST(test_dns);
	return test_summary();
}
