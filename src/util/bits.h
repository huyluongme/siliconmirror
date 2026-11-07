/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Bit field helpers for instruction decoders. Bit numbers count from 0,
 * the least significant bit, as in the architecture manuals.
 */

#ifndef UTIL_BITS_H
#define UTIL_BITS_H

#include <assert.h>
#include <stdint.h>

/* Bits hi to lo of value, what the Arm manuals write as value<hi:lo>. */
static inline uint32_t field32(uint32_t value, unsigned hi, unsigned lo)
{
	assert(lo <= hi && hi < 32);
	return (value >> lo) & (((uint32_t)2 << (hi - lo)) - 1);
}

/* Bit n of value. */
static inline uint32_t bit32(uint32_t value, unsigned n)
{
	return field32(value, n, n);
}

/*
 * Sign-extend the low width bits of value to 64 bits: bit width - 1 is
 * the sign bit, and the bits above it are ignored.
 */
static inline uint64_t sign_extend64(uint64_t value, unsigned width)
{
	assert(width >= 1 && width <= 64);

	uint64_t sign = (uint64_t)1 << (width - 1);

	value &= (sign << 1) - 1; /* all ones when width is 64 */
	return (value ^ sign) - sign;
}

#endif /* UTIL_BITS_H */
