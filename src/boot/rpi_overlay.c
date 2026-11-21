/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Devicetree overlays and parameters; see rpi_overlay.h. Where the
 * documentation is silent, this file chooses, and says so:
 *
 * - A parameter with no value ("dtparam=foo") is true for booleans,
 *   status, fragment operations and integers (1); a lookup table gives
 *   its default; a string or a byte string needs a value.
 * - In a lookup table, a value that matches no key passes through when
 *   the table allows it, before the default applies: "{,=18}" gives 18
 *   with no value and the value otherwise, as pps-rp1's pin parameter
 *   means it.
 * - Integers are decimal, or hexadecimal with 0x; on, off and the other
 *   boolean words count as 1 and 0.
 * - A false boolean removes the property from an overlay, but never from
 *   the base tree, which the documentation says it cannot change.
 * - An overlay's fragments are the children of its root that hold an
 *   __overlay__, in their order: the overlays that come with the
 *   firmware number theirs with gaps (i2c-fan has fragment@100), though
 *   the documentation asks for 0, 1, 2, ... Each finds its target when
 *   it merges, after what the fragments before it added. A fragment
 *   operation names fragment@N or fragment-N, the names that
 *   start.elf's strings give.
 * - Merging a node that has a phandle keeps it: the overlay's phandle
 *   for the same node is dropped. A bootargs property is appended to,
 *   after a space, as the firmware's command lines show.
 * - The labels in a fragment that targets the overlay itself are not
 *   exported: where they end up is not documented.
 */

#include "boot/rpi_overlay.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/byteorder.h"
#include "util/macros.h"

#define MAX_VALUE 512 /* the longest value a parameter writes */
#define MAX_NAME 128  /* the longest property name in a declaration */
#define MAX_PATH 512
#define MAX_MAP_STEPS 8

typedef struct Ctx {
	Fdt *base, *scope;
	char *error;
	size_t error_size;
} Ctx;

static bool fail(Ctx *c, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static bool fail(Ctx *c, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(c->error, c->error_size, fmt, ap);
	va_end(ap);
	return false;
}

/* --- Nodes and phandles ----------------------------------------------- */

/*
 * What is new goes first, before a node's other properties and children:
 * that is where libfdt, which fdtoverlay uses, puts it. The firmware's
 * order is not documented.
 */
static bool set_first_string(FdtNode *node, const char *name, const char *s)
{
	return fdt_set_first(node, name, s, strlen(s) + 1);
}

static FdtNode *exact_child(const FdtNode *parent, const char *name)
{
	for (FdtNode *n = fdt_first_child(parent); n != NULL; n = fdt_next_child(n))
		if (strcmp(fdt_name(n), name) == 0)
			return n;
	return NULL;
}

static FdtProp *find_prop(const FdtNode *node, const char *name)
{
	for (FdtProp *p = fdt_first_prop(node); p != NULL; p = fdt_next_prop(p))
		if (strcmp(fdt_prop_name(p), name) == 0)
			return p;
	return NULL;
}

/* Whether a property's value is a string: it ends with a NUL. */
static bool is_string(const void *value, size_t len)
{
	return len > 0 && ((const char *)value)[len - 1] == '\0';
}

/* The property name of node if it is a string, or NULL. */
static const char *get_string(const FdtNode *node, const char *name)
{
	size_t len;
	const char *v = node != NULL ? fdt_get(node, name, &len) : NULL;

	return v != NULL && is_string(v, len) ? v : NULL;
}

static bool is_phandle_prop(const char *name)
{
	return strcmp(name, "phandle") == 0 || strcmp(name, "linux,phandle") == 0;
}

static bool node_phandle(const FdtNode *node, uint32_t *phandle)
{
	return fdt_get_u32(node, "phandle", phandle) ||
	       fdt_get_u32(node, "linux,phandle", phandle);
}

static uint32_t max_phandle(const FdtNode *node)
{
	uint32_t max = 0, ph;

	if (node_phandle(node, &ph) && ph != 0xffffffffu)
		max = ph;
	for (FdtNode *n = fdt_first_child(node); n != NULL; n = fdt_next_child(n)) {
		uint32_t m = max_phandle(n);

		if (m > max)
			max = m;
	}
	return max;
}

static FdtNode *by_phandle(FdtNode *node, uint32_t phandle)
{
	uint32_t ph;

	if (node_phandle(node, &ph) && ph == phandle)
		return node;
	for (FdtNode *n = fdt_first_child(node); n != NULL; n = fdt_next_child(n)) {
		FdtNode *found = by_phandle(n, phandle);

		if (found != NULL)
			return found;
	}
	return NULL;
}

/* Add delta to every phandle that node and its children define. */
static void move_phandles(FdtNode *node, uint32_t delta)
{
	for (FdtProp *p = fdt_first_prop(node); p != NULL; p = fdt_next_prop(p)) {
		size_t len;
		uint8_t *v = fdt_prop_value(p, &len);

		if (is_phandle_prop(fdt_prop_name(p)) && len == 4)
			store_be32(v, load_be32(v) + delta);
	}
	for (FdtNode *n = fdt_first_child(node); n != NULL; n = fdt_next_child(n))
		move_phandles(n, delta);
}

/* A string of value at *at, which moves past it; NULL if unterminated. */
static const char *item_string(const uint8_t *value, size_t len, size_t *at)
{
	const uint8_t *end =
		*at < len ? memchr(value + *at, '\0', len - *at) : NULL;
	const char *s = (const char *)value + *at;

	if (end == NULL)
		return NULL;
	*at = (size_t)(end - value) + 1;
	return s;
}

/* --- Preparing an overlay --------------------------------------------- */

/* __local_fixups__: fix mirrors node; its properties list offsets. */
static bool local_fixups(Ctx *c, const FdtNode *fix, FdtNode *node,
                         uint32_t delta)
{
	for (FdtProp *p = fdt_first_prop(fix); p != NULL; p = fdt_next_prop(p)) {
		size_t len, target_len;
		const uint8_t *offsets = fdt_prop_value(p, &len);
		FdtProp *target = find_prop(node, fdt_prop_name(p));
		uint8_t *v;

		if (target == NULL || len % 4 != 0)
			return fail(c, "__local_fixups__ names %s of %s, which it has not",
			            fdt_prop_name(p), fdt_name(node));
		v = fdt_prop_value(target, &target_len);
		for (size_t i = 0; i < len; i += 4) {
			uint32_t off = load_be32(offsets + i);

			if (off > target_len || target_len - off < 4)
				return fail(c, "__local_fixups__: offset %u is outside %s",
				            (unsigned)off, fdt_prop_name(p));
			store_be32(v + off, load_be32(v + off) + delta);
		}
	}
	for (FdtNode *f = fdt_first_child(fix); f != NULL; f = fdt_next_child(f)) {
		FdtNode *n = exact_child(node, fdt_name(f));

		if (n == NULL)
			return fail(c, "__local_fixups__ names a node %s that is missing",
			            fdt_name(f));
		if (!local_fixups(c, f, n, delta))
			return false;
	}
	return true;
}

/* The node of base that label names: in __symbols__, or else aliases. */
static FdtNode *base_label(Fdt *base, const char *label)
{
	FdtNode *symbols = fdt_find(base, "/__symbols__");
	const char *path;

	if (symbols == NULL)
		symbols = fdt_find(base, "/aliases");
	path = get_string(symbols, label);
	return path != NULL ? fdt_find(base, path) : NULL;
}

/* Patch "path:property:offset" of overlay with phandle. */
static bool fix_up(Ctx *c, Fdt *overlay, const char *where, uint32_t phandle)
{
	const char *colon = strchr(where, ':'), *last = strrchr(where, ':');
	char path[MAX_PATH], name[MAX_NAME], *end;
	unsigned long off;
	FdtNode *node;
	FdtProp *prop;
	size_t len;
	uint8_t *v;

	if (colon == NULL || last == colon || (size_t)(colon - where) >= MAX_PATH ||
	    (size_t)(last - colon - 1) >= MAX_NAME)
		return fail(c, "__fixups__: \"%s\" is not path:property:offset", where);
	snprintf(path, sizeof(path), "%.*s", (int)(colon - where), where);
	snprintf(name, sizeof(name), "%.*s", (int)(last - colon - 1), colon + 1);
	off = strtoul(last + 1, &end, 10);
	node = fdt_find(overlay, path);
	prop = node != NULL ? find_prop(node, name) : NULL;
	if (prop == NULL || *end != '\0' || last[1] == '\0')
		return fail(c, "__fixups__: \"%s\" names nothing", where);
	v = fdt_prop_value(prop, &len);
	if (off > len || len - off < 4)
		return fail(c, "__fixups__: \"%s\" is outside its property", where);
	store_be32(v + off, phandle);
	return true;
}

bool rpi_overlay_prepare(Fdt *base, Fdt *overlay, char *error,
                         size_t error_size)
{
	Ctx c = {base, overlay, error, error_size};
	uint32_t delta = max_phandle(fdt_root(base));
	FdtNode *fixups = fdt_find(overlay, "/__fixups__");
	FdtNode *local = fdt_find(overlay, "/__local_fixups__");

	move_phandles(fdt_root(overlay), delta);
	if (local != NULL && !local_fixups(&c, local, fdt_root(overlay), delta))
		return false;
	if (fixups == NULL)
		return true;
	for (FdtProp *p = fdt_first_prop(fixups); p != NULL; p = fdt_next_prop(p)) {
		const char *label = fdt_prop_name(p);
		FdtNode *target = base_label(base, label);
		uint32_t phandle;
		size_t len, at = 0;
		const uint8_t *v = fdt_prop_value(p, &len);

		if (target == NULL)
			return fail(&c, "the base tree has no label %s", label);
		if (!node_phandle(target, &phandle)) {
			uint32_t a = max_phandle(fdt_root(base));
			uint32_t b = max_phandle(fdt_root(overlay));

			phandle = (a > b ? a : b) + 1;
			uint8_t cell[4];

			store_be32(cell, phandle);
			if (!fdt_set_first(target, "phandle", cell, sizeof(cell)))
				return fail(&c, "out of memory");
		}
		while (at < len) {
			const char *where = item_string(v, len, &at);

			if (where == NULL)
				return fail(&c, "__fixups__: %s is not a list of strings",
				            label);
			if (!fix_up(&c, overlay, where, phandle))
				return false;
		}
	}
	return true;
}

/* --- Values ----------------------------------------------------------- */

/* Whether text is one of words, in any case. */
static bool word_is(const char *text, const char *const *words, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const char *a = text, *b = words[i];

		while (*a != '\0' && (*a | 0x20) == *b) {
			a++;
			b++;
		}
		if (*a == '\0' && *b == '\0')
			return true;
	}
	return false;
}

static int hex_digit(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

static bool parse_int(const char *text, uint64_t *value);

/* A boolean: no value is true. */
static bool parse_bool(const char *text, bool *value)
{
	static const char *const yes[] = {"y", "yes", "on", "true", "okay"};
	static const char *const no[] = {"n", "no", "off", "false", "disabled"};
	uint64_t n;

	if (text == NULL || word_is(text, yes, ARRAY_SIZE(yes))) {
		*value = true;
		return true;
	}
	if (word_is(text, no, ARRAY_SIZE(no))) {
		*value = false;
		return true;
	}
	if (!parse_int(text, &n))
		return false;
	*value = n != 0;
	return true;
}

/* An integer: decimal, 0x hexadecimal, or a boolean word. */
static bool parse_int(const char *text, uint64_t *value)
{
	static const char *const yes[] = {"y", "yes", "on", "true"};
	static const char *const no[] = {"n", "no", "off", "false"};
	bool negative = false;
	const char *p = text;
	char *end;

	if (text == NULL || word_is(text, yes, ARRAY_SIZE(yes))) {
		*value = 1;
		return true;
	}
	if (word_is(text, no, ARRAY_SIZE(no))) {
		*value = 0;
		return true;
	}
	if (*p == '-') {
		negative = true;
		p++;
	}
	if (*p < '0' || *p > '9')
		return false;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		if (hex_digit(p[2]) < 0)
			return false;
		*value = strtoull(p + 2, &end, 16);
	} else {
		*value = strtoull(p, &end, 10);
	}
	if (*end != '\0')
		return false;
	if (negative)
		*value = (uint64_t)0 - *value;
	return true;
}

/*
 * A value for a target: text, or a cell that a declaration holds after
 * "=". absent: the parameter came with no value.
 */
typedef struct Value {
	const char *text;
	bool is_cell, absent;
	uint32_t cell;
	char buf[MAX_VALUE];
} Value;

static bool set_text(Ctx *c, Value *out, const char *text, size_t len)
{
	if (len >= sizeof(out->buf))
		return fail(c, "a value is longer than %zu bytes", sizeof(out->buf));
	memcpy(out->buf, text, len);
	out->buf[len] = '\0';
	out->text = out->buf;
	return true;
}

/* The next cell of the declaration, which "=" ended with. */
static bool next_cell(Ctx *c, const uint8_t *decl, size_t len, size_t *at,
                      uint32_t *cell)
{
	if (len - *at < 4)
		return fail(c, "a declaration ends where a value should be");
	*cell = load_be32(decl + *at);
	*at += 4;
	return true;
}

/*
 * A lookup table, from just after its "{": it looks up input, which may
 * be absent, into out. A key's value may be a cell, after which the table
 * goes on in the next string of the declaration.
 */
static bool lookup(Ctx *c, const char *p, const uint8_t *decl, size_t len,
                   size_t *at, bool cells, const Value *input, Value *out)
{
	Value def = {0}, found = {0};
	bool have_def = false, have_found = false, pass = false;
	bool after_cell = false;

	for (;;) {
		size_t key_len;
		const char *key;
		Value entry = {0};

		if (*p == '}' && after_cell)
			break;
		after_cell = false;
		key = p;
		key_len = strcspn(p, "=,}");
		p += key_len;
		if (*p == '=') {
			p++;
			if (*p == '\0') {
				if (!cells)
					return fail(c, "a string table holds a cell");
				entry.is_cell = true;
				if (!next_cell(c, decl, len, at, &entry.cell))
					return false;
				p = item_string(decl, len, at);
				if (p == NULL)
					return fail(c, "a lookup table does not end");
				after_cell = true;
			} else if (*p == '\'') {
				const char *q = strchr(p + 1, '\'');

				if (q == NULL)
					return fail(c, "a quote in a lookup table does not end");
				if (!set_text(c, &entry, p + 1, (size_t)(q - p - 1)))
					return false;
				p = q + 1;
			} else {
				size_t n = strcspn(p, ",}");

				if (!set_text(c, &entry, p, n))
					return false;
				p += n;
			}
			if (key_len == 0) {
				def = entry;
				have_def = true;
			}
		} else if (key_len == 0) {
			pass = true; /* an empty entry */
		} else if (!set_text(c, &entry, key, key_len)) {
			return false;
		}
		if (key_len != 0 && !have_found && !input->absent &&
		    strlen(input->text) == key_len &&
		    strncmp(input->text, key, key_len) == 0) {
			found = entry;
			have_found = true;
		}
		if (after_cell)
			continue;
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == '}')
			break;
		return fail(c, "a lookup table does not end");
	}
	if (p[1] != '\0')
		return fail(c, "something follows a lookup table");
	if (input->absent && !have_def)
		return fail(c, "it needs a value: its table has no default");
	if (input->absent)
		found = def;
	else if (!have_found && pass)
		return set_text(c, out, input->text, strlen(input->text));
	else if (!have_found && have_def)
		found = def;
	else if (!have_found)
		return fail(c, "its table has no entry for \"%s\"", input->text);
	*out = found;
	if (!out->is_cell)
		out->text = out->buf;
	return true;
}

/*
 * What follows a target's property and type: nothing (the parameter's
 * value), "=..." (a literal) or "{...}" (a lookup table).
 */
static bool resolve(Ctx *c, const char *rest, const uint8_t *decl, size_t len,
                    size_t *at, bool cells, const Value *input, Value *out)
{
	if (*rest == '\0') {
		*out = *input;
		return true;
	}
	if (*rest == '{')
		return lookup(c, rest + 1, decl, len, at, cells, input, out);
	if (*rest != '=')
		return fail(c, "\"%s\" is not a value, a literal or a table", rest);
	if (rest[1] != '\0')
		return set_text(c, out, rest + 1, strlen(rest + 1));
	if (cells) {
		out->is_cell = true;
		return next_cell(c, decl, len, at, &out->cell);
	}
	out->text = item_string(decl, len, at); /* = followed by a string */
	if (out->text == NULL)
		return fail(c, "a declaration ends where a value should be");
	return true;
}

/* --- Targets ---------------------------------------------------------- */

/* "reg" names the node's unit address too. */
static bool rename_unit(Ctx *c, FdtNode *node, uint64_t address)
{
	const char *name = fdt_name(node), *at = strchr(name, '@');
	char renamed[MAX_NAME + 20];

	if (at == NULL)
		return true;
	snprintf(renamed, sizeof(renamed), "%.*s@%llx", (int)(at - name), name,
	         (unsigned long long)address);
	return fdt_rename(node, renamed) || fail(c, "out of memory");
}

static bool set_int(Ctx *c, FdtNode *node, const char *prop, unsigned size,
                    unsigned long offset, const Value *v)
{
	size_t len = 0, new_len;
	const uint8_t *old = fdt_get(node, prop, &len);
	uint64_t value;
	uint8_t *buf;
	bool ok;

	if (v->is_cell)
		value = v->cell;
	else if (!parse_int(v->absent ? NULL : v->text, &value))
		return fail(c, "\"%s\" is not an integer", v->text);
	new_len = old != NULL && len > offset + size ? len : offset + size;
	buf = calloc(1, new_len);
	if (buf == NULL)
		return fail(c, "out of memory");
	if (old != NULL)
		memcpy(buf, old, len);
	for (unsigned i = 0; i < size; i++)
		buf[offset + i] = (uint8_t)(value >> (8 * (size - 1 - i)));
	ok = fdt_set_first(node, prop, buf, new_len);
	free(buf);
	if (!ok)
		return fail(c, "out of memory");
	if (strcmp(prop, "reg") == 0 && offset == 0)
		return rename_unit(c, node, value);
	return true;
}

static bool set_string(Ctx *c, FdtNode *node, const char *prop, const Value *v)
{
	const char *text = v->text;
	bool on;

	if (strcmp(prop, "status") == 0) {
		if (parse_bool(v->absent ? NULL : text, &on))
			text = on ? "okay" : "disabled";
	} else if (v->absent) {
		return fail(c, "it needs a value");
	}
	if (strcmp(prop, "name") == 0)
		return fdt_rename(node, text) || fail(c, "out of memory");
	if (strcmp(prop, "bootargs") == 0) {
		const char *old = get_string(node, prop);

		if (old != NULL && old[0] != '\0') {
			size_t len = strlen(old) + 1, n = strlen(text);
			char *joined = malloc(len + 1 + n);
			bool ok;

			if (joined == NULL)
				return fail(c, "out of memory");
			memcpy(joined, old, len - 1);
			joined[len - 1] = ' ';
			memcpy(joined + len, text, n + 1);
			ok = set_first_string(node, prop, joined);
			free(joined);
			return ok || fail(c, "out of memory");
		}
	}
	return set_first_string(node, prop, text) || fail(c, "out of memory");
}

static bool set_bool(Ctx *c, FdtNode *node, const char *prop, bool invert,
                     bool in_base, const Value *v)
{
	bool on;

	if (!parse_bool(v->absent ? NULL : v->text, &on))
		return fail(c, "\"%s\" is not a boolean", v->text);
	if (on != invert) {
		if (fdt_get(node, prop, &(size_t){0}) == NULL &&
		    !fdt_set_first(node, prop, "", 0))
			return fail(c, "out of memory");
	} else if (!in_base) {
		fdt_remove(node, prop);
	}
	return true;
}

static bool set_bytes(Ctx *c, FdtNode *node, const char *prop, const Value *v)
{
	uint8_t bytes[MAX_VALUE / 2];
	size_t n = 0;

	if (v->absent)
		return fail(c, "a byte string needs hexadecimal digits");
	for (const char *p = v->text; *p != '\0'; n++) {
		int hi = hex_digit(p[0]), lo = hi < 0 ? -1 : hex_digit(p[1]);

		if (lo < 0 || n == sizeof(bytes))
			return fail(c, "\"%s\" is not a byte string", v->text);
		bytes[n] = (uint8_t)(hi << 4 | lo);
		p += 2;
		if (*p == ':' && p[1] != '\0')
			p++;
	}
	if (n == 0)
		return fail(c, "a byte string needs hexadecimal digits");
	return fdt_set_first(node, prop, bytes, n) || fail(c, "out of memory");
}

/* One target: the node and the string that says what to set. */
static bool apply_target(Ctx *c, FdtNode *node, bool in_base, const char *s,
                         const uint8_t *decl, size_t len, size_t *at,
                         const Value *input)
{
	size_t n = s[0] != '\0' ? 1 + strcspn(s + 1, ".;:#?![{=") : 0;
	char prop[MAX_NAME];
	char kind = s[n];
	const char *rest = kind != '\0' ? s + n + 1 : s + n;
	Value v = {0};

	if (n == 0 || n >= sizeof(prop))
		return fail(c, "\"%s\" names no property", s);
	snprintf(prop, sizeof(prop), "%.*s", (int)n, s);
	switch (kind) {
	case '.':
	case ';':
	case ':':
	case '#': {
		unsigned size = kind == '.' ? 1 : kind == ';' ? 2 : kind == ':' ? 4 : 8;
		char *end;
		unsigned long offset = strtoul(rest, &end, 10);

		if (end == rest || offset > 0x10000)
			return fail(c, "\"%s\" has no offset", s);
		return resolve(c, end, decl, len, at, true, input, &v) &&
		       set_int(c, node, prop, size, offset, &v);
	}
	case '?':
	case '!':
		return resolve(c, rest, decl, len, at, false, input, &v) &&
		       set_bool(c, node, prop, kind == '!', in_base, &v);
	case '[':
		return resolve(c, rest, decl, len, at, false, input, &v) &&
		       set_bytes(c, node, prop, &v);
	case '{':
	case '=':
		rest--; /* the table or the literal starts at kind */
				/* fall through */
	default:
		return resolve(c, rest, decl, len, at, false, input, &v) &&
		       set_string(c, node, prop, &v);
	}
}

/* <0>,"+1-2=3!4": enable and disable the scope's fragments. */
static bool fragment_ops(Ctx *c, const char *ops, const Value *input)
{
	for (const char *p = ops; *p != '\0';) {
		char op = *p++, name[32], *end;
		unsigned long n = strtoul(p, &end, 10);
		FdtNode *frag, *body;
		bool on = true;

		if (strchr("+-=!", op) == NULL || end == p)
			return fail(c, "\"%s\" is not a list of fragment operations", ops);
		p = end;
		snprintf(name, sizeof(name), "fragment@%lu", n);
		frag = exact_child(fdt_root(c->scope), name);
		if (frag == NULL) {
			snprintf(name, sizeof(name), "fragment-%lu", n);
			frag = exact_child(fdt_root(c->scope), name);
		}
		if (frag == NULL)
			return fail(c, "fragment %lu not found", n);
		if (op == '-')
			on = false;
		if ((op == '=' || op == '!') &&
		    !parse_bool(input->absent ? NULL : input->text, &on))
			return fail(c, "\"%s\" is not a boolean", input->text);
		if (op == '!')
			on = !on;
		body = exact_child(frag, on ? "__dormant__" : "__overlay__");
		if (body != NULL &&
		    exact_child(frag, on ? "__overlay__" : "__dormant__") == NULL &&
		    !fdt_rename(body, on ? "__overlay__" : "__dormant__"))
			return fail(c, "out of memory");
	}
	return true;
}

RpiParamResult rpi_overlay_param(Fdt *base, Fdt *scope, const char *name,
                                 const char *value, char *error,
                                 size_t error_size)
{
	Ctx c = {base, scope, error, error_size};
	FdtNode *overrides = fdt_find(scope, "/__overrides__");
	Value input = {.text = value, .absent = value == NULL};
	const uint8_t *decl;
	size_t len, at = 0;

	decl = overrides != NULL ? fdt_get(overrides, name, &len) : NULL;
	if (decl == NULL)
		return RPI_PARAM_UNKNOWN;
	while (at < len) {
		uint32_t phandle = 0;
		const char *s;
		FdtNode *node;
		bool in_base = scope == base;

		if (!next_cell(&c, decl, len, &at, &phandle))
			return RPI_PARAM_FAILED;
		s = item_string(decl, len, &at);
		if (s == NULL) {
			fail(&c, "the declaration ends inside a string");
			return RPI_PARAM_FAILED;
		}
		if (phandle == 0) {
			if (scope == base) {
				fail(&c, "the base tree has no fragments");
				return RPI_PARAM_FAILED;
			}
			if (!fragment_ops(&c, s, &input))
				return RPI_PARAM_FAILED;
			continue;
		}
		node = by_phandle(fdt_root(scope), phandle);
		if (node == NULL && scope != base) {
			node = by_phandle(fdt_root(base), phandle);
			in_base = true;
		}
		if (node == NULL) {
			fail(&c, "no node has the phandle 0x%x", (unsigned)phandle);
			return RPI_PARAM_FAILED;
		}
		if (!apply_target(&c, node, in_base, s, decl, len, &at, &input))
			return RPI_PARAM_FAILED;
	}
	return RPI_PARAM_APPLIED;
}

/* --- Merging ---------------------------------------------------------- */

typedef struct Fragment {
	FdtNode *node, *body, *target; /* body: __overlay__ */
	bool intra;                    /* the target is in the overlay */
} Fragment;

static FdtNode *target_of(Ctx *c, Fdt *overlay, FdtNode *frag)
{
	uint32_t phandle;
	const char *path = get_string(frag, "target-path");
	FdtNode *node = NULL;

	if (fdt_get_u32(frag, "target", &phandle)) {
		node = by_phandle(fdt_root(overlay), phandle);
		if (node == NULL)
			node = by_phandle(fdt_root(c->base), phandle);
		if (node == NULL)
			fail(c, "%s: no node has the phandle 0x%x", fdt_name(frag),
			     (unsigned)phandle);
		return node;
	}
	if (path == NULL) {
		fail(c, "%s has no target", fdt_name(frag));
		return NULL;
	}
	if (path[0] != '/') {
		/* An alias, then maybe more path: "spi0/spidev@0". */
		FdtNode *aliases = fdt_find(c->base, "/aliases");
		size_t n = strcspn(path, "/");
		char name[MAX_NAME], full[MAX_PATH];
		const char *alias = NULL;

		if (n < sizeof(name)) {
			snprintf(name, sizeof(name), "%.*s", (int)n, path);
			alias = get_string(aliases, name);
		}
		if (alias != NULL && (size_t)snprintf(full, sizeof(full), "%s%s", alias,
		                                      path + n) < sizeof(full))
			node = fdt_find(c->base, full);
	} else {
		node = fdt_find(c->base, path);
	}
	if (node == NULL)
		fail(c, "%s: the base tree has no %s", fdt_name(frag), path);
	return node;
}

static bool merge_node(Ctx *c, FdtNode *dst, const FdtNode *src)
{
	uint32_t ph;

	for (FdtProp *p = fdt_first_prop(src); p != NULL; p = fdt_next_prop(p)) {
		const char *name = fdt_prop_name(p);
		size_t len;
		const uint8_t *v = fdt_prop_value(p, &len);
		bool ok;

		if (is_phandle_prop(name) && node_phandle(dst, &ph))
			continue;
		if (strcmp(name, "bootargs") == 0 && is_string(v, len)) {
			Value value = {.text = (const char *)v};

			ok = set_string(c, dst, name, &value);
		} else {
			ok = fdt_set_first(dst, name, v, len) || fail(c, "out of memory");
		}
		if (!ok)
			return false;
	}
	for (FdtNode *n = fdt_first_child(src); n != NULL; n = fdt_next_child(n)) {
		FdtNode *d = fdt_add_node_first(dst, fdt_name(n));

		if (d == NULL)
			return fail(c, "out of memory");
		if (!merge_node(c, d, n))
			return false;
	}
	return true;
}

/* The labels that the overlay exports go into base's __symbols__. */
static bool export_labels(Ctx *c, Fdt *overlay, const Fragment *frags,
                          size_t count)
{
	FdtNode *exports = fdt_find(overlay, "/__exports__");
	FdtNode *symbols = fdt_find(overlay, "/__symbols__"), *base_symbols;

	if (exports == NULL || symbols == NULL)
		return true;
	base_symbols = fdt_add_node_first(fdt_root(c->base), "__symbols__");
	if (base_symbols == NULL)
		return fail(c, "out of memory");
	for (FdtProp *p = fdt_first_prop(exports); p != NULL;
	     p = fdt_next_prop(p)) {
		const char *path = get_string(symbols, fdt_prop_name(p));
		char prefix[MAX_PATH], full[2 * MAX_PATH];

		if (path == NULL)
			continue;
		for (size_t i = 0; i < count; i++) {
			size_t n;
			const char *rest;

			if (!fdt_path(frags[i].body, prefix, sizeof(prefix)) ||
			    frags[i].intra)
				continue;
			n = strlen(prefix);
			if (strncmp(path, prefix, n) != 0 ||
			    (path[n] != '\0' && path[n] != '/'))
				continue;
			rest = path + n;
			if (!fdt_path(frags[i].target, prefix, sizeof(prefix)))
				return fail(c, "a path is too long");
			snprintf(full, sizeof(full), "%s%s",
			         strcmp(prefix, "/") == 0 && *rest != '\0' ? "" : prefix,
			         rest);
			if (!set_first_string(base_symbols, fdt_prop_name(p), full))
				return fail(c, "out of memory");
			break;
		}
	}
	return true;
}

/* A copy of base as a blob, to go back to; NULL when memory runs out. */
static uint8_t *save(Ctx *c, size_t *size)
{
	uint8_t *blob;

	*size = fdt_size(c->base);
	blob = malloc(*size);
	if (blob == NULL || !fdt_write(c->base, blob, *size)) {
		free(blob);
		fail(c, "out of memory");
		return NULL;
	}
	return blob;
}

bool rpi_overlay_merge(Fdt *base, Fdt *overlay, char *error, size_t error_size)
{
	Ctx c = {base, overlay, error, error_size};
	Fragment *frags = NULL;
	size_t count = 0, size;
	uint8_t *saved;
	bool ok = true;

	for (FdtNode *frag = fdt_first_child(fdt_root(overlay)); frag != NULL;
	     frag = fdt_next_child(frag)) {
		FdtNode *body = exact_child(frag, "__overlay__");
		Fragment *more;
		uint32_t phandle;

		if (body == NULL)
			continue; /* dormant, or not a fragment */
		more = realloc(frags, (count + 1) * sizeof(*frags));
		if (more == NULL) {
			free(frags);
			return fail(&c, "out of memory");
		}
		frags = more;
		frags[count].node = frag;
		frags[count].body = body;
		frags[count].target = NULL;
		frags[count++].intra = fdt_get_u32(frag, "target", &phandle) &&
		                       by_phandle(fdt_root(overlay), phandle) != NULL;
	}
	saved = save(&c, &size);
	if (saved == NULL) {
		free(frags);
		return false;
	}
	/*
	 * Each target is found when its fragment merges: a fragment may
	 * target what one before it added, as seeed-can-fd-hat-v1 does.
	 */
	for (int pass = 0; pass < 2 && ok; pass++) {
		for (size_t i = 0; i < count && ok; i++) {
			Fragment *f = &frags[i];

			if (f->intra != (pass == 0))
				continue;
			f->target = target_of(&c, overlay, f->node);
			ok = f->target != NULL && merge_node(&c, f->target, f->body);
		}
	}
	ok = ok && export_labels(&c, overlay, frags, count);
	if (!ok) {
		char why[32];
		Fdt *old = fdt_parse(saved, size, why, sizeof(why));

		if (old != NULL)
			fdt_swap(base, old); /* base as it was */
		fdt_free(old);
	}
	free(saved);
	free(frags);
	return ok;
}

/* --- The overlay map -------------------------------------------------- */

RpiMapResult rpi_overlay_map(Fdt *map, const char *platform, const char *name,
                             char *out, size_t out_size)
{
	bool renamed = false;

	snprintf(out, out_size, "%s", name);
	for (unsigned step = 0; step < MAX_MAP_STEPS; step++) {
		FdtNode *node = map != NULL ? exact_child(fdt_root(map), out) : NULL;
		const char *v;
		size_t len;

		if (node == NULL)
			return renamed ? RPI_MAP_RENAMED : RPI_MAP_LOAD;
		v = fdt_get(node, platform, &len);
		if (v != NULL) {
			if (!is_string(v, len) || v[0] == '\0' || strcmp(v, out) == 0)
				return renamed ? RPI_MAP_RENAMED : RPI_MAP_LOAD;
			snprintf(out, out_size, "%s", v);
			continue;
		}
		v = get_string(node, "renamed");
		if (v != NULL) {
			snprintf(out, out_size, "%s", v);
			renamed = true;
			continue;
		}
		v = get_string(node, "deprecated");
		if (v != NULL) {
			snprintf(out, out_size, "%s", v);
			return RPI_MAP_DEPRECATED;
		}
		return RPI_MAP_UNSUPPORTED;
	}
	return RPI_MAP_LOOP;
}

/* --- config.txt ------------------------------------------------------- */

static void warnf(RpiOverlays *o, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static void warnf(RpiOverlays *o, const char *fmt, ...)
{
	char message[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	o->warn(o->ctx, message);
}

/*
 * Set each param=value of list, which commas separate. The warnings say
 * what start.elf's strings say, with why.
 */
static void params(RpiOverlays *o, const char *what, const char *list)
{
	while (*list != '\0') {
		size_t len = strcspn(list, ",");
		char item[MAX_VALUE], why[256];
		char *value;
		RpiParamResult r = RPI_PARAM_UNKNOWN;

		if (len >= sizeof(item)) {
			warnf(o, "%s: a parameter is too long - ignored", what);
			return;
		}
		memcpy(item, list, len);
		item[len] = '\0';
		list += len + (list[len] == ',');
		if (len == 0)
			continue;
		value = strchr(item, '=');
		if (value != NULL)
			*value++ = '\0';
		if (o->pending != NULL)
			r = rpi_overlay_param(o->base, o->pending, item, value, why,
			                      sizeof(why));
		if (r == RPI_PARAM_UNKNOWN)
			r = rpi_overlay_param(o->base, o->base, item, value, why,
			                      sizeof(why));
		if (r == RPI_PARAM_UNKNOWN)
			warnf(o, "Unknown dtparam '%s' - ignored", item);
		else if (r == RPI_PARAM_FAILED)
			warnf(o, "dtparam '%s': %s - ignored", item, why);
	}
}

void rpi_overlays_end(RpiOverlays *o)
{
	char why[256];

	if (o->pending != NULL &&
	    !rpi_overlay_merge(o->base, o->pending, why, sizeof(why)))
		warnf(o, "Failed to merge overlay '%s': %s", o->pending_name, why);
	fdt_free(o->pending);
	o->pending = NULL;
	o->failed = false;
}

void rpi_overlays_dtoverlay(RpiOverlays *o, const char *line)
{
	size_t len = strcspn(line, ",");
	char name[sizeof(o->pending_name)], file[sizeof(o->pending_name)];
	char why[256];
	Fdt *overlay;

	rpi_overlays_end(o);
	if (len == 0)
		return; /* "dtoverlay=": back to the base's parameters */
	o->failed = true;
	if (len >= sizeof(name)) {
		warnf(o, "Failed to load overlay '%s': the name is too long", line);
		return;
	}
	snprintf(name, sizeof(name), "%.*s", (int)len, line);
	switch (rpi_overlay_map(o->map, o->platform, name, file, sizeof(file))) {
	case RPI_MAP_RENAMED:
		warnf(o, "overlay '%s' has been renamed '%s'", name, file);
		break;
	case RPI_MAP_DEPRECATED:
		warnf(o, "overlay '%s' is deprecated: %s", name, file);
		return;
	case RPI_MAP_UNSUPPORTED:
		warnf(o, "overlay '%s' is not supported on the '%s' platform", name,
		      o->platform);
		return;
	case RPI_MAP_LOOP:
		warnf(o, "overlay '%s': the overlay map goes round", name);
		return;
	case RPI_MAP_LOAD:
		break;
	}
	overlay = o->load(o->ctx, file, why, sizeof(why));
	if (overlay == NULL) {
		warnf(o, "Failed to load overlay '%s': %s", name, why);
		return;
	}
	if (!rpi_overlay_prepare(o->base, overlay, why, sizeof(why))) {
		warnf(o, "Failed to resolve overlay '%s': %s", name, why);
		fdt_free(overlay);
		return;
	}
	o->pending = overlay;
	o->failed = false;
	snprintf(o->pending_name, sizeof(o->pending_name), "%s", name);
	params(o, name, line + len + (line[len] == ','));
}

void rpi_overlays_dtparam(RpiOverlays *o, const char *line)
{
	if (o->failed)
		return; /* the parameters of an overlay that did not load */
	params(o, "dtparam", line);
}
