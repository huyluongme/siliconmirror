/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Fuzz the MBR reader (src/boot/mbr.c) with a whole card. */

#include <stddef.h>
#include <stdint.h>

#include "boot/mbr.h"
#include "fuzz_block.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzBlock b;
	MbrPartition parts[MBR_NUM_PARTITIONS];
	char why[160];

	mbr_read(fuzz_block(&b, data, size), parts, why, sizeof(why));
	return 0;
}
