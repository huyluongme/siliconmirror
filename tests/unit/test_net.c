/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for frames between the host's form and the cable's
 * (src/core/net.c), against IEEE 802.3 clause 3: 60 bytes at least, then
 * the FCS, whose CRC over the frame and the FCS leaves the residue
 * 0x2144DF1C once complemented.
 */

#include <stdint.h>
#include <string.h>

#include "core/net.h"
#include "test.h"

/* The CRC-32 of IEEE 802.3, bit by bit: not the code under test's. */
static uint32_t crc_bits(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++)
			crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
	}
	return ~crc;
}

static uint8_t frame[NET_MAX_FRAME], wire[NET_MAX_FRAME + NET_FCS_SIZE];

static void fill(size_t len)
{
	for (size_t i = 0; i < len; i++)
		frame[i] = (uint8_t)(i * 7 + 1);
}

/* A short frame: padded with zeros to 60, then the FCS. */
static void test_to_wire_short(void)
{
	fill(42);
	memset(wire, 0xaa, sizeof(wire));
	CHECK_EQ(net_to_wire(frame, 42, wire), 64);
	CHECK(memcmp(wire, frame, 42) == 0);
	for (int i = 42; i < 60; i++)
		CHECK_EQ(wire[i], 0);
	CHECK_EQ(crc_bits(wire, 64), 0x2144df1cu);
	CHECK_EQ(wire[64], 0xaa); /* nothing after */

	/* Nothing at all: 60 zeros. */
	CHECK_EQ(net_to_wire(frame, 0, wire), 64);
	for (int i = 0; i < 60; i++)
		CHECK_EQ(wire[i], 0);
	CHECK_EQ(crc_bits(wire, 64), 0x2144df1cu);
}

/* 59, 60 and 61 bytes, and the longest: no pad from 60 on. */
static void test_to_wire_lengths(void)
{
	static const size_t lens[] = {59, 60, 61, 1514, NET_MAX_FRAME};

	for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
		size_t len = lens[i], out = len < 60 ? 64 : len + 4;

		fill(len);
		CHECK_EQ(net_to_wire(frame, len, wire), out);
		CHECK(memcmp(wire, frame, len) == 0);
		CHECK_EQ(crc_bits(wire, out), 0x2144df1cu);
		/* The FCS, least significant byte first. */
		uint32_t fcs = crc_bits(wire, out - 4);
		CHECK_EQ(wire[out - 4], fcs & 0xff);
		CHECK_EQ(wire[out - 1], fcs >> 24);
	}
}

/* From the cable: the FCS goes; a runt or a wrong FCS drops the frame. */
static void test_from_wire(void)
{
	fill(100);
	CHECK_EQ(net_to_wire(frame, 100, wire), 104);
	CHECK_EQ(net_from_wire(wire, 104), 100);
	for (int bit = 0; bit < 8; bit++) {
		wire[50] ^= (uint8_t)(1u << bit); /* the data */
		CHECK_EQ(net_from_wire(wire, 104), 0);
		wire[50] ^= (uint8_t)(1u << bit);
		wire[100 + bit / 2] ^= (uint8_t)(1u << bit); /* the FCS */
		CHECK_EQ(net_from_wire(wire, 104), 0);
		wire[100 + bit / 2] ^= (uint8_t)(1u << bit);
	}
	CHECK_EQ(net_from_wire(wire, 104), 100);

	/* 64 bytes is the shortest; 63 with a good FCS is a runt. */
	fill(60);
	CHECK_EQ(net_to_wire(frame, 60, wire), 64);
	CHECK_EQ(net_from_wire(wire, 64), 60);
	memcpy(wire, frame, 59);
	uint32_t fcs = crc_bits(wire, 59);
	for (int i = 0; i < 4; i++)
		wire[59 + i] = (uint8_t)(fcs >> (8 * i));
	CHECK_EQ(net_from_wire(wire, 63), 0);
	CHECK_EQ(net_from_wire(wire, 0), 0);
}

int main(void)
{
	RUN_TEST(test_to_wire_short);
	RUN_TEST(test_to_wire_lengths);
	RUN_TEST(test_from_wire);
	return test_summary();
}
