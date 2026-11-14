/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Fuzz the devicetree code (src/util/fdt.c): parse a blob, change it as
 * the firmware does, write it out, and parse what was written.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "util/fdt.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const uint32_t cells[] = {0, 0x3b400000};
	char error[160];
	Fdt *fdt = fdt_parse(data, size, error, sizeof(error)), *again;
	FdtNode *node;
	size_t len, out_size;
	uint32_t value;
	void *out;

	if (fdt == NULL)
		return 0;
	node = fdt_find(fdt, "/chosen");
	if (node == NULL && fdt_find(fdt, "/") != NULL)
		node = fdt_add_node(fdt_find(fdt, "/"), "chosen");
	if (node != NULL) {
		fdt_get(node, "bootargs", &len);
		fdt_set_string(node, "bootargs", "console=serial0,115200");
		fdt_get_u32(node, "linux,initrd-start", &value);
	}
	node = fdt_find(fdt, "/memory@0");
	if (node != NULL) {
		fdt_set_cells(node, "reg", cells, 2);
		fdt_remove(node, "device_type");
	}
	fdt_find(fdt, "/soc/serial@7e215040");
	fdt_child(fdt_find(fdt, "/"), "aliases");
	out_size = fdt_size(fdt);
	out = malloc(out_size);
	if (out != NULL && fdt_write(fdt, out, out_size)) {
		again = fdt_parse(out, out_size, error, sizeof(error));
		if (again == NULL)
			abort(); /* what fdt_write() writes must parse */
		fdt_free(again);
	}
	free(out);
	fdt_free(fdt);
	return 0;
}
