/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "boot/direct.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool boot_direct_load(Bus *bus, const char *path, uint64_t address,
                      uint64_t limit, char *error, size_t error_size)
{
	FILE *file = fopen(path, "rb");

	if (file == NULL) {
		snprintf(error, error_size, "%s: %s", path, strerror(errno));
		return false;
	}

	uint8_t buf[65536];
	uint64_t loaded = 0;
	bool ok = true;

	for (;;) {
		size_t n = fread(buf, 1, sizeof(buf), file);

		if (n == 0)
			break;
		if (address >= limit || n > limit - address - loaded) {
			snprintf(error, error_size,
			         "%s: does not fit in memory between 0x%" PRIx64
			         " and 0x%" PRIx64,
			         path, address, limit);
			ok = false;
			break;
		}

		BusStatus status = bus_write_block(bus, address + loaded, buf, n);

		if (status != BUS_OK) {
			snprintf(error, error_size, "%s: cannot load at 0x%" PRIx64 ": %s",
			         path, address + loaded, bus_status_str(status));
			ok = false;
			break;
		}
		loaded += n;
	}

	if (ok && ferror(file)) {
		snprintf(error, error_size, "%s: %s", path, strerror(errno));
		ok = false;
	}
	if (ok && loaded == 0) {
		snprintf(error, error_size, "%s: the file is empty", path);
		ok = false;
	}
	fclose(file);
	return ok;
}

bool boot_direct_read(const char *path, uint64_t limit, uint8_t **data,
                      size_t *size, char *error, size_t error_size)
{
	FILE *file = fopen(path, "rb");
	uint8_t *buf = NULL;
	size_t len = 0, capacity = 0;
	bool ok = true;

	if (file == NULL) {
		snprintf(error, error_size, "%s: %s", path, strerror(errno));
		return false;
	}
	for (;;) {
		size_t n;

		if (len == capacity) {
			uint8_t *grown;

			capacity = capacity ? 2 * capacity : 65536;
			grown = realloc(buf, capacity);
			if (grown == NULL) {
				snprintf(error, error_size, "%s: out of memory", path);
				ok = false;
				break;
			}
			buf = grown;
		}
		n = fread(buf + len, 1, capacity - len, file);
		if (n == 0)
			break;
		len += n;
		if (len > limit) {
			snprintf(error, error_size,
			         "%s: is larger than 0x%" PRIx64 " bytes", path, limit);
			ok = false;
			break;
		}
	}
	if (ok && ferror(file)) {
		snprintf(error, error_size, "%s: %s", path, strerror(errno));
		ok = false;
	}
	if (ok && len == 0) {
		snprintf(error, error_size, "%s: the file is empty", path);
		ok = false;
	}
	fclose(file);
	if (!ok) {
		free(buf);
		return false;
	}
	*data = buf;
	*size = len;
	return true;
}
