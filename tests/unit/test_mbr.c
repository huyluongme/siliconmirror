/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the MBR partition table (src/boot/mbr.c), laid out as
 * Wikipedia's "Master boot record" describes it.
 */

#include <stdint.h>
#include <string.h>

#include "block_mem.h"
#include "boot/mbr.h"
#include "test.h"
#include "util/byteorder.h"

#define DISK_SECTORS 1000

static void entry(BlockMem *m, unsigned i, uint8_t status, uint8_t type,
                  uint32_t first, uint32_t sectors)
{
	uint8_t *e = m->data + 446 + 16 * i;

	e[0] = status;
	e[1] = 0xfe; /* CHS addresses: not used */
	e[4] = type;
	e[5] = 0xfe;
	store_le32(e + 8, first);
	store_le32(e + 12, sectors);
}

static void disk(BlockMem *m)
{
	block_mem_init(m, DISK_SECTORS * 512);
	m->data[510] = 0x55;
	m->data[511] = 0xaa;
}

static void test_table(void)
{
	BlockMem m;
	MbrPartition p[MBR_NUM_PARTITIONS];
	char error[128] = "";

	disk(&m);
	entry(&m, 0, 0x00, 0x0c, 8, 100);
	entry(&m, 1, 0x80, 0x83, 108, DISK_SECTORS - 108); /* to the end */
	entry(&m, 3, 0x00, 0x00, 0xffffffff, 0xffffffff);  /* unused */
	CHECK(mbr_read(&m.dev, p, error, sizeof(error)));
	CHECK(strcmp(error, "") == 0);
	CHECK_EQ(p[0].type, 0x0c);
	CHECK(!p[0].active);
	CHECK_EQ(p[0].first_sector, 8);
	CHECK_EQ(p[0].sectors, 100);
	CHECK_EQ(p[1].type, 0x83);
	CHECK(p[1].active);
	CHECK_EQ(p[1].first_sector, 108);
	CHECK_EQ(p[1].sectors, DISK_SECTORS - 108);
	CHECK_EQ(p[2].type, 0);
	CHECK_EQ(p[3].type, 0);
	CHECK_EQ(p[3].first_sector, 0xffffffff);
	block_mem_free(&m);
}

static void test_errors(void)
{
	BlockMem m;
	MbrPartition p[MBR_NUM_PARTITIONS];
	char error[128];

	disk(&m);
	m.data[511] = 0xab;
	CHECK(!mbr_read(&m.dev, p, error, sizeof(error)));
	CHECK(strstr(error, "no partition table") != NULL);
	m.data[511] = 0xaa;
	m.data[510] = 0x56;
	CHECK(!mbr_read(&m.dev, p, error, sizeof(error)));
	CHECK(strstr(error, "no partition table") != NULL);
	m.data[510] = 0x55;

	entry(&m, 2, 0x01, 0x0c, 8, 100);
	CHECK(!mbr_read(&m.dev, p, error, sizeof(error)));
	CHECK(strcmp(error, "partition 3 has an invalid status, 0x01") == 0);
	entry(&m, 2, 0x7f, 0x0c, 8, 100);
	CHECK(!mbr_read(&m.dev, p, error, sizeof(error)));
	CHECK(strcmp(error, "partition 3 has an invalid status, 0x7f") == 0);

	entry(&m, 2, 0x00, 0x0c, 8, DISK_SECTORS - 7);
	CHECK(!mbr_read(&m.dev, p, error, sizeof(error)));
	CHECK(strcmp(error, "partition 3 goes past the end of the disk") == 0);
	entry(&m, 2, 0x00, 0x0c, DISK_SECTORS + 1, 0);
	CHECK(!mbr_read(&m.dev, p, error, sizeof(error)));
	CHECK(strcmp(error, "partition 3 goes past the end of the disk") == 0);
	entry(&m, 2, 0x00, 0x0c, DISK_SECTORS, 0); /* empty, at the end */
	CHECK(mbr_read(&m.dev, p, error, sizeof(error)));

	m.fail_from = 511;
	CHECK(!mbr_read(&m.dev, p, error, sizeof(error)));
	CHECK(strcmp(error, "cannot read the partition table") == 0);
	block_mem_free(&m);

	block_mem_init(&m, 511); /* smaller than a sector */
	CHECK(!mbr_read(&m.dev, p, error, sizeof(error)));
	CHECK(strcmp(error, "cannot read the partition table") == 0);
	block_mem_free(&m);
}

int main(void)
{
	RUN_TEST(test_table);
	RUN_TEST(test_errors);
	return test_summary();
}
