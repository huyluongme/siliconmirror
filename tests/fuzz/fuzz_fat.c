/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Fuzz the FAT reader (src/boot/fat.c) with a volume, and config.txt as
 * the firmware reads it from there (src/boot/rpi_config.c), includes and
 * all.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "boot/fat.h"
#include "boot/rpi_config.h"
#include "fuzz_block.h"

static void warn(void *ctx, const char *message)
{
	(void)ctx;
	(void)message;
}

static void read_file(FatVolume *v, const char *path)
{
	static uint8_t buf[65536];
	char error[160];
	FatFile f;

	if (fat_find(v, path, &f, error, sizeof(error)) != 1 || f.directory)
		return;
	for (uint32_t at = 0; at < f.size && at < (1u << 20); at += sizeof(buf)) {
		size_t n = f.size - at < sizeof(buf) ? f.size - at : sizeof(buf);

		if (!fat_read(v, &f, at, buf, n, error, sizeof(error)))
			return;
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const char *const models[] = {"pi3", NULL};
	static const char *const paths[] = {
		"config.txt",       "bootcode.bin", "a/b/c.txt", "overlays/README",
		"LONGFILENAME.TXT", "kernel8.img",  "/",         "dir/",
	};
	const RpiBoard board = {.models = models, .board_type = 8};
	FuzzBlock b;
	FatVolume *v;
	RpiConfig config = {0};
	char error[160];

	v = fat_open(fuzz_block(&b, data, size), 0, size, error, sizeof(error));
	if (v == NULL)
		return 0;
	fat_bits(v);
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
		read_file(v, paths[i]);
	if (rpi_config_read(v, &board, &config, warn, NULL, error, sizeof(error))) {
		rpi_config_get(&config, "kernel", false);
		rpi_config_report_unused(&config, warn, NULL);
	}
	rpi_config_free(&config);
	fat_close(v);
	return 0;
}
