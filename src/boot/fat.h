/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Reading files from a FAT file system (FAT12, FAT16 or FAT32), with
 * their long names, as the Raspberry Pi firmware reads its boot
 * partition. Nothing is written.
 *
 * Written from ECMA-107 (2nd edition, 1995): the boot sector's
 * parameters (9.2), the FAT and its entries (10), directories and their
 * entries (11); and from Wikipedia, "File Allocation Table": the FAT32
 * parameters (the root directory cluster, the size of a FAT, the active
 * FAT), FAT32 entries, the choice between FAT12, FAT16 and FAT32 by the
 * number of clusters, and VFAT long file names with their checksum.
 *
 * Names match without regard to the case of ASCII letters, as on the
 * real file system; a long name and the short 8.3 name both match.
 */

#ifndef BOOT_FAT_H
#define BOOT_FAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/block.h"

typedef struct FatVolume FatVolume;

typedef struct FatFile {
	uint32_t cluster; /* the first; 0 for an empty file */
	uint32_t size;    /* bytes; 0 for a directory */
	bool directory;
	bool root; /* the fixed root directory of FAT12 and FAT16 */
} FatFile;

/*
 * Open the file system in the size bytes at offset of dev. On failure,
 * writes a message to error and returns NULL.
 */
FatVolume *fat_open(BlockDevice *dev, uint64_t offset, uint64_t size,
                    char *error, size_t error_size);
void fat_close(FatVolume *v);

/* 12, 16 or 32. */
unsigned fat_bits(const FatVolume *v);

/*
 * Find path, such as "config.txt" or "overlays/README", from the root
 * directory. Returns 1 and fills *file when found, 0 when not, and -1
 * with a message in error when the file system is damaged.
 */
int fat_find(FatVolume *v, const char *path, FatFile *file, char *error,
             size_t error_size);

/*
 * Read len bytes of file at offset, which must lie inside the file. On
 * failure, writes a message to error and returns false.
 */
bool fat_read(FatVolume *v, const FatFile *file, uint32_t offset, void *buf,
              size_t len, char *error, size_t error_size);

#endif /* BOOT_FAT_H */
