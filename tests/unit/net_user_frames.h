/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * What the tests of the user network (backend/net/user.h) share: the
 * addresses, frames from the guest with checksums of the test's own, and
 * taking the router's frames for the guest.
 */

#ifndef TESTS_UNIT_NET_USER_FRAMES_H
#define TESTS_UNIT_NET_USER_FRAMES_H

#include <stdint.h>
#include <string.h>

#include "core/net.h"
#include "test.h"

#define GATEWAY 0x0a000202u
#define DNS 0x0a000203u
#define GUEST 0x0a00020fu
#define OUTSIDE 0xc0000201u /* 192.0.2.1, TEST-NET-1 (RFC 5737) */

#define SECOND 1000000000ull

static const uint8_t router_mac[6] = {0x02, 0x53, 0x4d, 0x00, 0x02, 0x02};
static const uint8_t bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t guest_mac[6] = {0xb8, 0x27, 0xeb, 0x11, 0x22, 0x33};

static uint8_t frame[NET_MAX_FRAME], got[NET_MAX_FRAME];
static uint64_t now;

static inline uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t get32(const uint8_t *p)
{
	return (uint32_t)get16(p) << 16 | get16(p + 2);
}

static inline void put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void put32(uint8_t *p, uint32_t v)
{
	put16(p, v >> 16);
	put16(p + 2, v);
}

/* The one's complement sum, folded; data with a right checksum sums to 0xffff.
 */
static inline uint32_t sum(uint32_t s, const uint8_t *p, size_t len)
{
	for (size_t i = 0; i < len; i++)
		s += i % 2 == 0 ? (uint32_t)p[i] << 8 : p[i];
	while (s > 0xffff)
		s = (s >> 16) + (s & 0xffff);
	return s;
}

static inline uint32_t pseudo(uint32_t src, uint32_t dst, uint8_t proto,
                              size_t len)
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
static inline size_t ip_frame(uint8_t *f, const uint8_t dst_mac[6],
                              uint8_t proto, uint32_t src, uint32_t dst,
                              uint8_t ttl, const uint8_t *data, size_t len)
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

static inline void send_frame(NetLink *n, size_t len)
{
	CHECK(n->ops->send(n, now, frame, len));
}

/* The next frame for the guest; 0 when there is none. */
static inline size_t take(NetLink *n)
{
	size_t len = 12345;

	CHECK(n->ops->receive(n, now, got, &len));
	return len;
}

/* The next frame, and no other after it. */
static inline size_t take1(NetLink *n)
{
	static uint8_t more[NET_MAX_FRAME];
	size_t len = take(n), extra = 1;

	CHECK(n->ops->receive(n, now, more, &extra));
	CHECK_EQ(extra, 0);
	return len;
}

/* A frame from the router, IPv4 with a right header: its header length. */
static inline void check_ip(size_t len, const uint8_t dst_mac[6], uint8_t proto,
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

#endif /* TESTS_UNIT_NET_USER_FRAMES_H */
