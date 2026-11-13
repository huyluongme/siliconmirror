/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Reading a FAT file system; see fat.h. */

#include "boot/fat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/byteorder.h"
#include "util/macros.h"

/* The boot sector's parameters (ECMA-107 9.2; Wikipedia for FAT32). */
enum {
	BPB_SECTOR_SIZE = 0x0b,
	BPB_CLUSTER_SECTORS = 0x0d,
	BPB_RESERVED = 0x0e,
	BPB_NUM_FATS = 0x10,
	BPB_ROOT_ENTRIES = 0x11,
	BPB_TOTAL16 = 0x13,
	BPB_FAT_SIZE16 = 0x16,
	BPB_TOTAL32 = 0x20,
	BPB_FAT_SIZE32 = 0x24,
	BPB_EXT_FLAGS = 0x28,
	BPB_VERSION = 0x2a,
	BPB_ROOT_CLUSTER = 0x2c,
};

/* A directory entry (ECMA-107 11.3; Wikipedia for FAT32 and VFAT). */
enum {
	DIR_NAME = 0x00,
	DIR_ATTR = 0x0b,
	DIR_CLUSTER_HI = 0x14, /* FAT32 only */
	DIR_CLUSTER = 0x1a,
	DIR_SIZE = 0x1c,
	DIR_ENTRY_SIZE = 32,
	LFN_CHECKSUM = 0x0d,
};

#define ATTR_VOLUME 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LFN 0x0f /* read-only, hidden, system and volume label */
#define LFN_LAST 0x40 /* in the sequence number */
#define LFN_CHARS 13  /* name characters in an entry */
#define LFN_MAX 20    /* entries: 255 characters */
#define FREE_NAME 0xe5
#define END_NAME 0x00

struct FatVolume {
	BlockDevice *dev;
	uint64_t offset;       /* of the file system on dev */
	unsigned bits;         /* 12, 16 or 32 */
	uint32_t cluster_size; /* bytes */
	uint64_t fat;          /* the FAT in use, from offset */
	uint64_t root;         /* FAT12 and FAT16: the root directory */
	uint32_t root_size;    /* its bytes */
	uint64_t data;         /* cluster 2 */
	uint32_t clusters;     /* data clusters: 2 to clusters + 1 */
	uint32_t root_cluster; /* FAT32 */
};

static bool fail(char *error, size_t error_size, const char *fmt, ...)
	PRINTF_FORMAT(3, 4);

static bool fail(char *error, size_t error_size, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(error, error_size, fmt, ap);
	va_end(ap);
	return false;
}

static bool is_power_of_2(uint32_t n)
{
	return n != 0 && (n & (n - 1)) == 0;
}

FatVolume *fat_open(BlockDevice *dev, uint64_t offset, uint64_t size,
                    char *error, size_t error_size)
{
	uint8_t b[512];
	uint32_t sector, cluster_sectors, reserved, fats, fat_size, total;
	uint32_t root_entries, root_sectors, system;
	uint64_t fat_entries;
	FatVolume *v;

	if (size < sizeof(b) || !block_read(dev, offset, b, sizeof(b))) {
		fail(error, error_size, "cannot read the boot sector");
		return NULL;
	}
	sector = load_le16(b + BPB_SECTOR_SIZE);
	cluster_sectors = b[BPB_CLUSTER_SECTORS];
	reserved = load_le16(b + BPB_RESERVED);
	fats = b[BPB_NUM_FATS];
	root_entries = load_le16(b + BPB_ROOT_ENTRIES);
	total = load_le16(b + BPB_TOTAL16);
	if (total == 0)
		total = load_le32(b + BPB_TOTAL32);
	fat_size = load_le16(b + BPB_FAT_SIZE16);
	if (fat_size == 0)
		fat_size = load_le32(b + BPB_FAT_SIZE32);
	if (sector < 512 || sector > 4096 || !is_power_of_2(sector) ||
	    cluster_sectors > 128 || !is_power_of_2(cluster_sectors) ||
	    reserved == 0 || fats == 0 || fat_size == 0) {
		fail(error, error_size, "not a FAT file system");
		return NULL;
	}
	root_sectors = (root_entries * DIR_ENTRY_SIZE + sector - 1) / sector;
	system = reserved + root_sectors;
	if ((uint64_t)fats * fat_size > total ||
	    system > total - (uint64_t)fats * fat_size) {
		fail(error, error_size, "not a FAT file system");
		return NULL;
	}
	system += fats * fat_size;
	if ((uint64_t)total * sector > size) {
		fail(error, error_size, "the file system is larger than its partition");
		return NULL;
	}
	v = calloc(1, sizeof(*v));
	if (v == NULL) {
		fail(error, error_size, "out of memory");
		return NULL;
	}
	v->dev = dev;
	v->offset = offset;
	v->cluster_size = sector * cluster_sectors;
	v->clusters = (total - system) / cluster_sectors;
	/* The number of clusters alone decides the type. */
	v->bits = v->clusters < 4085 ? 12 : v->clusters < 65525 ? 16 : 32;
	v->fat = (uint64_t)reserved * sector;
	v->root = v->fat + (uint64_t)fats * fat_size * sector;
	v->root_size = root_entries * DIR_ENTRY_SIZE;
	v->data = (uint64_t)system * sector;
	if (v->bits == 32) {
		uint16_t flags = load_le16(b + BPB_EXT_FLAGS);

		v->root_cluster = load_le32(b + BPB_ROOT_CLUSTER);
		if (root_entries != 0 || load_le16(b + BPB_FAT_SIZE16) != 0 ||
		    load_le16(b + BPB_VERSION) != 0 || v->root_cluster < 2 ||
		    v->root_cluster > v->clusters + 1) {
			fail(error, error_size, "not a FAT32 file system of version 0");
			fat_close(v);
			return NULL;
		}
		/* Bit 7: only the FAT in bits 3:0 is in use. */
		if (flags & 0x80) {
			if ((flags & 0xf) >= fats) {
				fail(error, error_size, "the active FAT, %u, does not exist",
				     flags & 0xf);
				fat_close(v);
				return NULL;
			}
			v->fat += (uint64_t)(flags & 0xf) * fat_size * sector;
		}
	} else if (root_entries == 0) {
		fail(error, error_size, "a FAT%u file system without a root directory",
		     v->bits);
		fat_close(v);
		return NULL;
	}
	fat_entries = (uint64_t)fat_size * sector * 8 / v->bits;
	if (fat_entries < (uint64_t)v->clusters + 2) {
		fail(error, error_size, "the FAT is too small for the clusters");
		fat_close(v);
		return NULL;
	}
	return v;
}

void fat_close(FatVolume *v)
{
	free(v);
}

unsigned fat_bits(const FatVolume *v)
{
	return v->bits;
}

/* The FAT entry of cluster n. */
static bool fat_entry(FatVolume *v, uint32_t n, uint32_t *value, char *error,
                      size_t error_size)
{
	uint8_t b[4];
	uint64_t at = v->offset + v->fat;

	switch (v->bits) {
	case 12:
		at += n + n / 2;
		if (!block_read(v->dev, at, b, 2))
			break;
		*value = load_le16(b);
		*value = n & 1 ? *value >> 4 : *value & 0xfff;
		return true;
	case 16:
		if (!block_read(v->dev, at + 2 * (uint64_t)n, b, 2))
			break;
		*value = load_le16(b);
		return true;
	default:
		if (!block_read(v->dev, at + 4 * (uint64_t)n, b, 4))
			break;
		*value = load_le32(b) & 0x0fffffff; /* FAT32 uses 28 bits */
		return true;
	}
	/* false here, not fail()'s: compilers then see *value set on true. */
	fail(error, error_size, "cannot read the FAT");
	return false;
}

/*
 * The cluster after cluster in its chain, or 0 at the end of the chain.
 * A free or bad cluster, or one that does not exist, is an error.
 */
static bool next_cluster(FatVolume *v, uint32_t cluster, uint32_t *next,
                         char *error, size_t error_size)
{
	uint32_t value, end = v->bits == 12   ? 0xff8
	                      : v->bits == 16 ? 0xfff8
	                                      : 0x0ffffff8;

	if (!fat_entry(v, cluster, &value, error, error_size))
		return false;
	if (value >= end) {
		*next = 0;
		return true;
	}
	if (value == end - 1)
		return fail(error, error_size, "cluster %u is marked bad",
		            (unsigned)cluster);
	if (value < 2 || value > v->clusters + 1)
		return fail(error, error_size,
		            "the FAT entry of cluster %u, 0x%x, is not a cluster",
		            (unsigned)cluster, (unsigned)value);
	*next = value;
	return true;
}

static bool valid_cluster(FatVolume *v, uint32_t cluster, char *error,
                          size_t error_size)
{
	if (cluster >= 2 && cluster <= v->clusters + 1)
		return true;
	return fail(error, error_size, "cluster %u does not exist",
	            (unsigned)cluster);
}

/*
 * Read len bytes at offset of the chain that starts at cluster. With
 * at_end, set *at_end instead of failing when the chain ends first.
 */
static bool read_chain(FatVolume *v, uint32_t cluster, uint64_t offset,
                       uint8_t *buf, size_t len, bool *at_end, char *error,
                       size_t error_size)
{
	/* A chain longer than the number of clusters loops. */
	uint64_t steps = 0;

	if (!valid_cluster(v, cluster, error, error_size))
		return false;
	while (len > 0) {
		if (offset >= v->cluster_size) {
			offset -= v->cluster_size;
		} else {
			size_t n = v->cluster_size - offset;

			if (n > len)
				n = len;
			if (!block_read(v->dev,
			                v->offset + v->data +
			                    (uint64_t)(cluster - 2) * v->cluster_size +
			                    offset,
			                buf, n))
				return fail(error, error_size, "cannot read cluster %u",
				            (unsigned)cluster);
			buf += n;
			len -= n;
			offset = 0;
			if (len == 0)
				break;
		}
		if (!next_cluster(v, cluster, &cluster, error, error_size))
			return false;
		if (cluster == 0) {
			if (at_end != NULL) {
				*at_end = true;
				return true;
			}
			return fail(error, error_size, "a cluster chain ends too soon");
		}
		if (++steps > v->clusters)
			return fail(error, error_size, "a cluster chain loops");
	}
	return true;
}

bool fat_read(FatVolume *v, const FatFile *file, uint32_t offset, void *buf,
              size_t len, char *error, size_t error_size)
{
	if (file->directory || offset > file->size || len > file->size - offset)
		return fail(error, error_size, "a read outside the file");
	if (len == 0)
		return true;
	return read_chain(v, file->cluster, offset, buf, len, NULL, error,
	                  error_size);
}

/* The entry at index of dir: 1 when read, 0 at the end of dir, -1. */
static int dir_entry(FatVolume *v, const FatFile *dir, uint32_t index,
                     uint8_t entry[DIR_ENTRY_SIZE], char *error,
                     size_t error_size)
{
	uint64_t at = (uint64_t)index * DIR_ENTRY_SIZE;
	bool at_end = false;

	if (dir->root) {
		if (at >= v->root_size)
			return 0;
		if (!block_read(v->dev, v->offset + v->root + at, entry,
		                DIR_ENTRY_SIZE)) {
			fail(error, error_size, "cannot read the root directory");
			return -1;
		}
		return 1;
	}
	if (!read_chain(v, dir->cluster, at, entry, DIR_ENTRY_SIZE, &at_end, error,
	                error_size))
		return -1;
	return at_end ? 0 : 1;
}

static uint8_t lfn_checksum(const uint8_t name[11])
{
	uint8_t sum = 0;

	for (int i = 0; i < 11; i++)
		sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
	return sum;
}

static int fold(int c)
{
	return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

/* Whether the len bytes of component are name, ignoring ASCII case. */
static bool same_name(const char *name, const char *component, size_t len)
{
	for (size_t i = 0; i < len; i++)
		if (name[i] == '\0' ||
		    fold((unsigned char)name[i]) != fold((unsigned char)component[i]))
			return false;
	return name[len] == '\0';
}

/* The 8.3 name of an entry as "NAME.EXT", without padding spaces. */
static void short_name(const uint8_t *e, char out[13])
{
	size_t n = 0;

	for (int i = 0; i < 8 && e[i] != ' '; i++)
		out[n++] = (char)(i == 0 && e[0] == 0x05 ? FREE_NAME : e[i]);
	if (e[8] != ' ') {
		out[n++] = '.';
		for (int i = 8; i < 11 && e[i] != ' '; i++)
			out[n++] = (char)e[i];
	}
	out[n] = '\0';
}

/* A long name being gathered from its entries, last part first. */
typedef struct LongName {
	uint16_t chars[LFN_MAX * LFN_CHARS];
	unsigned parts;    /* entries in all */
	unsigned expected; /* the sequence number of the next entry; 0 done */
	uint8_t checksum;
	bool valid;
} LongName;

static void lfn_entry(LongName *ln, const uint8_t *e)
{
	static const uint8_t offsets[LFN_CHARS] = {1,  3,  5,  7,  9,  14, 16,
	                                           18, 20, 22, 24, 28, 30};
	unsigned seq = e[0] & 0x1f;

	if (e[0] & LFN_LAST) {
		ln->valid = seq >= 1 && seq <= LFN_MAX;
		ln->parts = seq;
		ln->expected = seq;
		ln->checksum = e[LFN_CHECKSUM];
	}
	if (!ln->valid || seq != ln->expected || e[LFN_CHECKSUM] != ln->checksum) {
		ln->valid = false;
		return;
	}
	for (unsigned i = 0; i < LFN_CHARS; i++)
		ln->chars[(seq - 1) * LFN_CHARS + i] = load_le16(e + offsets[i]);
	ln->expected--;
}

/* The long name in UTF-8, ended by 0x0000 or by the last entry. */
static void lfn_utf8(const LongName *ln, char *out, size_t size)
{
	size_t n = 0;

	for (unsigned i = 0; i < ln->parts * LFN_CHARS; i++) {
		uint16_t c = ln->chars[i];

		if (c == 0 || n + 4 > size)
			break;
		if (c < 0x80) {
			out[n++] = (char)c;
		} else if (c < 0x800) {
			out[n++] = (char)(0xc0 | c >> 6);
			out[n++] = (char)(0x80 | (c & 0x3f));
		} else {
			out[n++] = (char)(0xe0 | c >> 12);
			out[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
			out[n++] = (char)(0x80 | (c & 0x3f));
		}
	}
	out[n] = '\0';
}

/* Find the len bytes of component in dir: 1, 0 or -1, as fat_find(). */
static int find_in(FatVolume *v, const FatFile *dir, const char *component,
                   size_t len, FatFile *file, char *error, size_t error_size)
{
	LongName ln = {.valid = false};
	uint8_t e[DIR_ENTRY_SIZE];

	for (uint32_t i = 0;; i++) {
		char name[LFN_MAX * LFN_CHARS * 3 + 1], short_[13];
		int r = dir_entry(v, dir, i, e, error, error_size);

		if (r <= 0)
			return r;
		if (e[DIR_NAME] == END_NAME)
			return 0;
		if (e[DIR_NAME] == FREE_NAME) {
			ln.valid = false;
			continue;
		}
		if (e[DIR_ATTR] == ATTR_LFN) {
			lfn_entry(&ln, e);
			continue;
		}
		if (e[DIR_ATTR] & ATTR_VOLUME) {
			ln.valid = false;
			continue;
		}
		short_name(e, short_);
		if (ln.valid && ln.expected == 0 && ln.checksum == lfn_checksum(e))
			lfn_utf8(&ln, name, sizeof(name));
		else
			name[0] = '\0';
		ln.valid = false;
		if (!same_name(short_, component, len) &&
		    !same_name(name, component, len))
			continue;
		file->cluster = load_le16(e + DIR_CLUSTER);
		if (v->bits == 32)
			file->cluster |= (uint32_t)load_le16(e + DIR_CLUSTER_HI) << 16;
		file->directory = e[DIR_ATTR] & ATTR_DIRECTORY;
		file->size = file->directory ? 0 : load_le32(e + DIR_SIZE);
		file->root = false;
		if (file->directory && file->cluster == 0) {
			/* ".." of a directory in the root: the root itself. */
			file->root = v->bits != 32;
			file->cluster = v->bits == 32 ? v->root_cluster : 0;
		}
		if (!file->root && (file->directory || file->size != 0) &&
		    !valid_cluster(v, file->cluster, error, error_size))
			return -1;
		return 1;
	}
}

int fat_find(FatVolume *v, const char *path, FatFile *file, char *error,
             size_t error_size)
{
	FatFile dir = {.directory = true};

	if (v->bits == 32)
		dir.cluster = v->root_cluster;
	else
		dir.root = true;
	for (;;) {
		size_t len;
		int r;

		while (*path == '/')
			path++;
		len = strcspn(path, "/");
		if (len == 0) {
			*file = dir;
			return 1;
		}
		if (!dir.directory)
			return 0;
		r = find_in(v, &dir, path, len, &dir, error, error_size);
		if (r <= 0)
			return r;
		path += len;
	}
}
