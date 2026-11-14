/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for src/util/muldiv.h. */

#include <stdint.h>

#include "test.h"
#include "util/muldiv.h"

/* mul_div_floor_ns() gives what mul_div_floor() gives, at the edges too. */
static void test_floor_ns(void)
{
	static const uint64_t hz[] = {
		1,          19200000,   54000000,
		250000000,  999999999,  1000000000,
		1000000001, UINT32_MAX, (uint64_t)UINT32_MAX + 1,
		UINT64_MAX,
	};
	static const uint64_t t[] = {
		0,
		1,
		999999999,
		1000000000,
		1000000001,
		123456789012345,
		(uint64_t)UINT32_MAX * 1000000000u - 1,
		(uint64_t)UINT32_MAX * 1000000000u,
		(uint64_t)UINT32_MAX * 1000000000u + 999999999,
		((uint64_t)UINT32_MAX + 1) * 1000000000u,
		UINT64_MAX,
	};
	uint64_t x = 0x9e3779b97f4a7c15u;

	for (unsigned i = 0; i < sizeof(hz) / sizeof(hz[0]); i++)
		for (unsigned j = 0; j < sizeof(t) / sizeof(t[0]); j++)
			CHECK_EQ(mul_div_floor_ns(t[j], hz[i]),
			         mul_div_floor(t[j], hz[i], 1000000000u));
	/* Random times and clocks, below 2^32 Hz and above. */
	for (unsigned i = 0; i < 1000000; i++) {
		uint64_t a, b;

		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		a = x >> (x & 31);
		b = (x * 0x2545f4914f6cdd1du) >> (32 + (x >> 59));
		CHECK_EQ(mul_div_floor_ns(a, b), mul_div_floor(a, b, 1000000000u));
	}
}

int main(void)
{
	RUN_TEST(test_floor_ns);
	return test_summary();
}
