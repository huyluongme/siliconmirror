/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "cpu/jit/cache.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/*
 * The JIT's cache of blocks: keys, code memory, a full cache, a flush,
 * the blocks that a write to their page drops.
 */

/* The bus of the caches: RAM from 0 to 8 MB, nothing above. */
static Bus *bus;

#define RAM_SIZE 0x800000

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
	CHECK(jit_cache_create(bus, 4096, 0) == NULL);
	CHECK_EQ(errno, EINVAL);
	errno = 0;
	CHECK(jit_cache_create(bus, 0, 4) == NULL); /* no code memory */
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
	cache = jit_cache_create(bus, 4096, 4);
	CHECK(setrlimit(RLIMIT_NOFILE, &saved) == 0);
	CHECK(cache == NULL);
	CHECK_EQ(errno, EMFILE);

	cache = jit_cache_create(bus, 4096, 1);
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

	cache = jit_cache_create(bus, 4096, 5);
	CHECK_EQ(cache->bucket_bits, 3); /* 8 buckets for 5 blocks */
	jit_cache_destroy(cache);
	cache = jit_cache_create(bus, 4096, 8);
	CHECK_EQ(cache->bucket_bits, 3);
	jit_cache_destroy(cache);

	jit_cache_destroy(NULL);
}

/* A block is found by its physical and virtual address and its flags. */
static void test_find(void)
{
	JitCache *cache = jit_cache_create(bus, 4096, 8);
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
	cache = jit_cache_create(bus, 65536, 1024);
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
	JitCache *cache = jit_cache_create(bus, 4096, 8);
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
	JitCache *cache = jit_cache_create(bus, 4096, 2);
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	X64Emitter e;
	JitBlock *a;

	a = add(cache, 0, 0, 0, 1);
	CHECK(add(cache, 4, 4, 0, 2) != NULL);
	CHECK(add(cache, 8, 8, 0, 3) == NULL); /* the table is full */
	CHECK(add(cache, 0x500000, 0, 0, 3) == NULL);
	CHECK(!bus_ram_watched(bus, 0x500000)); /* nothing kept */
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
	CHECK(jit_cache_add(cache, &e, 0x501000, 4, 0, 1) == NULL);
	CHECK(!bus_ram_watched(bus, 0x501000));
	CHECK_EQ(cache->num_blocks, 1);
	CHECK_EQ(cache->used, page - 1);
	CHECK(jit_cache_find(cache, 0x501000, 4, 0) == NULL);
	jit_cache_destroy(cache);
}

/* The address of the n-th of the blocks of test_drop(): 16 in a page. */
static uint64_t drop_pa(uint32_t n)
{
	return 0x600000 + (uint64_t)(n / 16) * 0x1000 + 4 * (n % 16) * 17;
}

/*
 * A write that the bus makes to a page drops the page's blocks, and no
 * others. The page is watched again when a block of it is added.
 */
static void test_drop(void)
{
	JitCache *cache = jit_cache_create(bus, 4096, 70);
	JitBlock *a;

	for (uint32_t n = 0; n < 64; n++)
		CHECK(add(cache, drop_pa(n), n, n % 3, (int32_t)n) != NULL);
	CHECK_EQ(cache->num_pages, 4);
	for (uint64_t pa = 0x600000; pa < 0x604000; pa += 0x1000)
		CHECK(bus_ram_watched(bus, pa));
	CHECK(!bus_ram_watched(bus, 0x604000));

	/* Pages 1 and 2. */
	CHECK_EQ(bus_write(bus, 0x6017fc, 4, 0), BUS_OK);
	CHECK_EQ(cache->dropped, 16);
	CHECK_EQ(bus_write(bus, 0x602000, 1, 0), BUS_OK);
	CHECK_EQ(cache->dropped, 32);
	for (uint32_t n = 0; n < 64; n++) {
		JitBlock *b = jit_cache_find(cache, drop_pa(n), n, n % 3);

		if (n >= 16 && n < 48)
			CHECK(b == NULL);
		else
			CHECK(b != NULL && run(b) == (int32_t)n);
	}
	CHECK(!bus_ram_watched(bus, 0x601000));
	CHECK(bus_ram_watched(bus, 0x603000));
	CHECK_EQ(bus_write(bus, 0x601000, 4, 0), BUS_OK); /* not watched */
	CHECK_EQ(cache->dropped, 32);

	/* A page watched again: its blocks from now on. */
	a = add(cache, 0x601000, 0, 0, 99);
	CHECK(a != NULL);
	CHECK(bus_ram_watched(bus, 0x601000));
	CHECK_EQ(cache->num_pages, 4);
	CHECK(jit_cache_find(cache, 0x601000, 0, 0) == a);
	CHECK_EQ(bus_write(bus, 0x601ff8, 8, 0), BUS_OK);
	CHECK(jit_cache_find(cache, 0x601000, 0, 0) == NULL);
	CHECK_EQ(cache->dropped, 33);

	/* A watched page without blocks; one that is not RAM. */
	CHECK(bus_watch_ram(bus, 0x605000));
	CHECK_EQ(bus_write(bus, 0x605000, 4, 0), BUS_OK);
	CHECK_EQ(cache->dropped, 33);
	CHECK(add(cache, RAM_SIZE, 0, 0, 1) == NULL);
	CHECK(add(cache, RAM_SIZE - 0x1000, 0, 0, 1) != NULL);
	CHECK_EQ(cache->num_blocks, 66);
	CHECK_EQ(cache->translations, 66);
	jit_cache_destroy(cache);

	/* The watcher goes with the cache: another one can be made. */
	CHECK_EQ(bus_write(bus, 0x600000, 4, 0), BUS_OK);
	cache = jit_cache_create(bus, 4096, 1);
	CHECK(cache != NULL);
	jit_cache_destroy(cache);
}

/*
 * Blocks of three pages in the chain of one bucket (4 buckets; the
 * addresses chosen for it): dropping those of the middle page, at the
 * head and in the middle of the chain, keeps the others in it.
 */
static void test_drop_chain(void)
{
	JitCache *cache = jit_cache_create(bus, 4096, 4);
	JitBlock *a1 = add(cache, 0x600000, 0, 0, 1);
	JitBlock *b1 = add(cache, 0x601014, 0, 0, 2);
	JitBlock *c1 = add(cache, 0x602008, 0, 0, 3);
	JitBlock *b2 = add(cache, 0x601028, 0, 0, 4);

	CHECK(a1 && b1 && c1 && b2);
	CHECK(b2->next == c1 && c1->next == b1 && b1->next == a1);
	CHECK(a1->next == NULL);
	CHECK_EQ(bus_write(bus, 0x601000, 4, 0), BUS_OK);
	CHECK_EQ(cache->dropped, 2);
	CHECK(jit_cache_find(cache, 0x601014, 0, 0) == NULL);
	CHECK(jit_cache_find(cache, 0x601028, 0, 0) == NULL);
	CHECK(jit_cache_find(cache, 0x600000, 0, 0) == a1);
	CHECK(jit_cache_find(cache, 0x602008, 0, 0) == c1);
	CHECK(c1->next == a1);
	jit_cache_destroy(cache);
}

/* After a flush, the pages are those of the blocks added since. */
static void test_drop_flush(void)
{
	JitCache *cache = jit_cache_create(bus, 4096, 3);

	CHECK(add(cache, 0x700000, 0, 0, 1) != NULL);
	CHECK(add(cache, 0x701000, 0, 0, 2) != NULL);
	CHECK(add(cache, 0x702000, 0, 0, 3) != NULL);
	CHECK_EQ(cache->num_pages, 3);
	jit_cache_flush(cache);
	CHECK_EQ(cache->num_pages, 0);
	CHECK_EQ(bus_write(bus, 0x702000, 4, 0), BUS_OK); /* none to drop */
	CHECK_EQ(cache->dropped, 0);

	/* The first page again, then two others in its entry and after. */
	CHECK(add(cache, 0x700000, 0, 0, 4) != NULL);
	CHECK(add(cache, 0x703000, 0, 0, 5) != NULL);
	CHECK(add(cache, 0x704000, 0, 0, 6) != NULL);
	CHECK_EQ(cache->num_pages, 3);
	CHECK_EQ(bus_write(bus, 0x700000, 4, 0), BUS_OK);
	CHECK_EQ(cache->dropped, 1);
	CHECK(jit_cache_find(cache, 0x700000, 0, 0) == NULL);
	CHECK(jit_cache_find(cache, 0x703000, 0, 0) != NULL);
	CHECK(jit_cache_find(cache, 0x704000, 0, 0) != NULL);
	jit_cache_destroy(cache);
}

int main(void)
{
	alarm(60); /* a mutant may loop in a chain of blocks */
	bus = bus_create();
	CHECK_EQ(bus_map_ram(bus, "ram", 0, RAM_SIZE), BUS_OK);
	RUN_TEST(test_create);
	RUN_TEST(test_find);
	RUN_TEST(test_code);
	RUN_TEST(test_full);
	RUN_TEST(test_drop);
	RUN_TEST(test_drop_chain);
	RUN_TEST(test_drop_flush);
	bus_destroy(bus);
	return test_summary();
}

#else

int main(void)
{
	bus = bus_create();
	errno = 0;
	CHECK(jit_cache_create(bus, 4096, 8) == NULL);
	CHECK_EQ(errno, ENOSYS);
	bus_destroy(bus);
	puts("SKIP: the JIT needs a Linux x86-64 host");
	return test_summary();
}

#endif
