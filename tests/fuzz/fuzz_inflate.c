/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Fuzz the decompressor of gzip kernels (src/util/inflate.c): a gzip
 * file, with at most 1 MB inside.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "util/inflate.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char error[160];
	uint8_t *out;
	size_t out_size;

	if (gunzip(data, size, 1u << 20, &out, &out_size, error, sizeof(error)))
		free(out);
	return 0;
}
