/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for src/util/byteorder.h. */

#include <stdint.h>
#include <string.h>

#include "test.h"
#include "util/byteorder.h"

static const uint8_t bytes[9] = {
	0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xff,
};

static void test_loads(void)
{
	CHECK_EQ(load_le16(bytes), 0x7788);
	CHECK_EQ(load_le32(bytes), 0x55667788);
	CHECK_EQ(load_le64(bytes), 0x1122334455667788);

	CHECK_EQ(load_le(bytes, 1), 0x88);
	CHECK_EQ(load_le(bytes, 2), 0x7788);
	CHECK_EQ(load_le(bytes, 4), 0x55667788);
	CHECK_EQ(load_le(bytes, 8), 0x1122334455667788);
}

static void test_unaligned_load(void)
{
	CHECK_EQ(load_le64(bytes + 1), 0xff11223344556677);
}

static void test_stores(void)
{
	uint8_t buf[8];

	memset(buf, 0, sizeof(buf));
	store_le64(buf, 0x1122334455667788);
	CHECK(memcmp(buf, bytes, 8) == 0);

	memset(buf, 0, sizeof(buf));
	store_le32(buf, 0x55667788);
	CHECK(memcmp(buf, bytes, 4) == 0);
	CHECK_EQ(buf[4], 0);

	memset(buf, 0, sizeof(buf));
	store_le16(buf, 0x7788);
	CHECK(memcmp(buf, bytes, 2) == 0);
	CHECK_EQ(buf[2], 0);
}

static void test_store_by_size(void)
{
	uint8_t buf[8];

	/* Only the low size bytes are stored. */
	memset(buf, 0xaa, sizeof(buf));
	store_le(buf, 1, 0x1122334455667788);
	CHECK_EQ(load_le64(buf), 0xaaaaaaaaaaaaaa88);

	memset(buf, 0xaa, sizeof(buf));
	store_le(buf, 2, 0x1122334455667788);
	CHECK_EQ(load_le64(buf), 0xaaaaaaaaaaaa7788);

	memset(buf, 0xaa, sizeof(buf));
	store_le(buf, 4, 0x1122334455667788);
	CHECK_EQ(load_le64(buf), 0xaaaaaaaa55667788);

	store_le(buf, 8, 0x1122334455667788);
	CHECK_EQ(load_le64(buf), 0x1122334455667788);
}

int main(void)
{
	RUN_TEST(test_loads);
	RUN_TEST(test_unaligned_load);
	RUN_TEST(test_stores);
	RUN_TEST(test_store_by_size);
	return test_summary();
}
