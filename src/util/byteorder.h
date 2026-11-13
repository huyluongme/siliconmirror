/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Little-endian and big-endian loads and stores.
 *
 * They read and write bytes one at a time, so they work on any host byte
 * order and at any alignment. Compilers turn each of them into a single
 * load or store on little-endian hosts, plus a byte swap for the
 * big-endian ones.
 */

#ifndef UTIL_BYTEORDER_H
#define UTIL_BYTEORDER_H

#include <stdint.h>

static inline uint16_t load_le16(const void *ptr)
{
	const uint8_t *p = ptr;

	return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static inline uint32_t load_le32(const void *ptr)
{
	const uint8_t *p = ptr;

	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline uint64_t load_le64(const void *ptr)
{
	const uint8_t *p = ptr;

	return (uint64_t)load_le32(p) | (uint64_t)load_le32(p + 4) << 32;
}

static inline void store_le16(void *ptr, uint16_t value)
{
	uint8_t *p = ptr;

	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static inline void store_le32(void *ptr, uint32_t value)
{
	uint8_t *p = ptr;

	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static inline void store_le64(void *ptr, uint64_t value)
{
	uint8_t *p = ptr;

	store_le32(p, (uint32_t)value);
	store_le32(p + 4, (uint32_t)(value >> 32));
}

/* Load a value of size 1, 2, 4 or 8 bytes. */
static inline uint64_t load_le(const void *ptr, unsigned size)
{
	switch (size) {
	case 1:
		return *(const uint8_t *)ptr;
	case 2:
		return load_le16(ptr);
	case 4:
		return load_le32(ptr);
	default:
		return load_le64(ptr);
	}
}

/* Store the low size bytes of value; size is 1, 2, 4 or 8. */
static inline void store_le(void *ptr, unsigned size, uint64_t value)
{
	switch (size) {
	case 1:
		*(uint8_t *)ptr = (uint8_t)value;
		break;
	case 2:
		store_le16(ptr, (uint16_t)value);
		break;
	case 4:
		store_le32(ptr, (uint32_t)value);
		break;
	default:
		store_le64(ptr, value);
		break;
	}
}

/* Big-endian, as in devicetree blobs. */
static inline uint32_t load_be32(const void *ptr)
{
	const uint8_t *p = ptr;

	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
	       (uint32_t)p[3];
}

static inline uint64_t load_be64(const void *ptr)
{
	const uint8_t *p = ptr;

	return (uint64_t)load_be32(p) << 32 | (uint64_t)load_be32(p + 4);
}

static inline void store_be32(void *ptr, uint32_t value)
{
	uint8_t *p = ptr;

	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

static inline void store_be64(void *ptr, uint64_t value)
{
	uint8_t *p = ptr;

	store_be32(p, (uint32_t)(value >> 32));
	store_be32(p + 4, (uint32_t)value);
}

#endif /* UTIL_BYTEORDER_H */
