/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A block device in memory, for the unit tests: a disk image that the
 * test fills in, and that can fail its reads of a range, or its read
 * number n.
 */

#ifndef TESTS_UNIT_BLOCK_MEM_H
#define TESTS_UNIT_BLOCK_MEM_H

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/block.h"

typedef struct BlockMem {
	BlockDevice dev;
	uint8_t *data;
	uint64_t fail_from, fail_to; /* reads that touch this range fail */
	unsigned reads;              /* how many so far */
	unsigned fail_read;          /* the read that fails, counting from 0 */
} BlockMem;

static inline bool block_mem_read(BlockDevice *b, uint64_t offset, void *buf,
                                  size_t len)
{
	BlockMem *m = (BlockMem *)b;

	if (m->reads++ == m->fail_read)
		return false;
	if (offset + len > m->fail_from && offset < m->fail_to)
		return false;
	memcpy(buf, m->data + offset, len);
	return true;
}

static inline bool block_mem_write(BlockDevice *b, uint64_t offset,
                                   const void *buf, size_t len)
{
	BlockMem *m = (BlockMem *)b;

	memcpy(m->data + offset, buf, len);
	return true;
}

static inline void block_mem_destroy(BlockDevice *b)
{
	(void)b;
}

static const BlockDeviceOps block_mem_ops = {
	.read = block_mem_read,
	.write = block_mem_write,
	.destroy = block_mem_destroy,
};

/* A zeroed disk of size bytes. */
static inline void block_mem_init(BlockMem *m, uint64_t size)
{
	m->dev = (BlockDevice){.ops = &block_mem_ops, .size = size};
	m->data = calloc(1, size);
	m->fail_from = UINT64_MAX;
	m->fail_to = UINT64_MAX;
	m->reads = 0;
	m->fail_read = UINT_MAX;
}

static inline void block_mem_free(BlockMem *m)
{
	free(m->data);
}

#endif /* TESTS_UNIT_BLOCK_MEM_H */
