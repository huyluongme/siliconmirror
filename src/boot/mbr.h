/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The partition table of a master boot record (MBR): the four primary
 * entries at offset 446 of the first sector of a disk, which ends with
 * the bytes 0x55 0xaa.
 *
 * Written from Wikipedia, "Master boot record": the layout of the sector
 * and of a partition entry (status, type, first sector and number of
 * sectors in logical block addresses of 512 bytes). Addresses in CHS
 * form are not used, nor are extended partitions followed.
 */

#ifndef BOOT_MBR_H
#define BOOT_MBR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/block.h"

#define MBR_SECTOR_SIZE 512
#define MBR_NUM_PARTITIONS 4

typedef struct MbrPartition {
	uint8_t type;          /* 0 for an unused entry */
	bool active;           /* status 0x80: bootable */
	uint64_t first_sector; /* in sectors of 512 bytes */
	uint64_t sectors;
} MbrPartition;

/*
 * Read the partition table of disk. On failure (no MBR, a status byte
 * other than 0x00 and 0x80, a partition past the end of the disk),
 * writes a message to error and returns false.
 */
bool mbr_read(BlockDevice *disk, MbrPartition parts[MBR_NUM_PARTITIONS],
              char *error, size_t error_size);

#endif /* BOOT_MBR_H */
