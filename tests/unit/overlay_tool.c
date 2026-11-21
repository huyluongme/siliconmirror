/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A host tool for overlay.cmake: it applies config.txt's dtoverlay= and
 * dtparam= lines to a base tree with src/boot/rpi_overlay.c, as the
 * firmware does, so that dtc, which smirror did not write, can show the
 * result.
 *
 *   overlay_tool BASE DIR OUT LINE...
 *
 * The overlays are DIR/NAME.dtbo, and DIR/overlay_map.dtb the map if
 * there is one; each LINE is "dtoverlay=..." or "dtparam=...". The
 * warnings go to standard output, a line each.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "boot/rpi_overlay.h"
#include "util/fdt.h"

static unsigned char *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	unsigned char *data;
	long len;

	if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0) {
		if (f != NULL)
			fclose(f);
		return NULL;
	}
	rewind(f);
	data = malloc((size_t)len + 1);
	if (data != NULL && fread(data, 1, (size_t)len, f) != (size_t)len) {
		free(data);
		data = NULL;
	}
	fclose(f);
	*size = (size_t)len;
	return data;
}

static Fdt *read_tree(const char *path, char *error, size_t error_size)
{
	size_t size;
	unsigned char *blob = read_file(path, &size);
	Fdt *fdt;

	if (blob == NULL) {
		snprintf(error, error_size, "no %s", path);
		return NULL;
	}
	fdt = fdt_parse(blob, size, error, error_size);
	free(blob);
	return fdt;
}

static Fdt *load(void *ctx, const char *name, char *error, size_t error_size)
{
	char path[1024];
	Fdt *fdt;

	snprintf(path, sizeof(path), "%s/%s.dtbo", (const char *)ctx, name);
	fdt = read_tree(path, error, error_size);
	if (fdt == NULL && access(path, F_OK) != 0)
		snprintf(error, error_size, "no %s.dtbo", name);
	return fdt;
}

static void warn(void *ctx, const char *message)
{
	(void)ctx;
	printf("warning: %s\n", message);
}

int main(int argc, char **argv)
{
	char error[256], path[1024];
	RpiOverlays o = {.platform = "bcm2835", .load = load, .warn = warn};
	unsigned char *out;
	size_t size;
	FILE *f;

	if (argc < 4) {
		fputs("usage: overlay_tool BASE DIR OUT LINE...\n", stderr);
		return 2;
	}
	o.base = read_tree(argv[1], error, sizeof(error));
	if (o.base == NULL) {
		fprintf(stderr, "overlay_tool: %s\n", error);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/overlay_map.dtb", argv[2]);
	o.map = read_tree(path, error, sizeof(error));
	o.ctx = argv[2];
	for (int i = 4; i < argc; i++) {
		if (strncmp(argv[i], "dtoverlay=", 10) == 0) {
			rpi_overlays_dtoverlay(&o, argv[i] + 10);
		} else if (strncmp(argv[i], "dtparam=", 8) == 0) {
			rpi_overlays_dtparam(&o, argv[i] + 8);
		} else {
			fprintf(stderr, "overlay_tool: what is \"%s\"?\n", argv[i]);
			return 2;
		}
	}
	rpi_overlays_end(&o);
	size = fdt_size(o.base);
	out = malloc(size);
	f = fopen(argv[3], "wb");
	if (out == NULL || f == NULL || !fdt_write(o.base, out, size) ||
	    fwrite(out, 1, size, f) != size || fclose(f) != 0) {
		fprintf(stderr, "overlay_tool: cannot write %s\n", argv[3]);
		return 1;
	}
	free(out);
	fdt_free(o.base);
	fdt_free(o.map);
	return 0;
}
