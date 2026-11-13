/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The MBR partition table; see mbr.h. */

#include "boot/mbr.h"

#include <stdio.h>

#include "util/byteorder.h"

#define TABLE 446 /* the first partition entry */
#define ENTRY_SIZE 16
#define SIGNATURE 510 /* 0x55 0xaa */

bool mbr_read(BlockDevice *disk, MbrPartition parts[MBR_NUM_PARTITIONS],
              char *error, size_t error_size)
{
	uint8_t sector[MBR_SECTOR_SIZE];

	if (!block_read(disk, 0, sector, sizeof(sector))) {
		snprintf(error, error_size, "cannot read the partition table");
		return false;
	}
	if (sector[SIGNATURE] != 0x55 || sector[SIGNATURE + 1] != 0xaa) {
		snprintf(error, error_size,
		         "no partition table: the first sector "
		         "does not end with 0x55 0xaa");
		return false;
	}
	for (unsigned i = 0; i < MBR_NUM_PARTITIONS; i++) {
		const uint8_t *e = sector + TABLE + ENTRY_SIZE * i;
		MbrPartition *p = &parts[i];

		if (e[0] != 0x00 && e[0] != 0x80) {
			snprintf(error, error_size,
			         "partition %u has an invalid status, 0x%02x", i + 1, e[0]);
			return false;
		}
		p->active = e[0] == 0x80;
		p->type = e[4];
		p->first_sector = load_le32(e + 8);
		p->sectors = load_le32(e + 12);
		if (p->type != 0 &&
		    !block_in_range(disk, p->first_sector * MBR_SECTOR_SIZE,
		                    p->sectors * MBR_SECTOR_SIZE)) {
			snprintf(error, error_size,
			         "partition %u goes past the end of the disk", i + 1);
			return false;
		}
	}
	return true;
}
