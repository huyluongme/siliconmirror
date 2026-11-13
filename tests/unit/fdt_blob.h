/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Devicetree blobs built token by token for the unit tests, as chapter 5
 * of the Devicetree Specification v0.4 lays them out.
 */

#ifndef TESTS_UNIT_FDT_BLOB_H
#define TESTS_UNIT_FDT_BLOB_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "util/byteorder.h"
#include "util/fdt.h"

/* A blob under construction: the structure and strings blocks. */
typedef struct Builder {
	uint8_t structure[1024];
	size_t struct_len;
	char strings[256];
	size_t strings_len;
	uint64_t reservations[4][2];
	size_t num_reservations;
} Builder;

static inline void word(Builder *b, uint32_t value)
{
	store_be32(b->structure + b->struct_len, value);
	b->struct_len += 4;
}

static inline void bytes(Builder *b, const void *data, size_t len)
{
	memcpy(b->structure + b->struct_len, data, len);
	b->struct_len += (len + 3) & ~(size_t)3; /* zero padding */
}

static inline void begin(Builder *b, const char *name)
{
	word(b, 1);
	bytes(b, name, strlen(name) + 1);
}

static inline void end(Builder *b)
{
	word(b, 2);
}

/* A property; its name goes in the strings block, once. */
static inline void prop(Builder *b, const char *name, const void *value,
                        size_t len)
{
	size_t off = 0;

	while (off < b->strings_len && strcmp(b->strings + off, name) != 0)
		off += strlen(b->strings + off) + 1;
	if (off == b->strings_len) {
		memcpy(b->strings + off, name, strlen(name) + 1);
		b->strings_len += strlen(name) + 1;
	}
	word(b, 3);
	word(b, (uint32_t)len);
	word(b, (uint32_t)off);
	bytes(b, value, len);
}

static inline void prop_u32(Builder *b, const char *name, uint32_t value)
{
	uint8_t be[4];

	store_be32(be, value);
	prop(b, name, be, 4);
}

/*
 * The blob, in the order fdt_write() uses: header, memory reservations,
 * structure, strings. Returns its size; the caller frees *blob.
 */
static inline size_t finish(Builder *b, uint8_t **blob)
{
	size_t rsv = 40, structure = rsv + 16 * (b->num_reservations + 1);
	size_t strings = structure + b->struct_len + 4;
	size_t total = strings + b->strings_len;
	uint8_t *out = calloc(1, total);

	CHECK(out != NULL);
	word(b, 9); /* FDT_END */
	store_be32(out, FDT_MAGIC);
	store_be32(out + 4, (uint32_t)total);
	store_be32(out + 8, (uint32_t)structure);
	store_be32(out + 12, (uint32_t)strings);
	store_be32(out + 16, (uint32_t)rsv);
	store_be32(out + 20, 17);
	store_be32(out + 24, 16);
	store_be32(out + 28, 2); /* boot_cpuid_phys */
	store_be32(out + 32, (uint32_t)b->strings_len);
	store_be32(out + 36, (uint32_t)b->struct_len);
	for (size_t i = 0; i < b->num_reservations; i++) {
		store_be64(out + rsv + 16 * i, b->reservations[i][0]);
		store_be64(out + rsv + 16 * i + 8, b->reservations[i][1]);
	}
	memcpy(out + structure, b->structure, b->struct_len);
	memcpy(out + strings, b->strings, b->strings_len);
	*blob = out;
	return total;
}

#endif /* TESTS_UNIT_FDT_BLOB_H */
