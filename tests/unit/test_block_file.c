/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for block devices on image files (src/backend/block/file.c). */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backend/block/file.h"
#include "test.h"

#define IMAGE "test_block_file.img"

static void make_image(size_t size)
{
	FILE *f = fopen(IMAGE, "wb");

	CHECK(f != NULL);
	for (size_t i = 0; i < size; i++)
		fputc((int)(i & 0xff), f);
	fclose(f);
}

/* Reads and writes go to the file, at any offset. */
static void test_read_write(void)
{
	char error[128] = "";
	BlockDevice *b;
	uint8_t buf[8];
	FILE *f;

	make_image(4096);
	b = block_file_open(IMAGE, error, sizeof(error));
	CHECK(b != NULL);
	CHECK(strcmp(error, "") == 0);
	CHECK_EQ(b->size, 4096);
	CHECK(block_read(b, 510, buf, 4));
	CHECK_EQ(buf[0], 0xfe);
	CHECK_EQ(buf[1], 0xff);
	CHECK_EQ(buf[2], 0x00);
	CHECK_EQ(buf[3], 0x01);
	CHECK(block_write(b, 4093, "abc", 3));
	CHECK(block_read(b, 4092, buf, 4));
	CHECK(memcmp(buf, (const uint8_t[]){0xfc, 'a', 'b', 'c'}, 4) == 0);
	/* One byte; and with errno left at EINTR by something before. */
	CHECK(block_write(b, 4092, "z", 1));
	buf[0] = 0;
	errno = EINTR;
	CHECK(block_read(b, 4092, buf, 1));
	CHECK_EQ(buf[0], 'z');
	errno = EINTR;
	CHECK(block_write(b, 0, "y", 1));
	CHECK(block_read(b, 0, buf, 1));
	CHECK_EQ(buf[0], 'y');
	/* Offsets that the system refuses: an error, not a loop. */
	CHECK(!b->ops->read(b, UINT64_C(1) << 63, buf, 1));
	CHECK(!b->ops->write(b, UINT64_C(1) << 63, buf, 1));
	/* Outside the device. */
	CHECK(!block_read(b, 4093, buf, 4));
	CHECK(!block_read(b, 4097, buf, 0));
	CHECK(!block_write(b, UINT64_MAX, buf, 2));
	CHECK(block_read(b, 4096, buf, 0));
	block_destroy(b);
	block_destroy(NULL);

	/* The write reached the file. */
	f = fopen(IMAGE, "rb");
	CHECK(f != NULL);
	fseek(f, 4093, SEEK_SET);
	CHECK(fread(buf, 1, 3, f) == 3);
	CHECK(memcmp(buf, "abc", 3) == 0);
	fclose(f);
	remove(IMAGE);
}

/* Errors: no file, a file that cannot be written, a file in use. */
static void test_errors(void)
{
	char error[128];
	BlockDevice *b;
	pid_t child;
	int status, ready[2], done[2];
	char c;

	CHECK(block_file_open("no/such/image", error, sizeof(error)) == NULL);
	CHECK(strstr(error, "cannot open no/such/image: ") != NULL);
	CHECK(block_file_open(".", error, sizeof(error)) == NULL);
	CHECK(strstr(error, "cannot open .: ") != NULL);

	make_image(512);
	if (geteuid() != 0) { /* root can write anything */
		CHECK_EQ(chmod(IMAGE, 0444), 0);
		CHECK(block_file_open(IMAGE, error, sizeof(error)) == NULL);
		CHECK(strstr(error, "cannot open " IMAGE ": ") != NULL);
		CHECK_EQ(chmod(IMAGE, 0644), 0);
	}

	/* Another process holds the image: it is locked. */
	CHECK_EQ(pipe(ready), 0);
	CHECK_EQ(pipe(done), 0);
	child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		BlockDevice *held = block_file_open(IMAGE, error, sizeof(error));

		c = held != NULL ? 'y' : 'n';
		if (write(ready[1], &c, 1) != 1 || read(done[0], &c, 1) != 1)
			_exit(2);
		block_destroy(held);
		_exit(0);
	}
	CHECK_EQ(read(ready[0], &c, 1), 1);
	CHECK_EQ(c, 'y');
	{
		int fd = open("/dev/null", O_RDONLY);

		close(fd);
		CHECK(block_file_open(IMAGE, error, sizeof(error)) == NULL);
		CHECK_EQ(open("/dev/null", O_RDONLY), fd); /* closed again */
		close(fd);
	}
	CHECK(strcmp(error, IMAGE " is in use by another program") == 0);
	CHECK_EQ(write(done[1], "x", 1), 1);
	CHECK_EQ(waitpid(child, &status, 0), child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	/* Free again once the other process is gone. */
	b = block_file_open(IMAGE, error, sizeof(error));
	CHECK(b != NULL);
	block_destroy(b);
	close(ready[0]);
	close(ready[1]);
	close(done[0]);
	close(done[1]);
	remove(IMAGE);
}

/* The lowest file descriptor that is free now. */
static int free_fd(void)
{
	int fd = open("/dev/null", O_RDONLY);

	CHECK(fd >= 0);
	close(fd);
	return fd;
}

/*
 * An image that shrinks while open: reads past its new end fail. An
 * empty image opens. Every path closes what it opened.
 */
static void test_edges(void)
{
	char error[128];
	BlockDevice *b;
	uint8_t buf[512];
	int fd = free_fd();

	make_image(4096);
	b = block_file_open(IMAGE, error, sizeof(error));
	CHECK(b != NULL);
	CHECK_EQ(truncate(IMAGE, 1024), 0);
	CHECK(block_read(b, 512, buf, 512));
	CHECK(!block_read(b, 1024, buf, 512));
	errno = EINTR;
	CHECK(!block_read(b, 2048, buf, 512));
	block_destroy(b);
	CHECK_EQ(free_fd(), fd);

	make_image(0);
	b = block_file_open(IMAGE, error, sizeof(error));
	CHECK(b != NULL);
	CHECK_EQ(b->size, 0);
	block_destroy(b);
	CHECK_EQ(free_fd(), fd);

	/* Standard input closed: the image may get descriptor 0. */
	{
		int saved = dup(0);

		CHECK(saved >= 0);
		close(0);
		b = block_file_open(IMAGE, error, sizeof(error));
		CHECK(b != NULL);
		block_destroy(b);
		CHECK_EQ(dup2(saved, 0), 0);
		close(saved);
	}
	remove(IMAGE);
}

int main(void)
{
	RUN_TEST(test_read_write);
	RUN_TEST(test_errors);
	RUN_TEST(test_edges);
	return test_summary();
}
