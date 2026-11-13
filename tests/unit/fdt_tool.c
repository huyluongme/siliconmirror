/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A host tool for fdt_dtc.cmake: it reads a devicetree blob with
 * src/util/fdt.c and writes it out again, unchanged ("copy") or with
 * the changes that the Raspberry Pi firmware makes ("edit"), so that dtc
 * and fdtget, which smirror did not write, can check the result.
 *
 *   fdt_tool copy|edit IN OUT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* What the firmware does to a Pi 3's device tree. */
static int edit(Fdt *fdt)
{
	static const unsigned char mac[6] = {0xb8, 0x27, 0xeb, 0xff, 0xee, 0x03};
	static const uint32_t reg[] = {0, 0x3b400000};
	FdtNode *root = fdt_find(fdt, "/");
	FdtNode *chosen = fdt_find(fdt, "/chosen");
	FdtNode *memory = fdt_find(fdt, "/memory");
	FdtNode *ethernet = fdt_find(fdt, "/soc/usb@7e980000/usb1/ethernet");
	FdtNode *system;

	if (root == NULL || chosen == NULL || memory == NULL || ethernet == NULL)
		return 0;
	system = fdt_add_node(root, "system");
	return fdt_set_string(chosen, "bootargs", "coherent_pool=1M quiet") &&
	       fdt_set_cells(chosen, "linux,initrd-start",
	                     (const uint32_t[]){0x81000}, 1) &&
	       fdt_set_cells(memory, "reg", reg, 2) &&
	       fdt_set(ethernet, "local-mac-address", mac, sizeof(mac)) &&
	       system != NULL &&
	       fdt_set_cells(system, "linux,revision", (const uint32_t[]){0xa02082},
	                     1);
}

int main(int argc, char **argv)
{
	unsigned char *in, *out;
	size_t size, out_size;
	char error[128];
	Fdt *fdt;
	FILE *f;

	if (argc != 4 ||
	    (strcmp(argv[1], "copy") != 0 && strcmp(argv[1], "edit") != 0)) {
		fputs("usage: fdt_tool copy|edit IN OUT\n", stderr);
		return 2;
	}
	in = read_file(argv[2], &size);
	if (in == NULL) {
		fprintf(stderr, "fdt_tool: cannot read %s\n", argv[2]);
		return 1;
	}
	fdt = fdt_parse(in, size, error, sizeof(error));
	free(in);
	if (fdt == NULL) {
		fprintf(stderr, "fdt_tool: %s: %s\n", argv[2], error);
		return 1;
	}
	if (strcmp(argv[1], "edit") == 0 && !edit(fdt)) {
		fputs("fdt_tool: the tree lacks a node, or memory ran out\n", stderr);
		fdt_free(fdt);
		return 1;
	}
	out_size = fdt_size(fdt);
	out = malloc(out_size);
	if (out == NULL || !fdt_write(fdt, out, out_size)) {
		fputs("fdt_tool: cannot write the tree\n", stderr);
		free(out);
		fdt_free(fdt);
		return 1;
	}
	fdt_free(fdt);
	f = fopen(argv[3], "wb");
	if (f == NULL || fwrite(out, 1, out_size, f) != out_size) {
		fprintf(stderr, "fdt_tool: cannot write %s\n", argv[3]);
		if (f != NULL)
			fclose(f);
		free(out);
		return 1;
	}
	fclose(f);
	free(out);
	return 0;
}
