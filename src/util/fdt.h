/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Flattened devicetrees (.dtb files): read one into memory, find and
 * change its nodes and properties, and write it out again, as a boot
 * loader does before it hands the tree to the kernel.
 *
 * Written from chapter 5 of the Devicetree Specification, release
 * v0.4: the header (5.2), the memory reservation block (5.3), the
 * structure block (5.4) and the strings block (5.5), with the alignment
 * of 5.6; and from 2.2.3 for path names. Versions 16 and 17 are read;
 * version 17 is written, compatible with 16, with the blocks in the
 * order the specification lists them.
 */

#ifndef UTIL_FDT_H
#define UTIL_FDT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FDT_MAGIC 0xd00dfeedu

typedef struct Fdt Fdt;
typedef struct FdtNode FdtNode;

/*
 * Read the devicetree blob of size bytes at blob. On failure, writes a
 * message to error and returns NULL.
 */
Fdt *fdt_parse(const void *blob, size_t size, char *error, size_t error_size);

/* Free the tree. fdt may be NULL. */
void fdt_free(Fdt *fdt);

/*
 * The node at path, such as "/chosen" or "/cpus/cpu@1". A unit address
 * may be left out where only one child has the name. NULL when there is
 * no such node, or path is ambiguous.
 */
FdtNode *fdt_find(Fdt *fdt, const char *path);

/* The child of parent called name, as fdt_find() matches it, or NULL. */
FdtNode *fdt_child(FdtNode *parent, const char *name);

/*
 * The child of parent called name, added at the end of its children if
 * there is none. Returns NULL when memory runs out.
 */
FdtNode *fdt_add_node(FdtNode *parent, const char *name);

/*
 * The value of the property name of node, and its length in *len, or
 * NULL when node has no such property.
 */
const void *fdt_get(const FdtNode *node, const char *name, size_t *len);

/* The property as a 32-bit cell: false when it is missing or not 4 bytes. */
bool fdt_get_u32(const FdtNode *node, const char *name, uint32_t *value);

/*
 * Set the property name of node to the len bytes at value, adding it at
 * the end of node's properties if it is new. Returns false when memory
 * runs out.
 */
bool fdt_set(FdtNode *node, const char *name, const void *value, size_t len);

/* Set a property to a string, with its terminating 0. */
bool fdt_set_string(FdtNode *node, const char *name, const char *value);

/* Set a property to big-endian 32-bit cells. */
bool fdt_set_cells(FdtNode *node, const char *name, const uint32_t *cells,
                   size_t count);

/* Remove the property name of node, if it has one. */
void fdt_remove(FdtNode *node, const char *name);

/* The size of the blob that fdt_write() makes. */
size_t fdt_size(const Fdt *fdt);

/*
 * Write the tree as a blob of fdt_size() bytes to buf, which holds size
 * bytes. Returns false when it does not fit.
 */
bool fdt_write(const Fdt *fdt, void *buf, size_t size);

#endif /* UTIL_FDT_H */
