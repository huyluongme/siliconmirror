/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#ifndef SMIRROR_CPU_JIT_CACHE_H
#define SMIRROR_CPU_JIT_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/bus.h"
#include "cpu/jit/exec_mem.h"
#include "cpu/jit/x64.h"

/*
 * The JIT's translations (docs/architecture.md, "The JIT"): blocks of
 * guest code, each found by the physical and the virtual address of its
 * first instruction and by flags, what else its code depends on, such as
 * the Exception level. Their host code lives in one ExecMem. One cache
 * serves every core of a machine: the code of a block takes the core as
 * its argument.
 *
 * A block's code may depend on the bytes of the 4KB page of RAM that
 * holds its first instruction, and on nothing else in memory: the cache
 * watches that page on the bus, and when the bus writes to it the cache
 * drops the page's blocks. Writes that the bus does not see, through a
 * CPU's own pointers into RAM (bus_ram_pointer()), are the CPU's to keep
 * away from watched pages.
 *
 * Code is written only between blocks, by the translator: it starts a
 * block with jit_cache_begin() and keeps it with jit_cache_add(). When
 * the code memory or the table of blocks is full, the translator empties
 * the cache with jit_cache_flush() and translates again; with a large
 * cache that is rare, and it keeps the cache simple. A flush or a drop
 * only forgets blocks: their code stays in place until a translation
 * after a flush writes over it, so a block that is running when it is
 * forgotten (by a reset from inside it, or by a write to its page) runs
 * to its end.
 *
 * Linux x86-64 hosts only: elsewhere jit_cache_create() fails.
 */

/* The host code of a block, called with the core that runs it. */
typedef void JitCode(void *cpu);

typedef struct JitBlock {
	uint64_t pa;    /* physical address of the first instruction */
	uint64_t va;    /* its virtual address */
	uint32_t flags; /* what else the code depends on */
	uint32_t count; /* guest instructions in the block */
	JitCode *code;
	struct JitBlock *next;      /* the next block in the same hash bucket */
	struct JitBlock *page_next; /* the next block of the same page */
} JitBlock;

/* A page of RAM that blocks were translated from since the last flush. */
typedef struct JitPage {
	uint64_t pa;          /* of the page */
	JitBlock *blocks;     /* its blocks, none after a drop */
	struct JitPage *next; /* the next page in the same hash bucket */
} JitPage;

typedef struct JitCache {
	ExecMem mem;
	Bus *bus;    /* that holds the RAM of the blocks */
	size_t used; /* bytes of mem that hold code */
	JitBlock *blocks;
	size_t num_blocks, max_blocks;
	JitBlock **buckets;
	JitPage *pages; /* max_blocks of them: at most one for each block */
	size_t num_pages;
	JitPage **page_buckets;
	unsigned bucket_bits; /* 1 << bucket_bits buckets of each kind */

	/* Since the cache was created, for statistics and tests. */
	uint64_t translations; /* blocks added */
	uint64_t flushes;
	uint64_t dropped; /* blocks dropped by writes to their pages */
} JitCache;

/*
 * A size for the cache of a machine: 64 MB of code, for about three
 * million guest instructions in phase 3a, and a block for each 128 bytes
 * of it. The host gives memory to the pages only as they are used. A
 * choice of this model, to measure against real workloads.
 */
#define JIT_CACHE_CODE_SIZE ((size_t)64 << 20)
#define JIT_CACHE_BLOCKS ((size_t)1 << 19)

/*
 * A cache of max_blocks blocks (at least 1) and code_size bytes of code,
 * empty, for code in the RAM of bus: it becomes the bus's watcher.
 * Returns NULL, with errno set, when the host has no JIT or memory runs
 * out. Destroy it before the bus.
 */
JitCache *jit_cache_create(Bus *bus, size_t code_size, size_t max_blocks);
void jit_cache_destroy(JitCache *cache);

/* Forget every block. */
void jit_cache_flush(JitCache *cache);

/* The block for (pa, va, flags), or NULL. */
JitBlock *jit_cache_find(const JitCache *cache, uint64_t pa, uint64_t va,
                         uint32_t flags);

/* Start the code of a new block: e writes into the free code memory. */
void jit_cache_begin(JitCache *cache, X64Emitter *e);

/*
 * Keep the block whose code e wrote, from its first byte, for (pa, va,
 * flags) with count instructions, and watch the page of pa. Returns
 * NULL, and keeps nothing, when the code did not fit (e->full), the
 * table of blocks is full, or the page cannot be watched (not all of it
 * is RAM of one region).
 */
JitBlock *jit_cache_add(JitCache *cache, const X64Emitter *e, uint64_t pa,
                        uint64_t va, uint32_t flags, uint32_t count);

#endif /* SMIRROR_CPU_JIT_CACHE_H */
