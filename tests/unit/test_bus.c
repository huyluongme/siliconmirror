/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for the memory bus (src/core/bus.c). */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "test.h"

/* A fake device that records its last access. */
typedef struct FakeDevice {
	uint64_t read_value; /* returned by every read */
	uint64_t last_offset;
	uint64_t last_value;
	unsigned last_size;
	unsigned reads;
	unsigned writes;
} FakeDevice;

static uint64_t fake_read(void *ctx, uint64_t offset, unsigned size)
{
	FakeDevice *dev = ctx;

	dev->last_offset = offset;
	dev->last_size = size;
	dev->reads++;
	return dev->read_value;
}

static void fake_write(void *ctx, uint64_t offset, uint64_t value,
                       unsigned size)
{
	FakeDevice *dev = ctx;

	dev->last_offset = offset;
	dev->last_value = value;
	dev->last_size = size;
	dev->writes++;
}

static const BusMmioOps fake_ops = {
	.read = fake_read,
	.write = fake_write,
};

static void test_empty_bus(void)
{
	Bus *bus = bus_create();
	uint64_t value = 0xdead;

	CHECK(bus != NULL);
	CHECK_EQ(bus_read(bus, 0, 4, &value), BUS_ERR_UNMAPPED);
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_write(bus, 0, 4, 1), BUS_ERR_UNMAPPED);
	bus_destroy(bus);
	bus_destroy(NULL);
}

static void test_ram_starts_zeroed(void)
{
	Bus *bus = bus_create();
	uint64_t value = 1;

	CHECK_EQ(bus_map_ram(bus, "ram", 0x1000, 0x1000), BUS_OK);
	CHECK_EQ(bus_read(bus, 0x1000, 8, &value), BUS_OK);
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_read(bus, 0x1ff8, 8, &value), BUS_OK);
	CHECK_EQ(value, 0);
	bus_destroy(bus);
}

static void test_ram_access_sizes(void)
{
	Bus *bus = bus_create();
	uint64_t value;

	CHECK_EQ(bus_map_ram(bus, "ram", 0x1000, 0x1000), BUS_OK);
	CHECK_EQ(bus_write(bus, 0x1000, 8, 0x1122334455667788), BUS_OK);

	CHECK_EQ(bus_read(bus, 0x1000, 1, &value), BUS_OK);
	CHECK_EQ(value, 0x88);
	CHECK_EQ(bus_read(bus, 0x1000, 2, &value), BUS_OK);
	CHECK_EQ(value, 0x7788);
	CHECK_EQ(bus_read(bus, 0x1000, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x55667788);
	CHECK_EQ(bus_read(bus, 0x1007, 1, &value), BUS_OK);
	CHECK_EQ(value, 0x11);

	/* A narrow write changes only its own bytes. */
	CHECK_EQ(bus_write(bus, 0x1003, 1, 0xab), BUS_OK);
	CHECK_EQ(bus_read(bus, 0x1000, 8, &value), BUS_OK);
	CHECK_EQ(value, 0x11223344ab667788);

	/* Bits above the access size are dropped. */
	CHECK_EQ(bus_write(bus, 0x1010, 2, 0xffff1234), BUS_OK);
	CHECK_EQ(bus_read(bus, 0x1010, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x1234);

	/* Unaligned accesses are allowed. */
	CHECK_EQ(bus_write(bus, 0x1021, 4, 0xcafef00d), BUS_OK);
	CHECK_EQ(bus_read(bus, 0x1021, 4, &value), BUS_OK);
	CHECK_EQ(value, 0xcafef00d);

	bus_destroy(bus);
}

static void test_ram_is_little_endian(void)
{
	Bus *bus = bus_create();
	uint8_t bytes[4];
	static const uint8_t expected[4] = {0x44, 0x33, 0x22, 0x11};

	CHECK_EQ(bus_map_ram(bus, "ram", 0, 0x100), BUS_OK);
	CHECK_EQ(bus_write(bus, 0x10, 4, 0x11223344), BUS_OK);
	CHECK_EQ(bus_read_block(bus, 0x10, bytes, sizeof(bytes)), BUS_OK);
	CHECK(memcmp(bytes, expected, sizeof(bytes)) == 0);
	bus_destroy(bus);
}

static void test_unmapped_addresses(void)
{
	Bus *bus = bus_create();
	uint64_t value = 0xdead;

	CHECK_EQ(bus_map_ram(bus, "ram", 0x1000, 0x1000), BUS_OK);
	CHECK_EQ(bus_read(bus, 0x0fff, 1, &value), BUS_ERR_UNMAPPED);
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_read(bus, 0x2000, 1, &value), BUS_ERR_UNMAPPED);
	CHECK_EQ(bus_write(bus, 0x2000, 4, 1), BUS_ERR_UNMAPPED);
	CHECK_EQ(bus_read(bus, UINT64_MAX, 1, &value), BUS_ERR_UNMAPPED);
	bus_destroy(bus);
}

static void test_access_past_region_end(void)
{
	Bus *bus = bus_create();
	uint64_t value = 0xdead;

	CHECK_EQ(bus_map_ram(bus, "ram", 0x1000, 0x1000), BUS_OK);
	CHECK_EQ(bus_read(bus, 0x1ff8, 8, &value), BUS_OK);
	CHECK_EQ(bus_read(bus, 0x1ffc, 8, &value), BUS_ERR_BOUNDARY);
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_write(bus, 0x1fff, 2, 0), BUS_ERR_BOUNDARY);

	/* Still an error when the next region is right after this one. */
	CHECK_EQ(bus_map_ram(bus, "ram2", 0x2000, 0x1000), BUS_OK);
	CHECK_EQ(bus_read(bus, 0x1ffc, 8, &value), BUS_ERR_BOUNDARY);
	bus_destroy(bus);
}

static void test_mmio_dispatch(void)
{
	Bus *bus = bus_create();
	FakeDevice dev = {.read_value = UINT64_MAX};
	uint64_t value;

	CHECK_EQ(bus_map_mmio(bus, "dev", 0x3f201000, 0x1000, &fake_ops, &dev),
	         BUS_OK);

	CHECK_EQ(bus_write(bus, 0x3f201018, 4, 0xabcd), BUS_OK);
	CHECK_EQ(dev.writes, 1);
	CHECK_EQ(dev.last_offset, 0x18);
	CHECK_EQ(dev.last_size, 4);
	CHECK_EQ(dev.last_value, 0xabcd);

	/* The device only sees the low size bytes of a written value. */
	CHECK_EQ(bus_write(bus, 0x3f201000, 1, 0x1ff), BUS_OK);
	CHECK_EQ(dev.last_value, 0xff);

	/* Extra bits returned by the device are cleared. */
	CHECK_EQ(bus_read(bus, 0x3f201004, 2, &value), BUS_OK);
	CHECK_EQ(value, 0xffff);
	CHECK_EQ(dev.reads, 1);
	CHECK_EQ(dev.last_offset, 4);
	CHECK_EQ(dev.last_size, 2);

	bus_destroy(bus);
}

static void test_mmio_without_callbacks(void)
{
	static const BusMmioOps no_ops = {0};
	Bus *bus = bus_create();
	uint64_t value = 0xdead;

	CHECK_EQ(bus_map_mmio(bus, "dummy", 0x1000, 0x100, &no_ops, NULL), BUS_OK);
	CHECK_EQ(bus_read(bus, 0x1000, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_write(bus, 0x1000, 4, 1), BUS_OK);
	bus_destroy(bus);
}

static void test_overlap_rejected(void)
{
	Bus *bus = bus_create();
	FakeDevice dev = {0};

	CHECK_EQ(bus_map_ram(bus, "a", 0x1000, 0x1000), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "same", 0x1000, 0x1000), BUS_ERR_OVERLAP);
	CHECK_EQ(bus_map_ram(bus, "start", 0x0800, 0x1000), BUS_ERR_OVERLAP);
	CHECK_EQ(bus_map_ram(bus, "end", 0x1fff, 1), BUS_ERR_OVERLAP);
	CHECK_EQ(bus_map_ram(bus, "around", 0, 0x4000), BUS_ERR_OVERLAP);
	CHECK_EQ(bus_map_mmio(bus, "inside", 0x1800, 0x10, &fake_ops, &dev),
	         BUS_ERR_OVERLAP);

	/* Regions right next to each other are fine. */
	CHECK_EQ(bus_map_ram(bus, "after", 0x2000, 0x1000), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "before", 0, 0x1000), BUS_OK);
	bus_destroy(bus);
}

static void test_invalid_regions(void)
{
	Bus *bus = bus_create();
	FakeDevice dev = {0};
	uint64_t value;

	CHECK_EQ(bus_map_ram(bus, "empty", 0x1000, 0), BUS_ERR_INVALID);
	CHECK_EQ(bus_map_mmio(bus, "no ops", 0x1000, 0x10, NULL, NULL),
	         BUS_ERR_INVALID);
	CHECK_EQ(
		bus_map_mmio(bus, "wraps", UINT64_MAX - 0xfff, 0x2000, &fake_ops, &dev),
		BUS_ERR_INVALID);

	/* A region may end on the very last address. */
	CHECK_EQ(
		bus_map_mmio(bus, "top", UINT64_MAX - 0xfff, 0x1000, &fake_ops, &dev),
		BUS_OK);
	CHECK_EQ(bus_read(bus, UINT64_MAX - 7, 8, &value), BUS_OK);
	CHECK_EQ(bus_read(bus, UINT64_MAX - 3, 8, &value), BUS_ERR_BOUNDARY);
	bus_destroy(bus);
}

static void test_many_regions_in_any_order(void)
{
	Bus *bus = bus_create();
	uint64_t value;

	/* Map 16 pages in a scrambled order: 0, 7, 14, 5, 12, ... */
	for (unsigned i = 0; i < 16; i++) {
		uint64_t page = (i * 7) % 16;

		CHECK_EQ(bus_map_ram(bus, "page", page * 0x1000, 0x1000), BUS_OK);
		CHECK_EQ(bus_write(bus, page * 0x1000 + 0x80, 4, page), BUS_OK);
	}
	for (uint64_t page = 0; page < 16; page++) {
		CHECK_EQ(bus_read(bus, page * 0x1000 + 0x80, 4, &value), BUS_OK);
		CHECK_EQ(value, page);
	}
	bus_destroy(bus);
}

static void test_block_copy(void)
{
	Bus *bus = bus_create();
	FakeDevice dev = {.read_value = 0x5a};
	uint8_t data[32];
	uint8_t back[32];

	for (unsigned i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i + 1);

	CHECK_EQ(bus_map_ram(bus, "ram1", 0x1000, 0x1000), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "ram2", 0x2000, 0x1000), BUS_OK);
	CHECK_EQ(bus_map_mmio(bus, "dev", 0x3000, 0x10, &fake_ops, &dev), BUS_OK);

	/* Across two adjacent RAM regions. */
	CHECK_EQ(bus_write_block(bus, 0x1ff0, data, sizeof(data)), BUS_OK);
	CHECK_EQ(bus_read_block(bus, 0x1ff0, back, sizeof(back)), BUS_OK);
	CHECK(memcmp(data, back, sizeof(data)) == 0);

	/* From RAM into MMIO: the device gets one byte at a time. */
	CHECK_EQ(bus_write_block(bus, 0x2ff8, data, 16), BUS_OK);
	CHECK_EQ(dev.writes, 8);
	CHECK_EQ(dev.last_offset, 7);
	CHECK_EQ(dev.last_size, 1);
	CHECK_EQ(dev.last_value, 16);

	CHECK_EQ(bus_read_block(bus, 0x3000, back, 4), BUS_OK);
	CHECK_EQ(dev.reads, 4);
	CHECK_EQ(back[3], 0x5a);

	/* Running off the end of the last region. */
	CHECK_EQ(bus_read_block(bus, 0x3008, back, 16), BUS_ERR_UNMAPPED);
	CHECK_EQ(bus_write_block(bus, 0, data, 4), BUS_ERR_UNMAPPED);

	/* An empty copy always succeeds. */
	CHECK_EQ(bus_write_block(bus, 0, data, 0), BUS_OK);
	bus_destroy(bus);
}

static void test_block_copy_at_top_of_address_space(void)
{
	Bus *bus = bus_create();
	uint8_t buf[8] = {0};

	CHECK_EQ(bus_map_ram(bus, "top", UINT64_MAX - 0xff, 0x100), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "bottom", 0, 0x100), BUS_OK);
	CHECK_EQ(bus_write_block(bus, UINT64_MAX - 7, buf, 8), BUS_OK);

	/* A copy must not wrap around to address 0. */
	CHECK_EQ(bus_write_block(bus, UINT64_MAX - 3, buf, 8), BUS_ERR_UNMAPPED);
	bus_destroy(bus);
}

static void test_status_strings(void)
{
	for (int s = BUS_OK; s <= BUS_ERR_NOMEM; s++) {
		const char *str = bus_status_str((BusStatus)s);

		CHECK(str != NULL && str[0] != '\0');
	}
	CHECK(strcmp(bus_status_str((BusStatus)99), "unknown bus status") == 0);
}

int main(void)
{
	RUN_TEST(test_empty_bus);
	RUN_TEST(test_ram_starts_zeroed);
	RUN_TEST(test_ram_access_sizes);
	RUN_TEST(test_ram_is_little_endian);
	RUN_TEST(test_unmapped_addresses);
	RUN_TEST(test_access_past_region_end);
	RUN_TEST(test_mmio_dispatch);
	RUN_TEST(test_mmio_without_callbacks);
	RUN_TEST(test_overlap_rejected);
	RUN_TEST(test_invalid_regions);
	RUN_TEST(test_many_regions_in_any_order);
	RUN_TEST(test_block_copy);
	RUN_TEST(test_block_copy_at_top_of_address_space);
	RUN_TEST(test_status_strings);
	return test_summary();
}
