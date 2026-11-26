/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#define _GNU_SOURCE /* memfd_create() */

#include "cpu/jit/exec_mem.h"

#include <errno.h>

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifdef __linux__

bool exec_mem_init(ExecMem *mem, size_t size)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	void *write = MAP_FAILED;
	void *run = MAP_FAILED;
	int saved;

	/* A size of 0, or one so large that rounding up wraps around to 0,
	 * is refused by mmap() with EINVAL. */
	*mem = (ExecMem){0};
	size = (size + page - 1) / page * page;

	/* An anonymous file, gone with its last mapping once fd is closed. */
	int fd = memfd_create("smirror-jit", MFD_CLOEXEC);
	if (fd < 0)
		return false;
	if (ftruncate(fd, (off_t)size) == 0) {
		write = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (write != MAP_FAILED)
			run = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
	}
	saved = errno;
	close(fd);

	if (run == MAP_FAILED) {
		if (write != MAP_FAILED)
			munmap(write, size);
		errno = saved;
		return false;
	}
	mem->write = write;
	mem->run = run;
	mem->size = size;
	return true;
}

void exec_mem_free(ExecMem *mem)
{
	/* An empty one: munmap() refuses a size of 0 and does nothing. */
	munmap(mem->write, mem->size);
	munmap(mem->run, mem->size);
	*mem = (ExecMem){0};
}

#else

bool exec_mem_init(ExecMem *mem, size_t size)
{
	(void)size;
	*mem = (ExecMem){0};
	errno = ENOSYS;
	return false;
}

void exec_mem_free(ExecMem *mem)
{
	*mem = (ExecMem){0};
}

#endif
