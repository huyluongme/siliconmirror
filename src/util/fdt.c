/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Flattened devicetrees; see fdt.h. */

#include "util/fdt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/byteorder.h"
#include "util/macros.h"

/* Tokens of the structure block (5.4.1). */
enum {
	FDT_BEGIN_NODE = 1,
	FDT_END_NODE = 2,
	FDT_PROP = 3,
	FDT_NOP = 4,
	FDT_END = 9,
};

#define HEADER_SIZE 40 /* struct fdt_header, version 17 */
#define MAX_DEPTH 64   /* deeper trees are refused */

typedef struct FdtProp {
	struct FdtProp *next;
	char *name;
	uint8_t *value;
	size_t len;
} FdtProp;

struct FdtNode {
	FdtNode *next; /* the next child of the parent */
	FdtNode *children, *last_child;
	FdtProp *props, *last_prop;
	char *name;
};

typedef struct Reservation {
	uint64_t address, size;
} Reservation;

struct Fdt {
	FdtNode *root;
	Reservation *reservations;
	size_t num_reservations;
	uint32_t boot_cpuid_phys;
};

static char *copy_string(const char *s, size_t len)
{
	char *copy = malloc(len + 1);

	if (copy != NULL) {
		memcpy(copy, s, len);
		copy[len] = '\0';
	}
	return copy;
}

static FdtNode *new_node(const char *name, size_t len)
{
	FdtNode *node = calloc(1, sizeof(*node));

	if (node == NULL)
		return NULL;
	node->name = copy_string(name, len);
	if (node->name == NULL) {
		free(node);
		return NULL;
	}
	return node;
}

static void free_node(FdtNode *node)
{
	while (node != NULL) {
		FdtNode *next = node->next;

		for (FdtProp *p = node->props, *pn; p != NULL; p = pn) {
			pn = p->next;
			free(p->name);
			free(p->value);
			free(p);
		}
		free_node(node->children);
		free(node->name);
		free(node);
		node = next;
	}
}

void fdt_free(Fdt *fdt)
{
	if (fdt == NULL)
		return;
	free_node(fdt->root);
	free(fdt->reservations);
	free(fdt);
}

static void append_child(FdtNode *parent, FdtNode *child)
{
	if (parent->last_child != NULL)
		parent->last_child->next = child;
	else
		parent->children = child;
	parent->last_child = child;
}

static FdtProp *find_prop(const FdtNode *node, const char *name)
{
	for (FdtProp *p = node->props; p != NULL; p = p->next)
		if (strcmp(p->name, name) == 0)
			return p;
	return NULL;
}

/* --- Reading -------------------------------------------------------------- */

typedef struct Reader {
	const uint8_t *blob;
	uint32_t struct_off, struct_end; /* the structure block */
	uint32_t strings_off, strings_size;
	char *error;
	size_t error_size;
} Reader;

static bool fail(Reader *r, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static bool fail(Reader *r, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(r->error, r->error_size, fmt, ap);
	va_end(ap);
	return false;
}

/*
 * The token at *pos, which moves past it; false when the block ends.
 * The failures return false themselves, so that compilers see that
 * *value is set whenever the result is true.
 */
static bool token(Reader *r, uint32_t *pos, uint32_t *value)
{
	if (*pos > r->struct_end || r->struct_end - *pos < 4) {
		fail(r, "the structure block ends inside a token");
		return false;
	}
	*value = load_be32(r->blob + *pos);
	*pos += 4;
	return true;
}

/* The length of the string at pos, which must end before end. */
static bool string_at(Reader *r, uint32_t pos, uint32_t end, size_t *len,
                      const char *what)
{
	const void *nul = memchr(r->blob + pos, '\0', end - pos);

	if (nul == NULL) {
		fail(r, "%s at offset %u has no terminating 0", what, (unsigned)pos);
		return false;
	}
	*len = (size_t)((const uint8_t *)nul - (r->blob + pos));
	return true;
}

static uint32_t align4(uint32_t n)
{
	return (n + 3) & ~3u;
}

static bool read_prop(Reader *r, uint32_t *pos, FdtNode *node)
{
	uint32_t len, nameoff;
	size_t name_len;
	FdtProp *p;

	if (!token(r, pos, &len) || !token(r, pos, &nameoff))
		return false;
	if (len > r->struct_end - *pos)
		return fail(r, "a property runs past the structure block");
	if (nameoff >= r->strings_size)
		return fail(r, "a property name is outside the strings block");
	if (!string_at(r, r->strings_off + nameoff,
	               r->strings_off + r->strings_size, &name_len,
	               "a property name"))
		return false;
	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return fail(r, "out of memory");
	p->name =
		copy_string((const char *)r->blob + r->strings_off + nameoff, name_len);
	p->value = malloc(len ? len : 1);
	if (p->name == NULL || p->value == NULL) {
		free(p->name);
		free(p->value);
		free(p);
		return fail(r, "out of memory");
	}
	memcpy(p->value, r->blob + *pos, len);
	p->len = len;
	if (node->last_prop != NULL)
		node->last_prop->next = p;
	else
		node->props = p;
	node->last_prop = p;
	*pos += align4(len); /* the next token() checks it is inside */
	return true;
}

/* The structure block (5.4.2): one root node, then FDT_END. */
static bool read_struct(Reader *r, FdtNode **root)
{
	FdtNode *stack[MAX_DEPTH];
	unsigned depth = 0;
	uint32_t pos = r->struct_off, tok;

	*root = NULL;
	for (;;) {
		if (!token(r, &pos, &tok))
			return false;
		switch (tok) {
		case FDT_NOP:
			continue;
		case FDT_BEGIN_NODE: {
			size_t len;
			FdtNode *node;

			if (*root != NULL && depth == 0)
				return fail(r, "a second root node");
			if (depth == MAX_DEPTH)
				return fail(r, "nodes nest deeper than %d", MAX_DEPTH);
			if (!string_at(r, pos, r->struct_end, &len, "a node name"))
				return false;
			if (depth == 0 && len != 0)
				return fail(r, "the root node has a name");
			node = new_node((const char *)r->blob + pos, len);
			if (node == NULL)
				return fail(r, "out of memory");
			if (depth == 0)
				*root = node;
			else
				append_child(stack[depth - 1], node);
			stack[depth++] = node;
			pos = align4(pos + (uint32_t)len + 1);
			continue;
		}
		case FDT_PROP:
			if (depth == 0)
				return fail(r, "a property outside the nodes");
			if (stack[depth - 1]->children != NULL)
				return fail(r, "a property after a node's children");
			if (!read_prop(r, &pos, stack[depth - 1]))
				return false;
			continue;
		case FDT_END_NODE:
			if (depth == 0)
				return fail(r, "the end of a node that did not begin");
			depth--;
			continue;
		case FDT_END:
			if (*root == NULL)
				return fail(r, "no root node");
			if (depth != 0)
				return fail(r, "the structure block ends inside a node");
			return true;
		default:
			return fail(r, "unknown token 0x%x at offset %u", (unsigned)tok,
			            (unsigned)(pos - 4));
		}
	}
}

static bool read_reservations(Reader *r, Fdt *fdt, uint32_t off, uint32_t total)
{
	for (size_t n = 0;; n++) {
		uint64_t address, size;
		Reservation *grown;

		if (off > total || total - off < 16)
			return fail(r, "the memory reservation block has no end");
		address = load_be64(r->blob + off);
		size = load_be64(r->blob + off + 8);
		off += 16;
		if (address == 0 && size == 0)
			return true;
		grown = realloc(fdt->reservations, (n + 1) * sizeof(*grown));
		if (grown == NULL)
			return fail(r, "out of memory");
		fdt->reservations = grown;
		fdt->reservations[n] = (Reservation){address, size};
		fdt->num_reservations = n + 1;
	}
}

Fdt *fdt_parse(const void *blob, size_t size, char *error, size_t error_size)
{
	const uint8_t *b = blob;
	Reader r = {.blob = b, .error = error, .error_size = error_size};
	uint32_t total, version, rsv;
	Fdt *fdt;

	if (size < HEADER_SIZE || load_be32(b) != FDT_MAGIC) {
		snprintf(error, error_size, "not a devicetree blob");
		return NULL;
	}
	total = load_be32(b + 4);
	r.struct_off = load_be32(b + 8);
	r.strings_off = load_be32(b + 12);
	rsv = load_be32(b + 16);
	version = load_be32(b + 20);
	r.strings_size = load_be32(b + 32);
	if (version < 16 || load_be32(b + 24) > 17) {
		snprintf(error, error_size,
		         "devicetree version %u, compatible with %u, is not supported",
		         (unsigned)version, (unsigned)load_be32(b + 24));
		return NULL;
	}
	if (total > size || total < HEADER_SIZE) {
		snprintf(error, error_size, "the blob's total size, %u, is wrong",
		         (unsigned)total);
		return NULL;
	}
	r.struct_end = total;
	if (version >= 17) {
		uint32_t struct_size = load_be32(b + 36);

		if (struct_size > total || r.struct_off > total - struct_size) {
			snprintf(error, error_size, "the structure block is outside");
			return NULL;
		}
		r.struct_end = r.struct_off + struct_size;
	}
	if (r.struct_off % 4 != 0 || r.struct_off > r.struct_end || rsv % 8 != 0 ||
	    r.strings_size > total || r.strings_off > total - r.strings_size) {
		snprintf(error, error_size, "a block of the blob is misplaced");
		return NULL;
	}
	fdt = calloc(1, sizeof(*fdt));
	if (fdt == NULL) {
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	fdt->boot_cpuid_phys = load_be32(b + 28);
	if (!read_reservations(&r, fdt, rsv, total) ||
	    !read_struct(&r, &fdt->root)) {
		fdt_free(fdt);
		return NULL;
	}
	return fdt;
}

/* --- Finding and changing ------------------------------------------------- */

/* Whether name, a node's name, is component, len bytes of a path. */
static bool name_is(const char *name, const char *component, size_t len)
{
	return strncmp(name, component, len) == 0 && name[len] == '\0';
}

/* The base of name, the part before its unit address, is component. */
static bool base_is(const char *name, const char *component, size_t len)
{
	return strncmp(name, component, len) == 0 && name[len] == '@';
}

static FdtNode *child_of(FdtNode *parent, const char *component, size_t len)
{
	FdtNode *found = NULL;

	for (FdtNode *c = parent->children; c != NULL; c = c->next)
		if (name_is(c->name, component, len))
			return c;
	if (memchr(component, '@', len) != NULL)
		return NULL;
	for (FdtNode *c = parent->children; c != NULL; c = c->next) {
		if (base_is(c->name, component, len)) {
			if (found != NULL)
				return NULL; /* ambiguous */
			found = c;
		}
	}
	return found;
}

FdtNode *fdt_child(FdtNode *parent, const char *name)
{
	return child_of(parent, name, strlen(name));
}

FdtNode *fdt_find(Fdt *fdt, const char *path)
{
	FdtNode *node = fdt->root;

	if (path[0] != '/')
		return NULL;
	while (*path != '\0' && node != NULL) {
		size_t len;

		while (*path == '/')
			path++;
		len = strcspn(path, "/");
		if (len == 0)
			break;
		node = child_of(node, path, len);
		path += len;
	}
	return node;
}

FdtNode *fdt_add_node(FdtNode *parent, const char *name)
{
	FdtNode *node;

	for (FdtNode *c = parent->children; c != NULL; c = c->next)
		if (strcmp(c->name, name) == 0)
			return c;
	node = new_node(name, strlen(name));
	if (node != NULL)
		append_child(parent, node);
	return node;
}

const void *fdt_get(const FdtNode *node, const char *name, size_t *len)
{
	const FdtProp *p = find_prop(node, name);

	if (p == NULL)
		return NULL;
	*len = p->len;
	return p->value;
}

bool fdt_get_u32(const FdtNode *node, const char *name, uint32_t *value)
{
	size_t len;
	const void *v = fdt_get(node, name, &len);

	if (v == NULL || len != 4)
		return false;
	*value = load_be32(v);
	return true;
}

bool fdt_set(FdtNode *node, const char *name, const void *value, size_t len)
{
	FdtProp *p = find_prop(node, name);
	uint8_t *copy = malloc(len ? len : 1);

	if (copy == NULL)
		return false;
	memcpy(copy, value, len);
	if (p == NULL) {
		p = calloc(1, sizeof(*p));
		if (p == NULL || (p->name = copy_string(name, strlen(name))) == NULL) {
			free(p);
			free(copy);
			return false;
		}
		if (node->last_prop != NULL)
			node->last_prop->next = p;
		else
			node->props = p;
		node->last_prop = p;
	} else {
		free(p->value);
	}
	p->value = copy;
	p->len = len;
	return true;
}

bool fdt_set_string(FdtNode *node, const char *name, const char *value)
{
	return fdt_set(node, name, value, strlen(value) + 1);
}

bool fdt_set_cells(FdtNode *node, const char *name, const uint32_t *cells,
                   size_t count)
{
	uint8_t *buf = malloc(count ? 4 * count : 1);
	bool ok;

	if (buf == NULL)
		return false;
	for (size_t i = 0; i < count; i++)
		store_be32(buf + 4 * i, cells[i]);
	ok = fdt_set(node, name, buf, 4 * count);
	free(buf);
	return ok;
}

void fdt_remove(FdtNode *node, const char *name)
{
	FdtProp *prev = NULL;

	for (FdtProp *p = node->props; p != NULL; prev = p, p = p->next) {
		if (strcmp(p->name, name) != 0)
			continue;
		if (prev != NULL)
			prev->next = p->next;
		else
			node->props = p->next;
		if (node->last_prop == p)
			node->last_prop = prev;
		free(p->name);
		free(p->value);
		free(p);
		return;
	}
}

/* --- Writing -------------------------------------------------------------- */

/*
 * Lays the blocks out: with out NULL, only counts. Property names are
 * stored once each in the strings block.
 */
typedef struct Writer {
	uint8_t *out; /* the whole blob, or NULL */
	uint32_t struct_off, struct_len;
	uint32_t strings_off, strings_len;
} Writer;

static void put32(Writer *w, uint32_t value)
{
	if (w->out != NULL)
		store_be32(w->out + w->struct_off + w->struct_len, value);
	w->struct_len += 4;
}

static void put_bytes(Writer *w, const void *bytes, size_t len)
{
	size_t padded = align4((uint32_t)len);

	if (w->out != NULL) {
		uint8_t *dst = w->out + w->struct_off + w->struct_len;

		memcpy(dst, bytes, len);
		memset(dst + len, 0, padded - len);
	}
	w->struct_len += (uint32_t)padded;
}

/*
 * The offset of name in the strings block. Names go in the order they
 * first appear in the tree, so the first property with the name gives
 * the offset.
 */
typedef struct Name {
	const char *name;
	uint32_t offset;
} Name;

typedef struct Names {
	Name *names;
	size_t count, capacity;
} Names;

static bool name_offset(Writer *w, Names *names, const char *name,
                        uint32_t *offset)
{
	size_t len = strlen(name) + 1;

	for (size_t i = 0; i < names->count; i++) {
		if (strcmp(names->names[i].name, name) == 0) {
			*offset = names->names[i].offset;
			return true;
		}
	}
	if (names->count == names->capacity) {
		size_t capacity = names->capacity ? 2 * names->capacity : 64;
		Name *grown = realloc(names->names, capacity * sizeof(*grown));

		if (grown == NULL)
			return false;
		names->names = grown;
		names->capacity = capacity;
	}
	names->names[names->count++] = (Name){name, w->strings_len};
	*offset = w->strings_len;
	if (w->out != NULL)
		memcpy(w->out + w->strings_off + w->strings_len, name, len);
	w->strings_len += (uint32_t)len;
	return true;
}

static bool write_node(Writer *w, Names *names, const FdtNode *node)
{
	put32(w, FDT_BEGIN_NODE);
	put_bytes(w, node->name, strlen(node->name) + 1);
	for (const FdtProp *p = node->props; p != NULL; p = p->next) {
		uint32_t offset;

		if (!name_offset(w, names, p->name, &offset))
			return false;
		put32(w, FDT_PROP);
		put32(w, (uint32_t)p->len);
		put32(w, offset);
		put_bytes(w, p->value, p->len);
	}
	for (const FdtNode *c = node->children; c != NULL; c = c->next)
		if (!write_node(w, names, c))
			return false;
	put32(w, FDT_END_NODE);
	return true;
}

/* Lay out the tree into a Writer that is new; with w->out set, write it. */
static bool lay_out(const Fdt *fdt, Writer *w)
{
	Names names = {0};
	bool ok;

	w->struct_off = HEADER_SIZE + 16 * (uint32_t)(fdt->num_reservations + 1);
	ok = write_node(w, &names, fdt->root);
	put32(w, FDT_END);
	free(names.names);
	return ok;
}

size_t fdt_size(const Fdt *fdt)
{
	Writer w = {0};

	if (!lay_out(fdt, &w))
		return 0;
	return w.struct_off + w.struct_len + w.strings_len;
}

bool fdt_write(const Fdt *fdt, void *buf, size_t size)
{
	Writer count = {0}, w = {.out = buf};
	uint8_t *b = buf;
	uint32_t rsv = HEADER_SIZE;
	size_t total;

	/* Count first: the strings come after the structure block. */
	if (!lay_out(fdt, &count))
		return false;
	total = count.struct_off + count.struct_len + count.strings_len;
	if (total > size)
		return false;
	w.strings_off = count.struct_off + count.struct_len;
	if (!lay_out(fdt, &w))
		return false;
	store_be32(b, FDT_MAGIC);
	store_be32(b + 4, (uint32_t)total);
	store_be32(b + 8, w.struct_off);
	store_be32(b + 12, w.strings_off);
	store_be32(b + 16, rsv);
	store_be32(b + 20, 17);
	store_be32(b + 24, 16);
	store_be32(b + 28, fdt->boot_cpuid_phys);
	store_be32(b + 32, w.strings_len);
	store_be32(b + 36, w.struct_len);
	for (size_t i = 0; i < fdt->num_reservations; i++) {
		store_be64(b + rsv + 16 * i, fdt->reservations[i].address);
		store_be64(b + rsv + 16 * i + 8, fdt->reservations[i].size);
	}
	memset(b + rsv + 16 * fdt->num_reservations, 0, 16);
	return true;
}
