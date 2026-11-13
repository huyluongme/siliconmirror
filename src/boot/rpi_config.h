/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * config.txt, the Raspberry Pi firmware's settings, read as the firmware
 * reads it from the boot partition.
 *
 * Written from the Raspberry Pi documentation, "config.txt": the file
 * format (one property=value per line, # comments, 98 characters a
 * line, include), the conditional filters, and the settings that only
 * config.txt itself sets, not a file it includes (gpu_mem, total_mem,
 * start_x, ...: the first-stage bootloader reads them).
 *
 * What the filters test is the board's: its models, its board type, its
 * serial number and the levels of its GPIO pins. A filter that this
 * reading does not know applies nothing until the next [all]; a warning
 * says so. The settings come out as a list, in order, for the firmware
 * to look up; the ones it never looks up are reported as not applied.
 */

#ifndef BOOT_RPI_CONFIG_H
#define BOOT_RPI_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot/fat.h"

/* What a config.txt filter tests about the board. */
typedef struct RpiBoard {
	const char *const *models; /* the model filters that match, NULL-ended */
	uint32_t board_type;       /* the Type field of the revision code */
	uint32_t serial;           /* the low 32 bits of the serial number */
	bool (*gpio_level)(void *ctx, unsigned pin); /* NULL: no GPIO */
	void *ctx;
} RpiBoard;

/* A setting: key=value, or "initramfs" with the rest of its line. */
typedef struct RpiSetting {
	char *key, *value;
	char *file;    /* config.txt, or the file that included it */
	unsigned line; /* in file */
	bool included; /* from a file that config.txt includes */
	bool used;     /* the firmware looked it up */
} RpiSetting;

typedef struct RpiConfig {
	RpiSetting *settings;
	size_t count;
} RpiConfig;

/* Messages about the file: what it does not apply, and why. */
typedef void (*RpiWarn)(void *ctx, const char *message);

/*
 * Read config.txt from the root of fat for board; a missing config.txt
 * sets nothing. warn gets each problem with a line. On a damaged file
 * system, writes a message to error and returns false.
 */
bool rpi_config_read(FatVolume *fat, const RpiBoard *board, RpiConfig *config,
                     RpiWarn warn, void *warn_ctx, char *error,
                     size_t error_size);

/* Read config.txt from text, of len bytes, as rpi_config_read() does. */
bool rpi_config_parse(const char *text, size_t len, const RpiBoard *board,
                      RpiConfig *config, RpiWarn warn, void *warn_ctx);

void rpi_config_free(RpiConfig *config);

/*
 * The value of the last setting of key, or NULL. With bootloader_only,
 * settings from included files do not count. Marks the settings of key
 * as used.
 */
const char *rpi_config_get(RpiConfig *config, const char *key,
                           bool bootloader_only);

/*
 * The value of key as an integer, in decimal or in hexadecimal with 0x:
 * returns false, with *value untouched, when key is not set. A value that
 * is not an integer is reported through warn and does not count.
 */
bool rpi_config_int(RpiConfig *config, const char *key, bool bootloader_only,
                    uint64_t *value, RpiWarn warn, void *warn_ctx);

/* Report every setting that the firmware has not looked up. */
void rpi_config_report_unused(const RpiConfig *config, RpiWarn warn,
                              void *warn_ctx);

#endif /* BOOT_RPI_CONFIG_H */
