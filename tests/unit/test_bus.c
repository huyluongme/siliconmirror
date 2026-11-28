/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for the memory bus (src/core/bus.c). */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/bus.h"
#include "test.h"
#include "util/macros.h"

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

/* bus_ram_pointer(): host memory of a range inside one RAM region. */
static void test_ram_pointer(void)
{
	Bus *bus = bus_create();
	FakeDevice dev = {0};
	uint8_t *p;

	CHECK_EQ(bus_map_ram(bus, "ram", 0x1000, 0x2000), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "next", 0x3000, 0x1000), BUS_OK);
	CHECK_EQ(bus_map_mmio(bus, "dev", 0x4000, 0x1000, &fake_ops, &dev), BUS_OK);
	CHECK_EQ(bus_write(bus, 0x1ffc, 4, 0x11223344), BUS_OK);
	p = bus_ram_pointer(bus, 0x1000, 0x2000);
	CHECK(p != NULL);
	CHECK(bus_ram_pointer(bus, 0x1ffc, 4) == p + 0xffc);
	CHECK_EQ(p[0xffc], 0x44);
	CHECK(bus_ram_pointer(bus, 0x2fff, 1) == p + 0x1fff);
	CHECK(bus_ram_pointer(bus, 0x2fff, 2) == NULL); /* into "next" */
	CHECK(bus_ram_pointer(bus, 0x1000, 0x2001) == NULL);
	CHECK(bus_ram_pointer(bus, 0x1000, 0) == NULL);
	CHECK(bus_ram_pointer(bus, 0x0fff, 1) == NULL); /* unmapped */
	CHECK(bus_ram_pointer(bus, 0x4000, 4) == NULL); /* MMIO */
	CHECK_EQ(dev.reads + dev.writes, 0);
	CHECK(bus_ram_pointer(bus, 0x3000, 0x1000) != NULL);
	bus_destroy(bus);
}

/*
 * Which pages can be watched: all of a page in one RAM region. Regions:
 * "ram" 0-0x2fff, "odd" 0x10800-0x117ff (no whole page), "edge"
 * 0x20000-0x217ff (one whole page), "short" (a page but its last byte),
 * "big" with more pages than a word of the bitmap, "tail" whose last
 * byte is alone in the 65th page, a device and nothing.
 */
static Bus *watch_bus(FakeDevice *dev)
{
	Bus *bus = bus_create();

	CHECK_EQ(bus_map_ram(bus, "ram", 0, 0x3000), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "ram2", 0x3000, 0x1000), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "odd", 0x10800, 0x1000), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "edge", 0x20000, 0x1800), BUS_OK);
	CHECK_EQ(bus_map_mmio(bus, "dev", 0x30000, 0x1000, &fake_ops, dev), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "short", 0x50000, 0xfff), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "big", 0x100000, 0x100000), BUS_OK);
	CHECK_EQ(bus_map_ram(bus, "tail", 0x200000, 0x40001), BUS_OK);
	return bus;
}

static void test_watch_pages(void)
{
	FakeDevice dev = {0};
	Bus *bus = watch_bus(&dev);

	CHECK(!bus_ram_watched(bus, 0x1000));
	CHECK(bus_watch_ram(bus, 0x1234));
	CHECK(bus_ram_watched(bus, 0x1000));
	CHECK(bus_ram_watched(bus, 0x1fff));
	CHECK(!bus_ram_watched(bus, 0x0fff));
	CHECK(!bus_ram_watched(bus, 0x2000));
	CHECK(bus_watch_ram(bus, 0x1000)); /* again */
	CHECK(bus_watch_ram(bus, 0x2fff)); /* the last page of "ram" */
	CHECK(bus_ram_watched(bus, 0x2000));
	CHECK(!bus_ram_watched(bus, 0x3000)); /* "ram2" */

	/* Not all of the page in one RAM region. */
	CHECK(!bus_watch_ram(bus, 0x10800));
	CHECK(!bus_watch_ram(bus, 0x11000));
	CHECK(!bus_ram_watched(bus, 0x10800));
	CHECK(!bus_ram_watched(bus, 0x11000));
	CHECK(bus_watch_ram(bus, 0x20fff));
	CHECK(!bus_watch_ram(bus, 0x21000));
	CHECK(bus_ram_watched(bus, 0x20000));
	CHECK(!bus_ram_watched(bus, 0x21000));
	CHECK(!bus_watch_ram(bus, 0x50000));
	CHECK(!bus_ram_watched(bus, 0x50000));
	CHECK(!bus_watch_ram(bus, 0x240000));
	CHECK(!bus_ram_watched(bus, 0x240000));
	CHECK(!bus_watch_ram(bus, 0x30000)); /* MMIO */
	CHECK(!bus_watch_ram(bus, 0x40000)); /* nothing */
	CHECK(!bus_ram_watched(bus, 0x30000));
	CHECK(!bus_ram_watched(bus, 0x40000));
	CHECK_EQ(dev.reads + dev.writes, 0);

	/* Pages 63, 64 and 65 of "big": two words of the bitmap. */
	CHECK(bus_watch_ram(bus, 0x140000));
	CHECK(!bus_ram_watched(bus, 0x13f000));
	CHECK(bus_ram_watched(bus, 0x140000));
	CHECK(!bus_ram_watched(bus, 0x141000));
	CHECK(bus_watch_ram(bus, 0x13f000));
	CHECK(bus_ram_watched(bus, 0x13f000));
	CHECK(!bus_ram_watched(bus, 0x100000));
	bus_destroy(bus);
}

/* What a watcher was told. */
typedef struct Watch {
	Bus *bus;
	uint64_t probe; /* an address to read when told */
	uint64_t pages[8];
	uint64_t values[8]; /* at probe, when told */
	bool still[8];      /* whether the page was watched still */
	unsigned calls;
} Watch;

static void watcher(void *ctx, uint64_t page)
{
	Watch *w = ctx;

	if (w->calls == ARRAY_SIZE(w->pages))
		abort(); /* a mutant may call it without end */
	CHECK_EQ(bus_read(w->bus, w->probe, 4, &w->values[w->calls]), BUS_OK);
	w->still[w->calls] = bus_ram_watched(w->bus, page);
	w->pages[w->calls++] = page;
}

/* Writes to watched pages through the bus. */
static void test_watch_writes(void)
{
	FakeDevice dev = {0};
	Bus *bus = watch_bus(&dev);
	Watch w = {.bus = bus, .probe = 0x1ffe};
	uint8_t buf[0x1001] = {0};
	uint64_t value;

	bus_set_watcher(bus, watcher, &w);
	CHECK(bus_watch_ram(bus, 0x1000));
	CHECK(bus_watch_ram(bus, 0x2000));

	/* Reads, and writes elsewhere, are not seen. */
	CHECK_EQ(bus_read(bus, 0x1000, 8, &value), BUS_OK);
	CHECK_EQ(bus_read_block(bus, 0xff0, buf, 0x20), BUS_OK);
	CHECK_EQ(bus_write(bus, 0x0ffc, 4, 1), BUS_OK);
	CHECK_EQ(bus_write(bus, 0x30000, 4, 1), BUS_OK);
	CHECK_EQ(w.calls, 0);
	CHECK(bus_ram_watched(bus, 0x1000));

	/* A write to two pages: each is told once, after the write. */
	CHECK_EQ(bus_write(bus, 0x1ffe, 4, 0x11223344), BUS_OK);
	CHECK_EQ(w.calls, 2);
	CHECK_EQ(w.pages[0], 0x1000);
	CHECK_EQ(w.pages[1], 0x2000);
	CHECK_EQ(w.values[0], 0x11223344);
	CHECK(!w.still[0] && !w.still[1]);
	CHECK(!bus_ram_watched(bus, 0x1000));
	CHECK(!bus_ram_watched(bus, 0x2000));
	CHECK_EQ(bus_write(bus, 0x1000, 1, 0), BUS_OK);
	CHECK_EQ(w.calls, 2);

	/* Blocks: up to the page, into it, and over two regions. */
	w.calls = 0;
	w.probe = 0x1000;
	CHECK(bus_watch_ram(bus, 0x1000));
	CHECK_EQ(bus_write_block(bus, 0, buf, 0x1000), BUS_OK);
	CHECK_EQ(w.calls, 0);
	buf[0x1000] = 0x5a;
	CHECK_EQ(bus_write_block(bus, 0, buf, 0x1001), BUS_OK);
	CHECK_EQ(w.calls, 1);
	CHECK_EQ(w.pages[0], 0x1000);
	CHECK_EQ(w.values[0] & 0xff, 0x5a);
	CHECK(bus_watch_ram(bus, 0x2000));
	CHECK(bus_watch_ram(bus, 0x3000));
	CHECK_EQ(bus_write_block(bus, 0x2ff0, buf, 0x20), BUS_OK);
	CHECK_EQ(w.calls, 3);
	CHECK_EQ(w.pages[1], 0x2000);
	CHECK_EQ(w.pages[2], 0x3000);

	/* Pages of a region that does not start at 0: bits 63 and 64. */
	w.calls = 0;
	CHECK(bus_watch_ram(bus, 0x140000));
	CHECK_EQ(bus_write(bus, 0x140ff8, 8, 0), BUS_OK);
	CHECK_EQ(w.calls, 1);
	CHECK_EQ(w.pages[0], 0x140000);
	CHECK(bus_watch_ram(bus, 0x13f000));
	CHECK_EQ(bus_write(bus, 0x13f000, 1, 0), BUS_OK);
	CHECK_EQ(w.calls, 2);
	CHECK_EQ(w.pages[1], 0x13f000);
	CHECK(!bus_ram_watched(bus, 0x13f000));

	/* The last byte of "tail", in a page of its own. */
	CHECK_EQ(bus_write(bus, 0x240000, 1, 0), BUS_OK);
	CHECK_EQ(w.calls, 2);

	/* Without a watcher, a write still ends the watch. */
	bus_set_watcher(bus, NULL, NULL);
	CHECK(bus_watch_ram(bus, 0x1000));
	CHECK_EQ(bus_write(bus, 0x1000, 4, 0), BUS_OK);
	CHECK(!bus_ram_watched(bus, 0x1000));
	CHECK_EQ(w.calls, 2);
	CHECK_EQ(dev.writes, 1);
	bus_destroy(bus);
}

int main(void)
{
	RUN_TEST(test_empty_bus);
	RUN_TEST(test_ram_pointer);
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
	RUN_TEST(test_watch_pages);
	RUN_TEST(test_watch_writes);
	return test_summary();
}
