/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* A read-only block device over the fuzzer's input, for the fuzzers. */

#ifndef TESTS_FUZZ_FUZZ_BLOCK_H
#define TESTS_FUZZ_FUZZ_BLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/block.h"

typedef struct FuzzBlock {
	BlockDevice dev;
	const uint8_t *data;
} FuzzBlock;

static inline bool fuzz_block_read(BlockDevice *b, uint64_t offset, void *buf,
                                   size_t len)
{
	memcpy(buf, ((FuzzBlock *)b)->data + offset, len);
	return true;
}

static inline bool fuzz_block_write(BlockDevice *b, uint64_t offset,
                                    const void *buf, size_t len)
{
	(void)b;
	(void)offset;
	(void)buf;
	(void)len;
	return false;
}

static inline void fuzz_block_destroy(BlockDevice *b)
{
	(void)b;
}

static const BlockDeviceOps fuzz_block_ops = {
	.read = fuzz_block_read,
	.write = fuzz_block_write,
	.destroy = fuzz_block_destroy,
};

static inline BlockDevice *fuzz_block(FuzzBlock *b, const uint8_t *data,
                                      size_t size)
{
	b->dev = (BlockDevice){.ops = &fuzz_block_ops, .size = size};
	b->data = data;
	return &b->dev;
}

#endif /* TESTS_FUZZ_FUZZ_BLOCK_H */
