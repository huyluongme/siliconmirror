/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/direct.h"
#include "test.h"

/* Test files go to the current directory: the build tree under CTest. */
#define IMAGE "test_boot_direct.bin"
#define EMPTY "test_boot_direct_empty.bin"

/* More than one 64KB block, so that the loader reads several times. */
#define IMAGE_SIZE 100000

static uint8_t image[IMAGE_SIZE];

static void write_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");

	CHECK(f != NULL);
	if (f == NULL)
		return;
	CHECK_EQ(fwrite(data, 1, len, f), len);
	fclose(f);
}

/* A bus with 1 MiB of RAM at 0. */
static Bus *make_bus(void)
{
	Bus *bus = bus_create();

	CHECK(bus != NULL);
	CHECK_EQ(bus_map_ram(bus, "ram", 0, 0x100000), BUS_OK);
	return bus;
}

static void test_load(void)
{
	static uint8_t readback[IMAGE_SIZE];
	Bus *bus = make_bus();
	char error[256] = "";

	for (size_t i = 0; i < IMAGE_SIZE; i++)
		image[i] = (uint8_t)(i * 7 + 1);
	write_file(IMAGE, image, IMAGE_SIZE);

	/* The image may end exactly at the limit. */
	CHECK(boot_direct_load(bus, IMAGE, 0x1000, 0x1000 + IMAGE_SIZE, error,
	                       sizeof(error)));
	CHECK_EQ(bus_read_block(bus, 0x1000, readback, IMAGE_SIZE), BUS_OK);
	CHECK(memcmp(readback, image, IMAGE_SIZE) == 0);
	bus_destroy(bus);
}

static void test_errors(void)
{
	Bus *bus = make_bus();
	char error[256];

	CHECK(!boot_direct_load(bus, "no/such/file", 0, 0x100000, error,
	                        sizeof(error)));
	CHECK(strncmp(error, "no/such/file: ", 14) == 0);

	/* A directory opens on Linux, but reading it fails. */
	CHECK(!boot_direct_load(bus, ".", 0, 0x100000, error, sizeof(error)));
	CHECK(strncmp(error, ".: ", 3) == 0);

	write_file(EMPTY, "", 0);
	CHECK(!boot_direct_load(bus, EMPTY, 0, 0x100000, error, sizeof(error)));
	CHECK(strcmp(error, EMPTY ": the file is empty") == 0);

	/* One byte too many for the limit. */
	CHECK(!boot_direct_load(bus, IMAGE, 0x1000, 0x1000 + IMAGE_SIZE - 1, error,
	                        sizeof(error)));
	CHECK(strcmp(error, IMAGE ": does not fit in memory between 0x1000 and "
	                          "0x1969f") == 0);

	/* The limit allows it, but there is no RAM there. */
	CHECK(!boot_direct_load(bus, IMAGE, 0x200000, 0x400000, error,
	                        sizeof(error)));
	CHECK(strcmp(error, IMAGE ": cannot load at 0x200000: no region at this "
	                          "address") == 0);
	bus_destroy(bus);

	remove(IMAGE);
	remove(EMPTY);
}

int main(void)
{
	RUN_TEST(test_load);
	RUN_TEST(test_errors);
	return test_summary();
}
