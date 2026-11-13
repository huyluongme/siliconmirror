/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * FAT file systems built in memory for the unit tests, as ECMA-107 and
 * Wikipedia's "File Allocation Table" lay them out: format one, then
 * add files and directories, with long names or without.
 */

#ifndef TESTS_UNIT_FAT_IMAGE_H
#define TESTS_UNIT_FAT_IMAGE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "block_mem.h"
#include "util/byteorder.h"

#define SECTOR 512
#define BASE (8 * SECTOR) /* where the file system starts on the disk */

typedef struct Fs {
	BlockMem m;
	unsigned bits;
	uint32_t cluster_sectors, reserved, fats, fat_sectors, root_entries;
	uint32_t total, clusters;
	uint64_t fat, root, data; /* offsets on the disk */
	uint32_t next;            /* the next free cluster */
} Fs;

/* A directory being filled: the root of FAT12/16, or a cluster chain. */
typedef struct Dir {
	bool root;
	uint32_t cluster;
	unsigned used; /* entries */
} Dir;

static inline uint8_t *at(Fs *fs, uint64_t offset)
{
	return fs->m.data + offset;
}

static inline uint32_t cluster_size(const Fs *fs)
{
	return fs->cluster_sectors * SECTOR;
}

static inline uint8_t *cluster(Fs *fs, uint32_t n)
{
	return at(fs, fs->data + (uint64_t)(n - 2) * cluster_size(fs));
}

/* Set the FAT entry of cluster n, in every copy of the FAT. */
static inline void set_fat(Fs *fs, uint32_t n, uint32_t value)
{
	for (uint32_t i = 0; i < fs->fats; i++) {
		uint8_t *fat = at(fs, fs->fat + (uint64_t)i * fs->fat_sectors * SECTOR);

		if (fs->bits == 12) {
			uint8_t *e = fat + n + n / 2;
			uint16_t old = load_le16(e);

			store_le16(e, n & 1 ? (uint16_t)((old & 0x000f) | value << 4)
			                    : (uint16_t)((old & 0xf000) | value));
		} else if (fs->bits == 16) {
			store_le16(fat + 2 * n, (uint16_t)value);
		} else {
			store_le32(fat + 4 * n, value);
		}
	}
}

static inline uint32_t get_fat(Fs *fs, uint32_t n)
{
	uint8_t *fat = at(fs, fs->fat);

	if (fs->bits == 12)
		return n & 1 ? load_le16(fat + n + n / 2) >> 4
		             : load_le16(fat + n + n / 2) & 0xfff;
	if (fs->bits == 16)
		return load_le16(fat + 2 * n);
	return load_le32(fat + 4 * n) & 0x0fffffff;
}

static inline uint32_t end_mark(const Fs *fs)
{
	return fs->bits == 12 ? 0xfff : fs->bits == 16 ? 0xffff : 0x0fffffff;
}

/* A chain of count new clusters; returns the first. */
static inline uint32_t chain(Fs *fs, uint32_t count)
{
	uint32_t first = fs->next;

	for (uint32_t i = 0; i < count; i++)
		set_fat(fs, fs->next + i,
		        i + 1 < count ? fs->next + i + 1 : end_mark(fs));
	fs->next += count;
	return first;
}

/*
 * Format a file system of total sectors at BASE, on a disk with room
 * for it. root_cluster is where FAT32 keeps its root directory.
 */
static inline void format(Fs *fs, unsigned bits, uint32_t total,
                          uint32_t cluster_sectors)
{
	uint8_t *b;
	uint32_t entries = total / cluster_sectors + 2;

	memset(fs, 0, sizeof(*fs));
	/* A disk of whole 512 KB, as an SD card is. */
	block_mem_init(&fs->m, (BASE + (uint64_t)total * SECTOR + 0x7ffff) &
	                           ~(uint64_t)0x7ffff);
	fs->bits = bits;
	fs->total = total;
	fs->cluster_sectors = cluster_sectors;
	fs->reserved = bits == 32 ? 32 : 1;
	fs->fats = 2;
	fs->root_entries = bits == 32 ? 0 : 64;
	fs->fat_sectors = (entries * bits / 8 + 2 + SECTOR - 1) / SECTOR;
	fs->fat = BASE + (uint64_t)fs->reserved * SECTOR;
	fs->root = fs->fat + (uint64_t)fs->fats * fs->fat_sectors * SECTOR;
	fs->data = fs->root + (uint64_t)fs->root_entries * 32;
	fs->clusters = (uint32_t)((BASE + (uint64_t)total * SECTOR - fs->data) /
	                          cluster_size(fs));
	fs->next = 2;

	b = at(fs, BASE);
	b[0] = 0xeb;
	b[1] = 0x3c;
	b[2] = 0x90;
	memcpy(b + 3, "MSWIN4.1", 8);
	store_le16(b + 0x0b, SECTOR);
	b[0x0d] = (uint8_t)cluster_sectors;
	store_le16(b + 0x0e, (uint16_t)fs->reserved);
	b[0x10] = (uint8_t)fs->fats;
	store_le16(b + 0x11, (uint16_t)fs->root_entries);
	if (total < 0x10000 && bits != 32)
		store_le16(b + 0x13, (uint16_t)total);
	else
		store_le32(b + 0x20, total);
	b[0x15] = 0xf8;
	if (bits == 32) {
		store_le32(b + 0x24, fs->fat_sectors);
		store_le32(b + 0x2c, 2); /* the root directory's cluster */
	} else {
		store_le16(b + 0x16, (uint16_t)fs->fat_sectors);
	}
	b[510] = 0x55;
	b[511] = 0xaa;
	set_fat(fs, 0, end_mark(fs) & ~7u); /* the media byte */
	set_fat(fs, 1, end_mark(fs));
	if (bits == 32)
		chain(fs, 1);
}

static inline Dir root_dir(const Fs *fs)
{
	return fs->bits == 32 ? (Dir){false, 2, 0} : (Dir){true, 0, 0};
}

/* The next free entry of dir; a full directory gets one more cluster. */
static inline uint8_t *new_entry(Fs *fs, Dir *dir)
{
	uint32_t per_cluster = cluster_size(fs) / 32, n = dir->cluster;
	unsigned i = dir->used++;

	if (dir->root)
		return at(fs, fs->root + 32 * (uint64_t)i);
	for (unsigned k = 0; k < i / per_cluster; k++) {
		uint32_t next = get_fat(fs, n);

		if (next >= (end_mark(fs) & ~7u)) {
			set_fat(fs, n, fs->next);
			chain(fs, 1);
			next = fs->next - 1;
		}
		n = next;
	}
	return cluster(fs, n) + 32 * (i % per_cluster);
}

static inline void short_entry(uint8_t *e, const char name11[11], uint8_t attr,
                               uint32_t first, uint32_t size, unsigned bits)
{
	memcpy(e, name11, 11);
	e[11] = attr;
	if (bits == 32)
		store_le16(e + 0x14, (uint16_t)(first >> 16));
	store_le16(e + 0x1a, (uint16_t)first);
	store_le32(e + 0x1c, size);
}

static inline uint8_t checksum(const char name11[11])
{
	uint8_t sum = 0;

	for (int i = 0; i < 11; i++)
		sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)name11[i]);
	return sum;
}

/* The entries of a long name, last part first; returns the first one. */
static inline void long_entries(Fs *fs, Dir *dir, const char *name,
                                const char name11[11])
{
	static const uint8_t offsets[13] = {1,  3,  5,  7,  9,  14, 16,
	                                    18, 20, 22, 24, 28, 30};
	size_t len = strlen(name);
	unsigned parts = (unsigned)((len + 12) / 13);

	for (unsigned p = parts; p >= 1; p--) {
		uint8_t *e = new_entry(fs, dir);

		e[0] = (uint8_t)(p | (p == parts ? 0x40 : 0));
		e[11] = 0x0f;
		e[13] = checksum(name11);
		for (unsigned i = 0; i < 13; i++) {
			size_t k = (p - 1) * 13 + i;
			uint16_t c = k < len ? (uint8_t)name[k] : k == len ? 0 : 0xffff;

			store_le16(e + offsets[i], c);
		}
	}
}

/* Add a file with data, and a long name unless long_name is NULL. */
static inline uint32_t add_file(Fs *fs, Dir *dir, const char name11[11],
                                const char *long_name, const void *data,
                                uint32_t size)
{
	uint32_t count = (size + cluster_size(fs) - 1) / cluster_size(fs);
	uint32_t first = count ? chain(fs, count) : 0, n = first;
	const uint8_t *src = data;

	if (long_name != NULL)
		long_entries(fs, dir, long_name, name11);
	short_entry(new_entry(fs, dir), name11, 0x20, first, size, fs->bits);
	for (uint32_t done = 0; done < size; done += cluster_size(fs)) {
		uint32_t piece =
			size - done < cluster_size(fs) ? size - done : cluster_size(fs);

		memcpy(cluster(fs, n), src + done, piece);
		n = get_fat(fs, n);
	}
	return first;
}

/* Add a directory with its "." and ".." entries. */
static inline Dir add_dir(Fs *fs, Dir *parent, const char name11[11],
                          const char *long_name)
{
	Dir dir = {false, chain(fs, 1), 0};
	uint32_t up = parent->root ? 0 : parent->cluster;

	if (fs->bits == 32 && up == 2)
		up = 0; /* ".." of a directory in the root is 0, also on FAT32 */
	if (long_name != NULL)
		long_entries(fs, parent, long_name, name11);
	short_entry(new_entry(fs, parent), name11, 0x10, dir.cluster, 0, fs->bits);
	short_entry(new_entry(fs, &dir), ".          ", 0x10, dir.cluster, 0,
	            fs->bits);
	short_entry(new_entry(fs, &dir), "..         ", 0x10, up, 0, fs->bits);
	return dir;
}

/* A partition table whose first partition is the file system. */
static inline void add_mbr(Fs *fs, uint8_t type)
{
	uint8_t *e = at(fs, 446);

	e[4] = type;
	store_le32(e + 8, BASE / SECTOR);
	store_le32(e + 12, fs->total);
	fs->m.data[510] = 0x55;
	fs->m.data[511] = 0xaa;
}

#endif /* TESTS_UNIT_FAT_IMAGE_H */
