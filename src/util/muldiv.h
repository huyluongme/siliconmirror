/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * a * b / c on 64-bit values without losing the high bits of the
 * product: it is kept in 128 bits. Clock conversions need this, for
 * example from nanoseconds to the ticks of a 19.2 MHz crystal.
 *
 * GCC and Clang provide unsigned __int128 on 64-bit hosts; __extension__
 * keeps -Wpedantic quiet about it.
 */

#ifndef UTIL_MULDIV_H
#define UTIL_MULDIV_H

#include <stdint.h>

#ifndef __SIZEOF_INT128__
#error "a compiler with unsigned __int128 is required"
#endif

__extension__ typedef unsigned __int128 uint128_t;

/* floor(a * b / c), or UINT64_MAX when it does not fit. c is not 0. */
static inline uint64_t mul_div_floor(uint64_t a, uint64_t b, uint64_t c)
{
	uint128_t q = (uint128_t)a * b / c;

	return q > UINT64_MAX ? UINT64_MAX : (uint64_t)q;
}

/* ceil(a * b / c), or UINT64_MAX when it does not fit. c is not 0. */
static inline uint64_t mul_div_ceil(uint64_t a, uint64_t b, uint64_t c)
{
	uint128_t q = ((uint128_t)a * b + c - 1) / c;

	return q > UINT64_MAX ? UINT64_MAX : (uint64_t)q;
}

/*
 * floor(t * hz / 10^9), the edges of a clock of hz Hz in t nanoseconds,
 * as mul_div_floor() gives them. With t = q 10^9 + r, it is
 * q hz + floor(r hz / 10^9): 64-bit arithmetic is enough while hz and q
 * are below 2^32 (136 years), and the divisions by the constant become
 * multiplications, far cheaper than the 128-bit division.
 */
static inline uint64_t mul_div_floor_ns(uint64_t t, uint64_t hz)
{
	uint64_t q = t / 1000000000u, r = t % 1000000000u;

	if (hz > UINT32_MAX || q > UINT32_MAX)
		return mul_div_floor(t, hz, 1000000000u);
	return q * hz + r * hz / 1000000000u;
}

#endif /* UTIL_MULDIV_H */
