/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include <stdint.h>

#include "test.h"
#include "util/bits.h"

static void test_field32(void)
{
	CHECK_EQ(field32(0x12345678, 7, 0), 0x78);
	CHECK_EQ(field32(0x12345678, 31, 28), 0x1);
	CHECK_EQ(field32(0x12345678, 15, 4), 0x567);
	CHECK_EQ(field32(0x80000000, 31, 31), 1);
	CHECK_EQ(field32(0xdeadbeef, 31, 0), 0xdeadbeef);
	CHECK_EQ(bit32(0x00000004, 2), 1);
	CHECK_EQ(bit32(0x00000004, 3), 0);
}

static void test_sign_extend64(void)
{
	CHECK_EQ(sign_extend64(0x0fffff, 21), 0x0fffff);
	CHECK_EQ(sign_extend64(0x100000, 21), 0xfffffffffff00000);
	CHECK_EQ(sign_extend64(0x1fffff, 21), UINT64_MAX);
	CHECK_EQ(sign_extend64(0, 1), 0);
	CHECK_EQ(sign_extend64(1, 1), UINT64_MAX);
	CHECK_EQ(sign_extend64(0x80000000, 32), 0xffffffff80000000);
	CHECK_EQ(sign_extend64(0x8000000000000000, 64), 0x8000000000000000);

	/* Bits above the field do not matter. */
	CHECK_EQ(sign_extend64(0xff7f, 8), 0x7f);
	CHECK_EQ(sign_extend64(0x0080, 8), 0xffffffffffffff80);
}

int main(void)
{
	RUN_TEST(test_field32);
	RUN_TEST(test_sign_extend64);
	return test_summary();
}
