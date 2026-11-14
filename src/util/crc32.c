/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "util/crc32.h"

#include <stdbool.h>

#define POLYNOMIAL 0xedb88320u /* 0x04C11DB7, its bits reversed */

/* The CRC of each byte value, made on first use. */
static uint32_t table[256];
static bool table_made;

static void make_table(void)
{
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;

		for (int k = 0; k < 8; k++)
			c = c & 1 ? POLYNOMIAL ^ (c >> 1) : c >> 1;
		table[n] = c;
	}
	table_made = true;
}

uint32_t crc32(const void *data, size_t size)
{
	const uint8_t *p = data;
	uint32_t c = 0xffffffffu;

	if (!table_made)
		make_table();
	for (size_t i = 0; i < size; i++)
		c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}
