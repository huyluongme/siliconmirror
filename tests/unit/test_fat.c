/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for reading FAT file systems (src/boot/fat.c), on file
 * systems that fat_image.h builds in memory; tests/baremetal also checks
 * images made by mkfs.fat and mtools.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/fat.h"
#include "fat_image.h"
#include "test.h"
#include "util/byteorder.h"
#include "util/macros.h"

static FatVolume *open_fs(Fs *fs)
{
	char error[128] = "";
	FatVolume *v = fat_open(&fs->m.dev, BASE, (uint64_t)fs->total * SECTOR,
	                        error, sizeof(error));

	if (v == NULL)
		fprintf(stderr, "fat_open: %s\n", error);
	CHECK(v != NULL);
	return v;
}

static int find(FatVolume *v, const char *path, FatFile *file)
{
	char error[128] = "";
	int r = fat_find(v, path, file, error, sizeof(error));

	CHECK(r >= 0);
	return r;
}

/* The bytes of a file: byte i is i * 7, so that every cluster differs. */
static uint8_t *pattern(uint32_t size)
{
	uint8_t *p = malloc(size ? size : 1);

	for (uint32_t i = 0; i < size; i++)
		p[i] = (uint8_t)(i * 7);
	return p;
}

/* Files in the root and in a directory, on each type of FAT. */
static void check_type(unsigned bits, uint32_t total, uint32_t cluster_sectors)
{
	Fs fs;
	Dir root, overlays;
	uint8_t *kernel = pattern(3000), buf[3000];
	FatVolume *v;
	FatFile f;
	char error[128];

	format(&fs, bits, total, cluster_sectors);
	root = root_dir(&fs);
	short_entry(new_entry(&fs, &root), "BOOT       ", 0x08, 0, 0, bits);
	add_file(&fs, &root, "CONFIG  TXT", NULL, "arm_64bit=1\n", 12);
	add_file(&fs, &root, "KERNEL8 IMG", "kernel8.img", kernel, 3000);
	add_file(&fs, &root, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", "dtb", 3);
	add_file(&fs, &root, "EMPTY      ", NULL, NULL, 0);
	overlays = add_dir(&fs, &root, "OVERLAYS   ", "overlays");
	add_file(&fs, &overlays, "README     ", NULL, "readme", 6);
	add_file(&fs, &overlays, "DISABL~1DTB", "disable-bt.dtbo", "ov", 2);
	v = open_fs(&fs);
	CHECK_EQ(fat_bits(v), bits);

	CHECK_EQ(find(v, "config.txt", &f), 1);
	CHECK_EQ(f.size, 12);
	CHECK(!f.directory);
	CHECK(fat_read(v, &f, 0, buf, 12, error, sizeof(error)));
	CHECK(memcmp(buf, "arm_64bit=1\n", 12) == 0);
	CHECK(fat_read(v, &f, 4, buf, 3, error, sizeof(error)));
	CHECK(memcmp(buf, "64b", 3) == 0);

	/* A long name, and its short name, in any case. */
	CHECK_EQ(find(v, "KERNEL8.IMG", &f), 1);
	CHECK_EQ(find(v, "Kernel8.Img", &f), 1);
	CHECK_EQ(f.size, 3000);
	memset(buf, 0, sizeof(buf));
	CHECK(fat_read(v, &f, 0, buf, 3000, error, sizeof(error)));
	CHECK(memcmp(buf, kernel, 3000) == 0);
	/* Pieces across clusters. */
	CHECK(fat_read(v, &f, 1000, buf, 1500, error, sizeof(error)));
	CHECK(memcmp(buf, kernel + 1000, 1500) == 0);
	CHECK(fat_read(v, &f, 2999, buf, 1, error, sizeof(error)));
	CHECK_EQ(buf[0], kernel[2999]);
	CHECK(fat_read(v, &f, 3000, buf, 0, error, sizeof(error)));
	CHECK(!fat_read(v, &f, 2999, buf, 2, error, sizeof(error)));
	CHECK(strcmp(error, "a read outside the file") == 0);
	CHECK(!fat_read(v, &f, 3001, buf, 0, error, sizeof(error)));

	CHECK_EQ(find(v, "bcm2710-rpi-3-b.dtb", &f), 1);
	CHECK_EQ(f.size, 3);
	CHECK_EQ(find(v, "BCM271~1.DTB", &f), 1);
	CHECK_EQ(find(v, "bcm2710-rpi-3-b.dt", &f), 0);
	CHECK_EQ(find(v, "bcm2710-rpi-3-b.dtbx", &f), 0);
	CHECK_EQ(find(v, "empty", &f), 1);
	CHECK_EQ(f.size, 0);
	CHECK_EQ(f.cluster, 0);
	CHECK(fat_read(v, &f, 0, buf, 0, error, sizeof(error)));
	CHECK_EQ(find(v, "boot", &f), 0); /* the volume label */

	/* Directories, with "." and "..". */
	CHECK_EQ(find(v, "overlays", &f), 1);
	CHECK(f.directory);
	CHECK_EQ(f.size, 0);
	CHECK(!fat_read(v, &f, 0, buf, 0, error, sizeof(error)));
	CHECK_EQ(find(v, "/overlays/README", &f), 1);
	CHECK_EQ(f.size, 6);
	CHECK(fat_read(v, &f, 0, buf, 6, error, sizeof(error)));
	CHECK(memcmp(buf, "readme", 6) == 0);
	CHECK_EQ(find(v, "overlays/disable-bt.dtbo", &f), 1);
	CHECK_EQ(f.size, 2);
	CHECK_EQ(find(v, "overlays//./README", &f), 1);
	CHECK_EQ(find(v, "overlays/../config.txt", &f), 1);
	CHECK_EQ(f.size, 12);
	CHECK_EQ(find(v, "overlays/..", &f), 1);
	CHECK(f.directory);
	CHECK_EQ(f.root, bits != 32);
	CHECK_EQ(f.cluster, bits == 32 ? 2 : 0);
	CHECK_EQ(find(v, "overlays/../overlays/README", &f), 1);
	CHECK_EQ(find(v, "overlays/", &f), 1);
	CHECK(f.directory);
	CHECK_EQ(find(v, "/", &f), 1);
	CHECK(f.directory);
	CHECK_EQ(find(v, "", &f), 1);
	CHECK_EQ(find(v, "config.txt/x", &f), 0);
	CHECK_EQ(find(v, "overlays/missing", &f), 0);
	CHECK_EQ(find(v, "missing/README", &f), 0);
	fat_close(v);
	block_mem_free(&fs.m);
	free(kernel);
}

static void test_fat12(void)
{
	check_type(12, 2048, 1);
}

static void test_fat16(void)
{
	check_type(16, 16384, 1);
}

static void test_fat32(void)
{
	check_type(32, 70000, 1);
}

/* Clusters of several sectors. */
static void test_large_clusters(void)
{
	check_type(12, 8192, 4);
	check_type(16, 65536, 2);
}

/* The number of clusters decides the type: 4084 and 65524 at most. */
static void test_type_limits(void)
{
	static const struct {
		unsigned bits;
		uint32_t clusters, expected;
	} limits[] = {
		{12, 4084, 12},
		{16, 4085, 16},
		{16, 65524, 16},
		{32, 65525, 32},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(limits); i++) {
		Fs fs;
		FatVolume *v;
		uint32_t total;

		/* The size that gives the number of clusters wanted. */
		format(&fs, limits[i].bits, 1000, 1);
		total = (uint32_t)((fs.data - BASE) / SECTOR) + limits[i].clusters;
		block_mem_free(&fs.m);
		for (int k = 0; k < 3; k++) {
			format(&fs, limits[i].bits, total, 1);
			if (fs.clusters == limits[i].clusters)
				break;
			total += limits[i].clusters - fs.clusters;
			block_mem_free(&fs.m);
		}
		CHECK_EQ(fs.clusters, limits[i].clusters);
		v = open_fs(&fs);
		CHECK_EQ(fat_bits(v), limits[i].expected);
		fat_close(v);
		block_mem_free(&fs.m);
	}
}

/* Long names: checksums, order, deleted entries and the 0x05 escape. */
static void test_long_names(void)
{
	Fs fs;
	Dir root, sub;
	FatVolume *v;
	FatFile f;
	uint8_t *e;
	char name[300];

	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	/* A long name whose checksum is for another short name. */
	long_entries(&fs, &root, "wrong-checksum.txt", "OTHER   TXT");
	short_entry(new_entry(&fs, &root), "WRONG~1 TXT", 0x20, 0, 0, 16);
	/* A long name that a deleted entry interrupts. */
	long_entries(&fs, &root, "deleted-between.txt", "DELETE~1TXT");
	new_entry(&fs, &root)[0] = 0xe5;
	short_entry(new_entry(&fs, &root), "DELETE~1TXT", 0x20, 0, 0, 16);
	/* Entries out of order: the first part missing. */
	long_entries(&fs, &root, "a-name-of-two-entries", "TWO~1      ");
	e = at(&fs, fs.root + 32 * (root.used - 1));
	memset(e, 0, 32);
	e[0] = 0xe5; /* drops the entry with sequence number 1 */
	short_entry(new_entry(&fs, &root), "TWO~1      ", 0x20, 0, 0, 16);
	/* Part 2 again where part 1 should be. */
	long_entries(&fs, &root, "repeated-part-names", "REP~1      ");
	e = at(&fs, fs.root + 32 * (root.used - 1));
	e[0] = 2;
	short_entry(new_entry(&fs, &root), "REP~1      ", 0x20, 0, 0, 16);
	/* A long name of exactly 26 characters: no terminator. */
	long_entries(&fs, &root, "exactly-twenty-six-chars.x", "EXACTL~1X  ");
	short_entry(new_entry(&fs, &root), "EXACTL~1X  ", 0x20, 0, 0, 16);
	/* A name that starts with 0xe5 is stored with 0x05. */
	short_entry(new_entry(&fs, &root),
	            "\x05"
	            "ABC    TXT",
	            0x20, 0, 0, 16);
	/* A deleted file with the name searched for. */
	short_entry(new_entry(&fs, &root),
	            "\xe5"
	            "ONE    TXT",
	            0x20, 0, 0, 16);
	/* A long name with characters beyond ASCII: "é.txt". */
	long_entries(&fs, &root, "x.txt", "E~1     TXT");
	e = at(&fs, fs.root + 32 * (root.used - 1));
	store_le16(e + 1, 0x00e9);
	short_entry(new_entry(&fs, &root), "E~1     TXT", 0x20, 0, 0, 16);
	long_entries(&fs, &root, "x", "EURO~1     ");
	e = at(&fs, fs.root + 32 * (root.used - 1));
	store_le16(e + 1, 0x20ac);
	short_entry(new_entry(&fs, &root), "EURO~1     ", 0x20, 0, 0, 16);
	/* The longest name: 255 characters in 20 entries, in a directory. */
	sub = add_dir(&fs, &root, "SUB        ", NULL);
	memset(name, 'n', 255);
	name[255] = '\0';
	long_entries(&fs, &sub, name, "NNNNNN~1   ");
	short_entry(new_entry(&fs, &sub), "NNNNNN~1   ", 0x20, 0, 0, 16);
	v = open_fs(&fs);

	CHECK_EQ(find(v, "wrong-checksum.txt", &f), 0);
	CHECK_EQ(find(v, "wrong~1.txt", &f), 1);
	CHECK_EQ(find(v, "deleted-between.txt", &f), 0);
	CHECK_EQ(find(v, "delete~1.txt", &f), 1);
	CHECK_EQ(find(v, "a-name-of-two-entries", &f), 0);
	CHECK_EQ(find(v, "two~1", &f), 1);
	CHECK_EQ(find(v, "repeated-part-names", &f), 0);
	CHECK_EQ(find(v, "exactly-twenty-six-chars.x", &f), 1);
	CHECK_EQ(find(v,
	              "\xe5"
	              "abc.txt",
	              &f),
	         1);
	CHECK_EQ(find(v,
	              "\xe5"
	              "one.txt",
	              &f),
	         0);
	CHECK_EQ(find(v, "\xc3\xa9.txt", &f), 1);
	CHECK_EQ(find(v, "\xc3\x89.txt", &f), 0); /* É: only ASCII folds */
	CHECK_EQ(find(v, "\xe2\x82\xac", &f), 1);
	memcpy(name, "sub/", 4); /* then the 255 characters, in capitals */
	memset(name + 4, 'N', 255);
	name[259] = '\0';
	CHECK_EQ(find(v, name, &f), 1);
	name[258] = '\0';
	CHECK_EQ(find(v, name, &f), 0);
	fat_close(v);
	block_mem_free(&fs.m);
}

/* A directory of several clusters, without an end entry. */
static void test_long_directory(void)
{
	Fs fs;
	Dir root, dir;
	FatVolume *v;
	FatFile f;
	char name11[12];

	format(&fs, 32, 70000, 1);
	root = root_dir(&fs);
	dir = add_dir(&fs, &root, "DIR        ", NULL);
	for (unsigned i = 0; i < 40; i++) { /* 16 entries a cluster */
		snprintf(name11, sizeof(name11), "F%-7u   ", i);
		add_file(&fs, &dir, name11, NULL, "x", 1);
	}
	for (unsigned i = 0; i < 40; i++) {
		snprintf(name11, sizeof(name11), "F%-7u   ", i);
		add_file(&fs, &root, name11, NULL, "y", 1);
	}
	v = open_fs(&fs);
	CHECK_EQ(find(v, "dir/f39", &f), 1);
	CHECK_EQ(find(v, "f39", &f), 1);
	CHECK_EQ(find(v, "dir/f40", &f), 0); /* to the end of the chain */
	CHECK_EQ(find(v, "dir/..", &f), 1);
	CHECK(f.directory && !f.root);
	CHECK_EQ(f.cluster, 2); /* FAT32: the root's cluster */
	fat_close(v);
	block_mem_free(&fs.m);

	/* FAT16: the root ends after its entries, with no end entry. */
	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	for (unsigned i = 0; i < 64; i++) {
		snprintf(name11, sizeof(name11), "F%-7u   ", i);
		short_entry(new_entry(&fs, &root), name11, 0x20, 0, 0, 16);
	}
	v = open_fs(&fs);
	CHECK_EQ(find(v, "f63", &f), 1);
	CHECK_EQ(find(v, "f64", &f), 0);
	fat_close(v);
	block_mem_free(&fs.m);
}

/* Damaged chains, and reads that fail. */
static void test_damage(void)
{
	Fs fs;
	Dir root;
	FatVolume *v;
	FatFile f;
	uint32_t first;
	uint8_t buf[2048];
	char error[128];

	format(&fs, 12, 2048, 1);
	root = root_dir(&fs);
	first = add_file(&fs, &root, "FILE       ", NULL, buf, 2048); /* 4 */
	v = open_fs(&fs);
	CHECK_EQ(find(v, "file", &f), 1);

	set_fat(&fs, first + 1, 0); /* a free cluster in the chain */
	CHECK(!fat_read(v, &f, 0, buf, 2048, error, sizeof(error)));
	CHECK(strcmp(error, "the FAT entry of cluster 3, 0x0, is not a cluster") ==
	      0);
	set_fat(&fs, first + 1, 1);
	CHECK(!fat_read(v, &f, 0, buf, 2048, error, sizeof(error)));
	CHECK(strstr(error, "0x1, is not a cluster") != NULL);
	set_fat(&fs, first + 1, fs.clusters + 2);
	CHECK(!fat_read(v, &f, 0, buf, 2048, error, sizeof(error)));
	CHECK(strstr(error, "is not a cluster") != NULL);
	set_fat(&fs, first + 1, fs.clusters + 1); /* the last cluster: free */
	CHECK(!fat_read(v, &f, 0, buf, 2048, error, sizeof(error)));
	CHECK(strstr(error, "is not a cluster") != NULL);
	set_fat(&fs, first + 1, 0xff7); /* bad */
	CHECK(!fat_read(v, &f, 0, buf, 2048, error, sizeof(error)));
	CHECK(strcmp(error, "cluster 3 is marked bad") == 0);
	set_fat(&fs, first + 1, 0xff8); /* the end, too soon */
	CHECK(!fat_read(v, &f, 0, buf, 2048, error, sizeof(error)));
	CHECK(strcmp(error, "a cluster chain ends too soon") == 0);
	CHECK(fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	/*
	 * A loop: found once the chain is longer than there are clusters.
	 * Shorter reads cannot tell it from a chain that repeats clusters.
	 */
	set_fat(&fs, first + 1, first);
	f.size = 0xffffffff; /* so that the loop goes on */
	CHECK(!fat_read(v, &f, 0x7fffffff, buf, 1, error, sizeof(error)));
	CHECK(strcmp(error, "a cluster chain loops") == 0);
	set_fat(&fs, first + 1, first + 2);

	/* A directory entry that points outside the clusters. */
	short_entry(new_entry(&fs, &root), "BAD        ", 0x20, fs.clusters + 2, 1,
	            12);
	CHECK(fat_find(v, "bad", &f, error, sizeof(error)) == -1);
	CHECK(strstr(error, " does not exist") != NULL);
	short_entry(at(&fs, fs.root + 32 * (root.used - 1)), "BAD        ", 0x10, 1,
	            0, 12);
	CHECK(fat_find(v, "bad", &f, error, sizeof(error)) == -1);
	CHECK(strcmp(error, "cluster 1 does not exist") == 0);
	CHECK(fat_find(v, "bad/x", &f, error, sizeof(error)) == -1);

	/* Reads that fail. */
	CHECK_EQ(find(v, "file", &f), 1);
	fs.m.fail_from = fs.data + 1000;
	CHECK(!fat_read(v, &f, 0, buf, 2048, error, sizeof(error)));
	CHECK(strcmp(error, "cannot read cluster 3") == 0);
	fs.m.fail_from = fs.fat;
	fs.m.fail_to = fs.root;
	CHECK(!fat_read(v, &f, 0, buf, 2048, error, sizeof(error)));
	CHECK(strcmp(error, "cannot read the FAT") == 0);
	fs.m.fail_from = fs.root;
	fs.m.fail_to = fs.root + 1;
	CHECK(fat_find(v, "file", &f, error, sizeof(error)) == -1);
	CHECK(strcmp(error, "cannot read the root directory") == 0);
	fat_close(v);
	fs.m.fail_from = BASE + 100;
	fs.m.fail_to = UINT64_MAX;
	CHECK(fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	               sizeof(error)) == NULL);
	CHECK(strcmp(error, "cannot read the boot sector") == 0);
	CHECK(fat_open(&fs.m.dev, BASE, 511, error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "cannot read the boot sector") == 0);
	block_mem_free(&fs.m);

	/* FAT16 and FAT32 entries: bad and end marks of their own width. */
	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	first = add_file(&fs, &root, "FILE       ", NULL, buf, 1024);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "file", &f), 1);
	set_fat(&fs, first, 0xfff7);
	CHECK(!fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	CHECK(strcmp(error, "cluster 2 is marked bad") == 0);
	set_fat(&fs, first, 0xfff8);
	CHECK(!fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	CHECK(strcmp(error, "a cluster chain ends too soon") == 0);
	set_fat(&fs, first, 0xff8); /* a FAT12 end mark is a cluster here */
	CHECK(fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	fat_close(v);
	block_mem_free(&fs.m);

	format(&fs, 32, 70000, 1);
	root = root_dir(&fs);
	first = add_file(&fs, &root, "FILE       ", NULL, buf, 1024);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "file", &f), 1);
	CHECK_EQ(f.cluster, 3);
	set_fat(&fs, first, 0xf0000004); /* the top 4 bits do not count */
	CHECK(fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	set_fat(&fs, first, 0x0ffffff7);
	CHECK(!fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	CHECK(strcmp(error, "cluster 3 is marked bad") == 0);
	set_fat(&fs, first, 0xfff8); /* a FAT16 end mark is a cluster here */
	CHECK(fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	set_fat(&fs, first, 0x0ffffff0);
	CHECK(!fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	CHECK(strstr(error, "0xffffff0, is not a cluster") != NULL);
	fat_close(v);
	block_mem_free(&fs.m);
}

/* Clusters above 65535 need the high half of the cluster on FAT32. */
static void test_high_clusters(void)
{
	Fs fs;
	Dir root;
	FatVolume *v;
	FatFile f;
	uint8_t buf[4];
	char error[128];

	format(&fs, 32, 70000, 1);
	root = root_dir(&fs);
	fs.next = 0x10005;
	add_file(&fs, &root, "HIGH       ", NULL, "high", 4);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "high", &f), 1);
	CHECK_EQ(f.cluster, 0x10005);
	CHECK(fat_read(v, &f, 0, buf, 4, error, sizeof(error)));
	CHECK(memcmp(buf, "high", 4) == 0);
	fat_close(v);
	block_mem_free(&fs.m);

	/* FAT16 ignores the high half: it holds other things there. */
	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	add_file(&fs, &root, "FILE       ", NULL, "file", 4);
	store_le16(at(&fs, fs.root) + 0x14, 0x1234);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "file", &f), 1);
	CHECK_EQ(f.cluster, 2);
	fat_close(v);
	block_mem_free(&fs.m);
}

/* FAT32's active FAT: with mirroring off, only that copy counts. */
static void test_active_fat(void)
{
	Fs fs;
	Dir root;
	FatVolume *v;
	FatFile f;
	uint8_t buf[1024];
	char error[128];
	uint32_t first;

	format(&fs, 32, 70000, 1);
	root = root_dir(&fs);
	first = add_file(&fs, &root, "FILE       ", NULL, buf, 1024);
	/* Break the chain in FAT 0 only. */
	store_le32(at(&fs, fs.fat + 4 * first), 0);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "file", &f), 1);
	CHECK(!fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	fat_close(v);
	store_le16(at(&fs, BASE + 0x28), 0x81); /* FAT 1 is active */
	v = open_fs(&fs);
	CHECK(fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	fat_close(v);
	store_le16(at(&fs, BASE + 0x28), 0x01); /* mirrored: FAT 0 */
	v = open_fs(&fs);
	CHECK(!fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	fat_close(v);
	store_le16(at(&fs, BASE + 0x28), 0x82);
	CHECK(fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	               sizeof(error)) == NULL);
	CHECK(strcmp(error, "the active FAT, 2, does not exist") == 0);
	block_mem_free(&fs.m);
}

/* Boot sectors that are not those of a FAT file system. */
static void test_bad_boot_sector(void)
{
	static const struct {
		unsigned bits;
		unsigned offset, size;
		uint32_t value;
		const char *message;
	} cases[] = {
		{16, 0x0b, 2, 256, "not a FAT file system"},
		{16, 0x0b, 2, 8192, "not a FAT file system"},
		{16, 0x0b, 2, 768, "not a FAT file system"},
		{16, 0x0d, 1, 0, "not a FAT file system"},
		{16, 0x0d, 1, 3, "not a FAT file system"},
		{16, 0x0e, 2, 0, "not a FAT file system"},
		{16, 0x10, 1, 0, "not a FAT file system"},
		{16, 0x16, 2, 0, "not a FAT file system"},
		{16, 0x13, 2, 40, "not a FAT file system"},
		{16, 0x13, 2, 0, "not a FAT file system"},
		{16, 0x11, 2, 0, "a FAT16 file system without a root directory"},
		{16, 0x16, 2, 1, "the FAT is too small for the clusters"},
		{16, 0x13, 2, 16385, "the file system is larger than its partition"},
		{32, 0x11, 2, 16, "not a FAT32 file system of version 0"},
		{32, 0x16, 2, 1, "not a FAT32 file system of version 0"},
		{32, 0x2a, 2, 1, "not a FAT32 file system of version 0"},
		{32, 0x2c, 4, 1, "not a FAT32 file system of version 0"},
		{32, 0x2c, 4, 0x20000, "not a FAT32 file system of version 0"},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		Fs fs;
		uint8_t *b;
		char error[128] = "";

		format(&fs, cases[i].bits, cases[i].bits == 32 ? 70000 : 16384, 1);
		b = at(&fs, BASE + cases[i].offset);
		if (cases[i].size == 1)
			b[0] = (uint8_t)cases[i].value;
		else if (cases[i].size == 2)
			store_le16(b, (uint16_t)cases[i].value);
		else
			store_le32(b, cases[i].value);
		CHECK(fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
		               sizeof(error)) == NULL);
		if (strcmp(error, cases[i].message) != 0)
			fprintf(stderr, "case %u: %s\n", i, error);
		CHECK(strcmp(error, cases[i].message) == 0);
		block_mem_free(&fs.m);
	}
}

/* A boot sector of a file system with these parameters, at 0 of m. */
static void raw_boot_sector(BlockMem *m, uint32_t sector, uint32_t cluster,
                            uint32_t reserved, uint32_t fats,
                            uint32_t root_entries, uint32_t total,
                            uint32_t fat_size)
{
	uint8_t *b = m->data;

	memset(b, 0, 512);
	store_le16(b + 0x0b, (uint16_t)sector);
	b[0x0d] = (uint8_t)cluster;
	store_le16(b + 0x0e, (uint16_t)reserved);
	b[0x10] = (uint8_t)fats;
	store_le16(b + 0x11, (uint16_t)root_entries);
	if (total < 0x10000)
		store_le16(b + 0x13, (uint16_t)total);
	else
		store_le32(b + 0x20, total);
	store_le16(b + 0x16, (uint16_t)fat_size);
	b[510] = 0x55;
	b[511] = 0xaa;
}

/* Sizes at the limits: of sectors, clusters, the FAT and the data. */
static void test_geometry(void)
{
	static const struct {
		uint32_t sector, cluster, reserved, fats, root, total, fat_size;
		const char *error; /* NULL: opens */
		unsigned bits;
	} cases[] = {
		{4096, 1, 1, 2, 64, 2048, 1, NULL, 12},
		{512, 128, 1, 2, 64, 2048, 1, NULL, 12},
		{512, 1, 1, 1, 16, 4112, 16, NULL, 16}, /* exactly 4094 + 2 */
		{512, 1, 1, 1, 16, 4113, 16, "the FAT is too small", 0},
		{512, 1, 1, 2, 16, 6, 2, NULL, 12}, /* no data clusters */
		{512, 1, 2, 2, 16, 6, 2, "not a FAT file system", 0},
		{512, 1, 1, 3, 16, 6, 2, "not a FAT file system", 0},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		BlockMem m;
		char error[128] = "";
		FatVolume *v;
		uint64_t size = (uint64_t)cases[i].total * cases[i].sector;

		block_mem_init(&m, size < 4096 ? 4096 : size);
		raw_boot_sector(&m, cases[i].sector, cases[i].cluster,
		                cases[i].reserved, cases[i].fats, cases[i].root,
		                cases[i].total, cases[i].fat_size);
		v = fat_open(&m.dev, 0, size, error, sizeof(error));
		if (cases[i].error == NULL) {
			FatFile f;

			if (v == NULL)
				fprintf(stderr, "case %zu: %s\n", i, error);
			CHECK(v != NULL);
			CHECK_EQ(fat_bits(v), cases[i].bits);
			CHECK_EQ(fat_find(v, "x", &f, error, sizeof(error)), 0);
			fat_close(v);
		} else {
			CHECK(v == NULL);
			if (strstr(error, cases[i].error) == NULL)
				fprintf(stderr, "case %zu: %s\n", i, error);
			CHECK(strstr(error, cases[i].error) != NULL);
		}
		block_mem_free(&m);
	}

	/* A partition of the boot sector alone: read, then too small. */
	{
		Fs fs;
		char error[128];

		format(&fs, 16, 16384, 1);
		CHECK(fat_open(&fs.m.dev, BASE, 512, error, sizeof(error)) == NULL);
		CHECK(strcmp(error, "the file system is larger than its partition") ==
		      0);
		block_mem_free(&fs.m);
	}
}

/* FAT32's root cluster, version and active FAT at their limits. */
static void test_fat32_fields(void)
{
	Fs fs;
	FatVolume *v;
	char error[128];

	format(&fs, 32, 70000, 1);
	store_le32(at(&fs, BASE + 0x2c), fs.clusters + 1); /* the last cluster */
	v = open_fs(&fs);
	fat_close(v);
	store_le32(at(&fs, BASE + 0x2c), fs.clusters + 2);
	CHECK(fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	               sizeof(error)) == NULL);
	store_le32(at(&fs, BASE + 0x2c), 2);
	at(&fs, BASE + 0x2b)[0] = 1; /* version 1.0 */
	CHECK(fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	               sizeof(error)) == NULL);
	CHECK(strcmp(error, "not a FAT32 file system of version 0") == 0);
	at(&fs, BASE + 0x2b)[0] = 0;
	/* One FAT, and FAT 1 said to be the active one. */
	at(&fs, BASE + 0x10)[0] = 1;
	store_le16(at(&fs, BASE + 0x28), 0x81);
	CHECK(fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	               sizeof(error)) == NULL);
	CHECK(strcmp(error, "the active FAT, 1, does not exist") == 0);
	block_mem_free(&fs.m);
}

/* The first and last clusters, a chain of them all, a fragmented read. */
static void test_cluster_limits(void)
{
	Fs fs;
	Dir root;
	FatVolume *v;
	FatFile f;
	uint8_t *data, buf[1024];
	char error[128];
	uint32_t a, b;

	/* A file in the last two clusters. */
	format(&fs, 12, 2048, 1);
	root = root_dir(&fs);
	fs.next = fs.clusters;
	data = pattern(1024);
	add_file(&fs, &root, "LAST       ", NULL, data, 1024);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "last", &f), 1);
	CHECK_EQ(f.cluster, fs.clusters);
	CHECK(fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	CHECK(memcmp(buf, data, 1024) == 0);
	fat_close(v);
	free(data);
	block_mem_free(&fs.m);

	/* A file in the last cluster alone; a chain that runs past it. */
	format(&fs, 12, 2048, 1);
	root = root_dir(&fs);
	fs.next = fs.clusters + 1;
	add_file(&fs, &root, "END        ", NULL, "end", 3);
	fs.next = 2;
	data = pattern(1024);
	a = add_file(&fs, &root, "PAST       ", NULL, data, 1024);
	free(data);
	set_fat(&fs, a, fs.clusters + 2);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "end", &f), 1);
	CHECK_EQ(f.cluster, fs.clusters + 1);
	CHECK(fat_read(v, &f, 0, buf, 3, error, sizeof(error)));
	CHECK(memcmp(buf, "end", 3) == 0);
	CHECK_EQ(find(v, "past", &f), 1);
	CHECK(!fat_read(v, &f, 0, buf, 1024, error, sizeof(error)));
	CHECK(strstr(error, "is not a cluster") != NULL);
	fat_close(v);
	block_mem_free(&fs.m);

	/* A file in every cluster: a chain that long is no loop. */
	format(&fs, 12, 2048, 1);
	root = root_dir(&fs);
	data = pattern(fs.clusters * 512);
	add_file(&fs, &root, "ALL        ", NULL, data, fs.clusters * 512);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "all", &f), 1);
	{
		uint8_t *back = malloc(f.size);

		CHECK(fat_read(v, &f, 0, back, f.size, error, sizeof(error)));
		CHECK(memcmp(back, data, f.size) == 0);
		free(back);
	}
	fat_close(v);
	free(data);
	block_mem_free(&fs.m);

	/* A chain that jumps: its second cluster is not the next one. */
	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	data = pattern(1024);
	a = add_file(&fs, &root, "JUMP       ", NULL, data, 1024);
	b = add_file(&fs, &root, "OTHER      ", NULL, "other", 5);
	set_fat(&fs, a, b); /* JUMP: a, then OTHER's cluster */
	v = open_fs(&fs);
	CHECK_EQ(find(v, "jump", &f), 1);
	CHECK(fat_read(v, &f, 512, buf, 5, error, sizeof(error)));
	CHECK(memcmp(buf, "other", 5) == 0);
	/* A file that does not come from fat_find: its cluster is checked. */
	CHECK(!fat_read(v, &(FatFile){.cluster = 0, .size = 10}, 0, buf, 10, error,
	                sizeof(error)));
	CHECK(strcmp(error, "cluster 0 does not exist") == 0);
	fat_close(v);
	free(data);
	block_mem_free(&fs.m);
}

/* The ends of directories: full ones, an end entry, a file as a path. */
static void test_directory_ends(void)
{
	Fs fs;
	Dir root, dir;
	FatVolume *v;
	FatFile f;
	char name11[12];
	uint8_t fake[32];
	char error[128];

	/* What follows a full FAT16 root looks like an entry: not read. */
	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	memset(fake, 0, sizeof(fake));
	short_entry(fake, "F64        ", 0x20, 0, 0, 16);
	for (unsigned i = 0; i < 64; i++) {
		snprintf(name11, sizeof(name11), "F%-7u   ", i);
		if (i == 0) /* the first file holds the fake entry, in cluster 2 */
			add_file(&fs, &root, name11, NULL, fake, 32);
		else
			short_entry(new_entry(&fs, &root), name11, 0x20, 0, 0, 16);
	}
	v = open_fs(&fs);
	CHECK_EQ(find(v, "f63", &f), 1);
	CHECK_EQ(find(v, "f64", &f), 0);
	/* A file is not a directory, even if it holds entries. */
	CHECK_EQ(find(v, "f0/f64", &f), 0);
	fat_close(v);
	block_mem_free(&fs.m);

	/* Directories of exactly one cluster, on FAT16 and FAT32. */
	for (unsigned bits = 16; bits <= 32; bits += 16) {
		format(&fs, bits, bits == 32 ? 70000 : 16384, 1);
		root = root_dir(&fs);
		dir = add_dir(&fs, &root, "DIR        ", NULL);
		for (unsigned i = 0; i < 14; i++) { /* with "." and "..": 16 */
			snprintf(name11, sizeof(name11), "F%-7u   ", i);
			short_entry(new_entry(&fs, &dir), name11, 0x20, 0, 0, bits);
		}
		v = open_fs(&fs);
		CHECK_EQ(find(v, "dir/f13", &f), 1);
		CHECK_EQ(find(v, "dir/f14", &f), 0);
		/* A read that fails in the directory is an error. */
		fs.m.fail_from = fs.data;
		fs.m.fail_to = UINT64_MAX;
		CHECK_EQ(fat_find(v, "dir/f13", &f, error, sizeof(error)), -1);
		fs.m.fail_from = fs.m.fail_to = UINT64_MAX;
		fat_close(v);
		block_mem_free(&fs.m);
	}

	/* Nothing counts after the end entry. */
	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	short_entry(new_entry(&fs, &root), "BEFORE     ", 0x20, 0, 0, 16);
	memset(new_entry(&fs, &root), 0, 32);
	short_entry(new_entry(&fs, &root), "AFTER      ", 0x20, 0, 0, 16);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "before", &f), 1);
	CHECK_EQ(find(v, "after", &f), 0);
	fat_close(v);
	block_mem_free(&fs.m);
}

/* Short names: attributes, extensions and case. */
static void test_short_names(void)
{
	Fs fs;
	Dir root;
	FatVolume *v;
	FatFile f;

	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	add_file(&fs, &root, "CONFIG  TXT", NULL, "x", 1);
	short_entry(new_entry(&fs, &root), "ABC     TXT", 0x01, 0, 0, 16); /* RO */
	short_entry(new_entry(&fs, &root), "A       B  ", 0x00, 0, 0, 16);
	short_entry(new_entry(&fs, &root), "ZZZ        ", 0x00, 0, 0, 16);
	v = open_fs(&fs);
	CHECK_EQ(find(v, "abc.txt", &f), 1);
	CHECK(!f.directory);
	CHECK_EQ(find(v, "a.b", &f), 1);
	CHECK_EQ(find(v, "zzz", &f), 1);
	CHECK_EQ(find(v, "xonfig.txt", &f), 0);
	CHECK_EQ(find(v, "config.txx", &f), 0);
	fat_close(v);
	block_mem_free(&fs.m);
}

/* Write the long-name entry seq of chars into e, for a short name. */
static void lfn(uint8_t *e, unsigned seq, const uint16_t *chars, size_t n,
                const char name11[11])
{
	static const uint8_t offsets[13] = {1,  3,  5,  7,  9,  14, 16,
	                                    18, 20, 22, 24, 28, 30};

	memset(e, 0, 32);
	e[0] = (uint8_t)seq;
	e[11] = 0x0f;
	e[13] = checksum(name11);
	for (unsigned i = 0; i < 13; i++)
		store_le16(e + offsets[i], i < n ? chars[i] : i == n ? 0 : 0xffff);
}

/* Long names at their limits, broken ones, and UTF-8 of every length. */
static void test_long_name_limits(void)
{
	static const uint16_t utf[] = {0x7f, 0x80, 0x7ff, 0x800, 0x801, 0xfffd};
	static const uint16_t a13[13] = {'a', 'a', 'a', 'a', 'a', 'a', 'a',
	                                 'a', 'a', 'a', 'a', 'a', 'a'};
	static const uint16_t b[] = {'b', 'b'}, c[] = {'c', 'c'}, d[] = {'d', 'd'};
	Fs fs;
	Dir root;
	FatVolume *v;
	FatFile f;
	char name[800];
	uint16_t wide[200];

	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	/* UTF-8 of 1, 2 and 3 bytes, at the edges. */
	lfn(new_entry(&fs, &root), 0x41, utf, 6, "UTF~1      ");
	short_entry(new_entry(&fs, &root), "UTF~1      ", 0x20, 0, 0, 16);
	/* 200 characters of 3 bytes. */
	for (unsigned i = 0; i < 200; i++)
		wide[i] = 0x4e00;
	for (unsigned p = 16; p >= 1; p--)
		lfn(new_entry(&fs, &root), p | (p == 16 ? 0x40 : 0),
		    wide + 13 * (p - 1), p == 16 ? 200 - 13 * 15 : 13, "WIDE~1     ");
	short_entry(new_entry(&fs, &root), "WIDE~1     ", 0x20, 0, 0, 16);
	/* After a name of 2 parts, one of exactly 13 characters. */
	lfn(new_entry(&fs, &root), 0x42, c, 2, "TWO~1      ");
	lfn(new_entry(&fs, &root), 0x01, a13, 13, "TWO~1      ");
	short_entry(new_entry(&fs, &root), "TWO~1      ", 0x20, 0, 0, 16);
	lfn(new_entry(&fs, &root), 0x41, a13, 13, "THIRTE~1   ");
	short_entry(new_entry(&fs, &root), "THIRTE~1   ", 0x20, 0, 0, 16);
	/* Sequence numbers 0, 21 and 25 are not long names. */
	lfn(new_entry(&fs, &root), 0x40, b, 2, "ZERO~1     ");
	short_entry(new_entry(&fs, &root), "ZERO~1     ", 0x20, 0, 0, 16);
	lfn(new_entry(&fs, &root), 0x59, b, 2, "BIG~1      ");
	short_entry(new_entry(&fs, &root), "BIG~1      ", 0x20, 0, 0, 16);
	for (unsigned p = 21; p >= 1; p--)
		lfn(new_entry(&fs, &root), p | (p == 21 ? 0x40 : 0), a13, 13,
		    "MANY~1     ");
	short_entry(new_entry(&fs, &root), "MANY~1     ", 0x20, 0, 0, 16);
	/* Part 2 twice, then part 1: no long name. */
	lfn(new_entry(&fs, &root), 0x42, b, 2, "TWICE~1    ");
	lfn(new_entry(&fs, &root), 0x02, d, 2, "TWICE~1    ");
	lfn(new_entry(&fs, &root), 0x01, a13, 13, "TWICE~1    ");
	short_entry(new_entry(&fs, &root), "TWICE~1    ", 0x20, 0, 0, 16);
	/* A stray entry in the middle: no long name either. */
	lfn(new_entry(&fs, &root), 0x42, b, 2, "STRAY~1    ");
	lfn(new_entry(&fs, &root), 0x05, d, 2, "STRAY~1    ");
	lfn(new_entry(&fs, &root), 0x01, a13, 13, "STRAY~1    ");
	short_entry(new_entry(&fs, &root), "STRAY~1    ", 0x20, 0, 0, 16);
	/* A second part whose checksum is another name's: no long name. */
	lfn(new_entry(&fs, &root), 0x42, b, 2, "CHK~1      ");
	lfn(new_entry(&fs, &root), 0x01, a13, 13, "OTHER~1    ");
	short_entry(new_entry(&fs, &root), "CHK~1      ", 0x20, 0, 0, 16);
	/* A volume label between a long name and its entry. */
	lfn(new_entry(&fs, &root), 0x41, c, 2, "LABEL~1    ");
	short_entry(new_entry(&fs, &root), "VOLUME     ", 0x08, 0, 0, 16);
	short_entry(new_entry(&fs, &root), "LABEL~1    ", 0x20, 0, 0, 16);
	v = open_fs(&fs);

	CHECK_EQ(
		find(v, "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xe0\xa0\x81\xef\xbf\xbd", &f),
		1);
	name[0] = '\0';
	for (unsigned i = 0; i < 200; i++)
		strcat(name, "\xe4\xb8\x80");
	CHECK_EQ(find(v, name, &f), 1);
	CHECK_EQ(find(v, "aaaaaaaaaaaaacc", &f), 1);
	CHECK_EQ(find(v, "aaaaaaaaaaaaa", &f), 1);
	CHECK_EQ(find(v, "bb", &f), 0);
	CHECK_EQ(find(v, "zero~1", &f), 1);
	CHECK_EQ(find(v, "big~1", &f), 1);
	memset(name, 'a', 273);
	name[273] = '\0';
	CHECK_EQ(find(v, name, &f), 0);
	CHECK_EQ(find(v, "many~1", &f), 1);
	CHECK_EQ(find(v, "aaaaaaaaaaaaadd", &f), 0);
	CHECK_EQ(find(v, "aaaaaaaaaaaaabb", &f), 0);
	CHECK_EQ(find(v, "twice~1", &f), 1);
	CHECK_EQ(find(v, "stray~1", &f), 1);
	CHECK_EQ(find(v, "chk~1", &f), 1);
	CHECK_EQ(find(v, "cc", &f), 0);
	CHECK_EQ(find(v, "label~1", &f), 1);
	fat_close(v);
	block_mem_free(&fs.m);
}

/*
 * Each read that a lookup and a read of a file make, when it fails, is
 * an error: on FAT12, FAT16 and FAT32, in a directory.
 */
static void test_read_failures(void)
{
	static const struct {
		unsigned bits;
		uint32_t total;
	} types[] = {{12, 2048}, {16, 16384}, {32, 70000}};

	for (size_t t = 0; t < ARRAY_SIZE(types); t++) {
		Fs fs;
		Dir root, dir;
		FatVolume *v;
		FatFile f;
		uint8_t *data = pattern(2000), buf[2000];
		char error[128];
		unsigned reads;

		format(&fs, types[t].bits, types[t].total, 1);
		root = root_dir(&fs);
		dir = add_dir(&fs, &root, "DIR        ", "dir");
		add_file(&fs, &dir, "FILE    BIN", "file.bin", data, 2000);
		v = open_fs(&fs);
		fs.m.reads = 0;
		CHECK_EQ(find(v, "dir/file.bin", &f), 1);
		CHECK(fat_read(v, &f, 0, buf, 2000, error, sizeof(error)));
		reads = fs.m.reads;
		for (unsigned n = 0; n < reads; n++) {
			int r;

			fs.m.reads = 0;
			fs.m.fail_read = n;
			r = fat_find(v, "dir/file.bin", &f, error, sizeof(error));
			if (r == 1)
				CHECK(!fat_read(v, &f, 0, buf, 2000, error, sizeof(error)));
			else
				CHECK_EQ(r, -1);
			CHECK(strstr(error, "cannot read") != NULL);
		}
		fs.m.fail_read = UINT_MAX;
		fat_close(v);
		free(data);
		block_mem_free(&fs.m);
	}
}

int main(void)
{
	RUN_TEST(test_fat12);
	RUN_TEST(test_fat16);
	RUN_TEST(test_fat32);
	RUN_TEST(test_type_limits);
	RUN_TEST(test_long_names);
	RUN_TEST(test_long_directory);
	RUN_TEST(test_damage);
	RUN_TEST(test_high_clusters);
	RUN_TEST(test_active_fat);
	RUN_TEST(test_bad_boot_sector);
	RUN_TEST(test_large_clusters);
	RUN_TEST(test_geometry);
	RUN_TEST(test_fat32_fields);
	RUN_TEST(test_cluster_limits);
	RUN_TEST(test_directory_ends);
	RUN_TEST(test_short_names);
	RUN_TEST(test_long_name_limits);
	RUN_TEST(test_read_failures);
	return test_summary();
}
