/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "cpu/jit/cache.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* The JIT's cache of blocks: keys, code memory, a full cache, a flush. */

#if defined(__linux__) && defined(__x86_64__)

/* The mappings of the code memory's memfd that the process has. */
static unsigned count_maps(void)
{
	char line[512];
	unsigned n = 0;
	FILE *maps = fopen("/proc/self/maps", "r");

	if (maps == NULL)
		return 0;
	while (fgets(line, sizeof line, maps) != NULL)
		n += strstr(line, "smirror-jit") != NULL;
	fclose(maps);
	return n;
}

/* A block whose code stores value at the address it is called with. */
static JitBlock *add(JitCache *cache, uint64_t pa, uint64_t va, uint32_t flags,
                     int32_t value)
{
	X64Emitter e;

	jit_cache_begin(cache, &e);
	x64_store_imm(&e, X64_32, X64_RDI, 0, value);
	x64_ret(&e);
	return jit_cache_add(cache, &e, pa, va, flags, 3);
}

static EXEC_MEM_CALLER int32_t run(const JitBlock *b)
{
	int32_t value = 0;

	b->code(&value);
	return value;
}

static void test_create(void)
{
	JitCache *cache;

	errno = 0;
	CHECK(jit_cache_create(4096, 0) == NULL);
	CHECK_EQ(errno, EINVAL);
	errno = 0;
	CHECK(jit_cache_create(0, 4) == NULL); /* no code memory */
	CHECK(errno != 0);

	/*
	 * No file descriptor left for the memfd: the error of the code
	 * memory, which freeing the rest of the cache keeps.
	 */
	struct rlimit saved, limit;
	int fd = open("/dev/null", O_RDONLY);

	CHECK(fd >= 0);
	close(fd);
	CHECK(getrlimit(RLIMIT_NOFILE, &saved) == 0);
	limit = saved;
	limit.rlim_cur = (rlim_t)fd;
	CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);
	errno = 0;
	cache = jit_cache_create(4096, 4);
	CHECK(setrlimit(RLIMIT_NOFILE, &saved) == 0);
	CHECK(cache == NULL);
	CHECK_EQ(errno, EMFILE);

	cache = jit_cache_create(4096, 1);
	CHECK(cache != NULL);
	CHECK_EQ(count_maps(), 2);       /* writable and executable */
	CHECK_EQ(cache->bucket_bits, 1); /* at least 2 buckets */
	CHECK_EQ(cache->max_blocks, 1);
	CHECK_EQ(cache->num_blocks, 0);
	CHECK_EQ(cache->used, 0);
	CHECK_EQ(cache->translations, 0);
	CHECK_EQ(cache->flushes, 0);
	jit_cache_destroy(cache);
	CHECK_EQ(count_maps(), 0);

	cache = jit_cache_create(4096, 5);
	CHECK_EQ(cache->bucket_bits, 3); /* 8 buckets for 5 blocks */
	jit_cache_destroy(cache);
	cache = jit_cache_create(4096, 8);
	CHECK_EQ(cache->bucket_bits, 3);
	jit_cache_destroy(cache);

	jit_cache_destroy(NULL);
}

/* A block is found by its physical and virtual address and its flags. */
static void test_find(void)
{
	JitCache *cache = jit_cache_create(4096, 8);
	JitBlock *a, *b, *c;

	CHECK(jit_cache_find(cache, 0x80000, 0x80000, 3) == NULL);
	a = add(cache, 0x80000, 0x80000, 3, 11);
	b = add(cache, 0x80000, 0xffff000000080000, 3, 22);
	c = add(cache, 0x80000, 0x80000, 1, 33);
	CHECK(a != NULL && b != NULL && c != NULL);
	CHECK(jit_cache_find(cache, 0x80000, 0x80000, 3) == a);
	CHECK(jit_cache_find(cache, 0x80000, 0xffff000000080000, 3) == b);
	CHECK(jit_cache_find(cache, 0x80000, 0x80000, 1) == c);
	CHECK(jit_cache_find(cache, 0x80000, 0x80000, 2) == NULL);
	CHECK(jit_cache_find(cache, 0x80004, 0x80000, 3) == NULL);
	CHECK(jit_cache_find(cache, 0x80000, 0x80004, 3) == NULL);
	CHECK_EQ(a->pa, 0x80000);
	CHECK_EQ(b->va, 0xffff000000080000);
	CHECK_EQ(c->flags, 1);
	CHECK_EQ(a->count, 3);
	CHECK_EQ(run(a), 11);
	CHECK_EQ(run(b), 22);
	CHECK_EQ(run(c), 33);
	CHECK_EQ(cache->translations, 3);
	CHECK_EQ(cache->num_blocks, 3);

	/* Many addresses: each block in its bucket, the buckets in range. */
	jit_cache_destroy(cache);
	cache = jit_cache_create(65536, 1024);
	for (uint32_t i = 0; i < 1024; i++)
		CHECK(add(cache, (uint64_t)i * 4096 + 4 * (i % 1024), i, 0,
		          (int32_t)i) != NULL);
	for (uint32_t i = 0; i < 1024; i += 97) {
		JitBlock *found =
			jit_cache_find(cache, (uint64_t)i * 4096 + 4 * (i % 1024), i, 0);

		CHECK(found != NULL && run(found) == (int32_t)i);
	}
	jit_cache_destroy(cache);
}

/* Code starts at a multiple of 16 bytes, after the code before it. */
static void test_code(void)
{
	JitCache *cache = jit_cache_create(4096, 8);
	X64Emitter e;
	JitBlock *a, *b;

	jit_cache_begin(cache, &e);
	CHECK(e.buf == cache->mem.write);
	CHECK_EQ(e.run_base, (uintptr_t)cache->mem.run);
	CHECK_EQ(e.size, cache->mem.size);
	a = add(cache, 0, 0, 0, 1); /* 7 bytes: C7 07 imm32, C3 */
	CHECK_EQ(cache->used, 7);
	b = add(cache, 4, 4, 0, 2);
	CHECK((uintptr_t)a->code == (uintptr_t)cache->mem.run);
	CHECK((uintptr_t)b->code == (uintptr_t)cache->mem.run + 16);
	CHECK_EQ(cache->used, 23);
	jit_cache_begin(cache, &e);
	CHECK(e.buf == cache->mem.write + 32);
	CHECK_EQ(e.size, cache->mem.size - 32);
	CHECK_EQ(run(a), 1);
	CHECK_EQ(run(b), 2);

	/* After a block of one byte, the next starts 15 bytes later. */
	x64_ret(&e);
	CHECK(jit_cache_add(cache, &e, 8, 8, 0, 1) != NULL);
	CHECK_EQ(cache->used, 33);
	jit_cache_begin(cache, &e);
	CHECK(e.buf == cache->mem.write + 48);
	jit_cache_destroy(cache);
}

/* A full cache keeps nothing more; a flush empties it. */
static void test_full(void)
{
	JitCache *cache = jit_cache_create(4096, 2);
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	X64Emitter e;
	JitBlock *a;

	a = add(cache, 0, 0, 0, 1);
	CHECK(add(cache, 4, 4, 0, 2) != NULL);
	CHECK(add(cache, 8, 8, 0, 3) == NULL); /* the table is full */
	CHECK_EQ(cache->num_blocks, 2);
	CHECK_EQ(cache->translations, 2);
	CHECK(jit_cache_find(cache, 8, 8, 0) == NULL);
	CHECK(jit_cache_find(cache, 0, 0, 0) == a);

	jit_cache_flush(cache);
	CHECK_EQ(cache->flushes, 1);
	CHECK_EQ(cache->num_blocks, 0);
	CHECK_EQ(cache->used, 0);
	CHECK(jit_cache_find(cache, 0, 0, 0) == NULL);
	CHECK(jit_cache_find(cache, 4, 4, 0) == NULL);

	/* Code that does not fit in what is left of the code memory. */
	jit_cache_begin(cache, &e);
	for (size_t i = 0; i + 1 < page; i++)
		x64_ret(&e);
	CHECK(jit_cache_add(cache, &e, 0, 0, 0, 1) != NULL);
	CHECK_EQ(cache->used, page - 1);
	jit_cache_begin(cache, &e);
	CHECK_EQ(e.size, 0);
	x64_ret(&e);
	CHECK(e.full);
	CHECK(jit_cache_add(cache, &e, 4, 4, 0, 1) == NULL);
	CHECK_EQ(cache->num_blocks, 1);
	CHECK_EQ(cache->used, page - 1);
	CHECK(jit_cache_find(cache, 4, 4, 0) == NULL);
	jit_cache_destroy(cache);
}

int main(void)
{
	RUN_TEST(test_create);
	RUN_TEST(test_find);
	RUN_TEST(test_code);
	RUN_TEST(test_full);
	return test_summary();
}

#else

int main(void)
{
	errno = 0;
	CHECK(jit_cache_create(4096, 8) == NULL);
	CHECK_EQ(errno, ENOSYS);
	puts("SKIP: the JIT needs a Linux x86-64 host");
	return test_summary();
}

#endif
