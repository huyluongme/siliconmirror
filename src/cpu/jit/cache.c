/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "cpu/jit/cache.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Each block's code starts at a multiple of 16 bytes, where the host
 * fetches it fastest (Intel's optimization manual aligns branch targets
 * so; general knowledge).
 */
#define CODE_ALIGN 16

/* Fibonacci hashing: the top bits of pa / 4 times 2^64 / phi. */
static size_t bucket_of(const JitCache *cache, uint64_t pa)
{
	return (size_t)(((pa >> 2) * UINT64_C(0x9e3779b97f4a7c15)) >>
	                (64 - cache->bucket_bits));
}

JitCache *jit_cache_create(size_t code_size, size_t max_blocks)
{
#if defined(__x86_64__)
	JitCache *cache;
	int saved;

	if (max_blocks == 0) {
		errno = EINVAL;
		return NULL;
	}
	cache = calloc(1, sizeof(*cache));
	if (cache == NULL)
		return NULL;
	cache->blocks = calloc(max_blocks, sizeof(*cache->blocks));
	if (cache->blocks == NULL)
		goto fail;
	cache->max_blocks = max_blocks;

	/* At least as many buckets as blocks, and at least 2. */
	cache->bucket_bits = 1;
	while (((size_t)1 << cache->bucket_bits) < max_blocks)
		cache->bucket_bits++;
	cache->buckets =
		calloc((size_t)1 << cache->bucket_bits, sizeof(*cache->buckets));
	if (cache->buckets == NULL || !exec_mem_init(&cache->mem, code_size))
		goto fail;
	return cache;

fail:
	saved = errno;
	jit_cache_destroy(cache);
	errno = saved;
	return NULL;
#else
	(void)code_size;
	(void)max_blocks;
	errno = ENOSYS; /* the emitter writes x86-64 code */
	return NULL;
#endif
}

void jit_cache_destroy(JitCache *cache)
{
	if (cache == NULL)
		return;
	exec_mem_free(&cache->mem);
	free(cache->buckets);
	free(cache->blocks);
	free(cache);
}

void jit_cache_flush(JitCache *cache)
{
	memset(cache->buckets, 0,
	       ((size_t)1 << cache->bucket_bits) * sizeof(*cache->buckets));
	cache->num_blocks = 0;
	cache->used = 0;
	cache->flushes++;
}

JitBlock *jit_cache_find(const JitCache *cache, uint64_t pa, uint64_t va,
                         uint32_t flags)
{
	for (JitBlock *b = cache->buckets[bucket_of(cache, pa)]; b != NULL;
	     b = b->next)
		if (b->pa == pa && b->va == va && b->flags == flags)
			return b;
	return NULL;
}

void jit_cache_begin(JitCache *cache, X64Emitter *e)
{
	/* used <= mem.size, a multiple of the host's page size: start fits. */
	size_t start = (cache->used + CODE_ALIGN - 1) & ~(size_t)(CODE_ALIGN - 1);

	x64_init(e, cache->mem.write + start, (uintptr_t)cache->mem.run + start,
	         cache->mem.size - start);
}

JitBlock *jit_cache_add(JitCache *cache, const X64Emitter *e, uint64_t pa,
                        uint64_t va, uint32_t flags, uint32_t count)
{
	JitBlock *b, **bucket;

	if (e->full || cache->num_blocks == cache->max_blocks)
		return NULL;
	b = &cache->blocks[cache->num_blocks++];
	bucket = &cache->buckets[bucket_of(cache, pa)];
	*b = (JitBlock){
		.pa = pa,
		.va = va,
		.flags = flags,
		.count = count,
		.code = (JitCode *)e->run_base,
		.next = *bucket,
	};
	*bucket = b;
	cache->used = (size_t)(e->buf - cache->mem.write) + e->pos;
	cache->translations++;
	return b;
}
