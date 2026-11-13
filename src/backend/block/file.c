/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Block devices backed by an image file; see file.h. */

/* pread(), pwrite() and fcntl() locks are POSIX. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "backend/block/file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct BlockFile {
	BlockDevice block; /* first, so that a BlockDevice * is a BlockFile * */
	int fd;
} BlockFile;

static bool file_read(BlockDevice *b, uint64_t offset, void *buf, size_t len)
{
	BlockFile *f = (BlockFile *)b;
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = pread(f->fd, p, len, (off_t)offset);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		p += n;
		len -= (size_t)n;
		offset += (uint64_t)n;
	}
	return true;
}

static bool file_write(BlockDevice *b, uint64_t offset, const void *buf,
                       size_t len)
{
	BlockFile *f = (BlockFile *)b;
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = pwrite(f->fd, p, len, (off_t)offset);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		p += n;
		len -= (size_t)n;
		offset += (uint64_t)n;
	}
	return true;
}

static void file_destroy(BlockDevice *b)
{
	BlockFile *f = (BlockFile *)b;

	close(f->fd); /* also releases the lock */
	free(f);
}

static const BlockDeviceOps file_ops = {
	.read = file_read,
	.write = file_write,
	.destroy = file_destroy,
};

BlockDevice *block_file_open(const char *path, char *error, size_t error_size)
{
	struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
	BlockFile *f;
	off_t size;
	int fd = open(path, O_RDWR);

	if (fd < 0) {
		snprintf(error, error_size, "cannot open %s: %s", path,
		         strerror(errno));
		return NULL;
	}
	if (fcntl(fd, F_SETLK, &lock) < 0) {
		snprintf(error, error_size, "%s is in use by another program", path);
		close(fd);
		return NULL;
	}
	size = lseek(fd, 0, SEEK_END);
	if (size < 0) {
		snprintf(error, error_size, "cannot find the size of %s: %s", path,
		         strerror(errno));
		close(fd);
		return NULL;
	}
	f = calloc(1, sizeof(*f));
	if (f == NULL) {
		snprintf(error, error_size, "out of memory");
		close(fd);
		return NULL;
	}
	f->block = (BlockDevice){.ops = &file_ops, .size = (uint64_t)size};
	f->fd = fd;
	return &f->block;
}
