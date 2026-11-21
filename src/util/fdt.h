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
typedef struct FdtProp FdtProp;

/*
 * Read the devicetree blob of size bytes at blob. On failure, writes a
 * message to error and returns NULL.
 */
Fdt *fdt_parse(const void *blob, size_t size, char *error, size_t error_size);

/* Free the tree. fdt may be NULL. */
void fdt_free(Fdt *fdt);

/* Exchange the trees of a and b, as when b is a copy to go back to. */
void fdt_swap(Fdt *a, Fdt *b);

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

/* As fdt_add_node(), but a new child goes before the others. */
FdtNode *fdt_add_node_first(FdtNode *parent, const char *name);

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

/* As fdt_set(), but a new property goes before the others. */
bool fdt_set_first(FdtNode *node, const char *name, const void *value,
                   size_t len);

/* Set a property to a string, with its terminating 0. */
bool fdt_set_string(FdtNode *node, const char *name, const char *value);

/* Set a property to big-endian 32-bit cells. */
bool fdt_set_cells(FdtNode *node, const char *name, const uint32_t *cells,
                   size_t count);

/* Remove the property name of node, if it has one. */
void fdt_remove(FdtNode *node, const char *name);

/* The root node. */
FdtNode *fdt_root(Fdt *fdt);

/* The name of node, with its unit address: "" for the root. */
const char *fdt_name(const FdtNode *node);

/* The parent of node, or NULL for the root. */
FdtNode *fdt_parent(const FdtNode *node);

/* The first child of node, and the child after child: NULL at the end. */
FdtNode *fdt_first_child(const FdtNode *node);
FdtNode *fdt_next_child(const FdtNode *child);

/* Give node another name. Returns false when memory runs out. */
bool fdt_rename(FdtNode *node, const char *name);

/*
 * The full path of node, such as "/soc/gpio@7e200000", in buf of size
 * bytes. Returns false when it does not fit.
 */
bool fdt_path(const FdtNode *node, char *buf, size_t size);

/*
 * The first property of node, and the property after prop: NULL at the
 * end. The value may be changed in place, its length not.
 */
FdtProp *fdt_first_prop(const FdtNode *node);
FdtProp *fdt_next_prop(const FdtProp *prop);
const char *fdt_prop_name(const FdtProp *prop);
uint8_t *fdt_prop_value(const FdtProp *prop, size_t *len);

/* The size of the blob that fdt_write() makes. */
size_t fdt_size(const Fdt *fdt);

/*
 * Write the tree as a blob of fdt_size() bytes to buf, which holds size
 * bytes. Returns false when it does not fit.
 */
bool fdt_write(const Fdt *fdt, void *buf, size_t size);

#endif /* UTIL_FDT_H */
