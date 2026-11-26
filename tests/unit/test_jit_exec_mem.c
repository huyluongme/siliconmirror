/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#define _POSIX_C_SOURCE 200809L /* sysconf() */

#include "cpu/jit/exec_mem.h"
#include "test.h"

#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* The JIT's executable memory: two views of the same pages. */

#ifdef __linux__

static size_t page;

/* The permissions /proc/self/maps gives the mapping holding addr, as
 * "rwxs"/"rwxp" with '-' for what is missing, or "" if none. */
static const char *perms(const void *addr)
{
	static char found[5];
	char line[512];
	FILE *maps = fopen("/proc/self/maps", "r");

	found[0] = 0;
	if (maps == NULL)
		return found;
	while (fgets(line, sizeof line, maps) != NULL) {
		unsigned long start, end;
		char p[5];

		if (sscanf(line, "%lx-%lx %4s", &start, &end, p) == 3 &&
		    (uintptr_t)addr >= start && (uintptr_t)addr < end) {
			memcpy(found, p, sizeof found);
			break;
		}
	}
	fclose(maps);
	return found;
}

/* The number of mappings in /proc/self/maps. */
static int count_maps(void)
{
	char line[512];
	FILE *maps = fopen("/proc/self/maps", "r");
	int n = 0;

	if (maps == NULL)
		return -1;
	while (fgets(line, sizeof line, maps) != NULL)
		n++;
	fclose(maps);
	return n;
}

/* The lowest free file descriptor: a file left open takes it. */
static int lowest_fd(void)
{
	int fd = dup(0);

	close(fd);
	return fd;
}

static void test_views(void)
{
	ExecMem mem;
	int fd = lowest_fd();

	CHECK(exec_mem_init(&mem, 100));
	CHECK_EQ(lowest_fd(), fd);
	CHECK_EQ(mem.size, page);
	CHECK(mem.write != NULL);
	CHECK(mem.run != NULL);
	CHECK(mem.write != mem.run);

	/* W^X: one view writable, the other executable, both shared. */
	CHECK(strcmp(perms(mem.write), "rw-s") == 0);
	CHECK(strcmp(perms(mem.run), "r-xs") == 0);

	/* Zeroed, and a write shows through the other view at once. */
	CHECK_EQ(mem.run[0], 0);
	CHECK_EQ(mem.run[page - 1], 0);
	const uint8_t *write = mem.write;
	const uint8_t *run = mem.run;
	mem.write[0] = 0x5A;
	mem.write[page - 1] = 0xA5;
	CHECK_EQ(mem.run[0], 0x5A);
	CHECK_EQ(mem.run[page - 1], 0xA5);
	exec_mem_free(&mem);
	CHECK(mem.write == NULL);
	CHECK(mem.run == NULL);
	CHECK_EQ(mem.size, 0);
	CHECK(strcmp(perms(write), "") == 0);
	CHECK(strcmp(perms(run), "") == 0);
}

static void test_sizes(void)
{
	ExecMem mem;

	CHECK(exec_mem_init(&mem, page));
	CHECK_EQ(mem.size, page);
	mem.write[page - 1] = 1;
	exec_mem_free(&mem);

	CHECK(exec_mem_init(&mem, page + 1));
	CHECK_EQ(mem.size, 2 * page);
	mem.write[2 * page - 1] = 1;
	CHECK_EQ(mem.run[2 * page - 1], 1);
	exec_mem_free(&mem);

	CHECK(exec_mem_init(&mem, 1));
	CHECK_EQ(mem.size, page);
	exec_mem_free(&mem);
}

static void test_errors(void)
{
	ExecMem mem = {.size = 7};
	int fd = lowest_fd();

	errno = 0;
	CHECK(!exec_mem_init(&mem, 0));
	CHECK_EQ(errno, EINVAL);
	CHECK_EQ(mem.size, 0);
	CHECK(mem.write == NULL);

	/* Rounding up would wrap around. */
	errno = 0;
	CHECK(!exec_mem_init(&mem, SIZE_MAX - page + 2));
	CHECK_EQ(errno, EINVAL);
	CHECK_EQ(mem.size, 0);

	/* The largest size that rounds up: too big for the file. */
	errno = 0;
	CHECK(!exec_mem_init(&mem, SIZE_MAX - page + 1));
	CHECK(errno != 0);
	CHECK_EQ(mem.size, 0);

	/* A file that fits but two views of it do not: 2 x 64 TB is all
	 * of a 47-bit address space. The first view is unmapped. */
	int maps = count_maps();
	errno = 0;
	CHECK(!exec_mem_init(&mem, (size_t)1 << 46));
	CHECK_EQ(errno, ENOMEM);
	CHECK_EQ(mem.size, 0);
	CHECK(mem.write == NULL);
	CHECK(mem.run == NULL);
	CHECK_EQ(count_maps(), maps);

	/* No file descriptor left open by any of them. */
	CHECK_EQ(lowest_fd(), fd);

	/* No file descriptor left for the memfd. */
	struct rlimit saved, limit;
	CHECK(getrlimit(RLIMIT_NOFILE, &saved) == 0);
	limit = saved;
	limit.rlim_cur = (rlim_t)fd;
	CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);
	errno = 0;
	CHECK(!exec_mem_init(&mem, page));
	CHECK_EQ(errno, EMFILE);
	CHECK_EQ(mem.size, 0);
	CHECK(setrlimit(RLIMIT_NOFILE, &saved) == 0);

	/* Freeing an empty one does nothing. */
	exec_mem_free(&mem);
	CHECK_EQ(mem.size, 0);
}

#if defined(__x86_64__)

static EXEC_MEM_CALLER int call(int (*fn)(void))
{
	return fn();
}

/* The run view executes: mov eax, 42; ret. */
static void test_run(void)
{
	static const uint8_t ret42[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
	ExecMem mem;
	int (*fn)(void);

	CHECK(exec_mem_init(&mem, 64));
	memcpy(mem.write + 16, ret42, sizeof ret42);
	const uint8_t *entry = mem.run + 16;
	memcpy(&fn, &entry, sizeof fn);
	CHECK_EQ(call(fn), 42);

	/* Code changed between runs is seen (SDM vol. 3A 11.1.3). */
	mem.write[17] = 0x07;
	CHECK_EQ(call(fn), 7);
	exec_mem_free(&mem);
}

#else

static void test_run(void)
{
	printf("SKIP running code: not an x86-64 host\n");
}

#endif

int main(void)
{
	page = (size_t)sysconf(_SC_PAGESIZE);
	RUN_TEST(test_views);
	RUN_TEST(test_sizes);
	RUN_TEST(test_errors);
	RUN_TEST(test_run);
	return test_summary();
}

#else

int main(void)
{
	ExecMem mem;

	CHECK(!exec_mem_init(&mem, 4096));
	CHECK_EQ(errno, ENOSYS);
	return test_summary();
}

#endif
