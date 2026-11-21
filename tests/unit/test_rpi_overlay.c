/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for src/boot/rpi_overlay.c on trees made here, for what dtc
 * does not write: broken declarations, fixups and fragments, and the
 * edges of the values. The cases of tests/unit/overlay check the rest on
 * trees that dtc compiles.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/rpi_overlay.h"
#include "test.h"
#include "util/byteorder.h"
#include "util/fdt.h"

/* A declaration: its bytes, with the cells written out, and its length. */
#define D(s) s, sizeof(s) - 1

static char error[256];

/*
 * A tree of just a root: the header, an empty reservation map, the
 * structure block (FDT_BEGIN_NODE "", FDT_END_NODE, FDT_END) and no
 * strings.
 */
static Fdt *empty_tree(void)
{
	static const uint32_t words[] = {
		FDT_MAGIC, 72, 56, 72, 40, 17, 16, 0, 0, 16, /* the header */
		0,         0,  0,  0,                        /* no reservations */
		1,         0,  2,  9,
	};
	uint8_t blob[sizeof(words)];
	Fdt *fdt;

	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		store_be32(blob + 4 * i, words[i]);
	fdt = fdt_parse(blob, sizeof(blob), error, sizeof(error));
	CHECK(fdt != NULL);
	return fdt;
}

/* The node at path, made with the nodes on the way if it is not there. */
static FdtNode *mk(Fdt *t, const char *path)
{
	char copy[1024];
	FdtNode *node = fdt_root(t);

	snprintf(copy, sizeof(copy), "%s", path);
	for (char *name = strtok(copy, "/"); name != NULL; name = strtok(NULL, "/"))
		node = fdt_add_node(node, name);
	return node;
}

static void set(Fdt *t, const char *path, const char *prop, const void *v,
                size_t len)
{
	CHECK(fdt_set(mk(t, path), prop, v, len));
}

static void set_s(Fdt *t, const char *path, const char *prop, const char *s)
{
	set(t, path, prop, s, strlen(s) + 1);
}

static void set_u32(Fdt *t, const char *path, const char *prop, uint32_t v)
{
	uint8_t cell[4];

	store_be32(cell, v);
	set(t, path, prop, cell, 4);
}

static const uint8_t *get(Fdt *t, const char *path, const char *prop,
                          size_t *len)
{
	FdtNode *node = fdt_find(t, path);

	*len = 0;
	return node != NULL ? fdt_get(node, prop, len) : NULL;
}

static bool is_s(Fdt *t, const char *path, const char *prop, const char *s)
{
	size_t len;
	const uint8_t *v = get(t, path, prop, &len);

	return v != NULL && len == strlen(s) + 1 && memcmp(v, s, len) == 0;
}

static uint32_t u32_of(Fdt *t, const char *path, const char *prop)
{
	size_t len;
	const uint8_t *v = get(t, path, prop, &len);

	CHECK(v != NULL && len >= 4);
	return v != NULL && len >= 4 ? load_be32(v) : 0xdeadbeef;
}

static bool has(Fdt *t, const char *path, const char *prop)
{
	size_t len;

	return get(t, path, prop, &len) != NULL;
}

/*
 * The base: /node@10 (phandle 1) and /plain (phandle 2), with labels for
 * both; bare leaves out the phandles, the labels and the aliases.
 */
static Fdt *base_tree(bool bare)
{
	Fdt *t = empty_tree();

	set_s(t, "/node@10", "s", "x");
	set_u32(t, "/node@10", "reg", 0x10);
	set_s(t, "/plain", "s", "y");
	if (bare)
		return t;
	set_u32(t, "/node@10", "phandle", 1);
	set_u32(t, "/plain", "phandle", 2);
	set_s(t, "/__symbols__", "node", "/node@10");
	set_s(t, "/__symbols__", "plain", "/plain");
	set(t, "/__symbols__", "empty", "", 0);
	set_s(t, "/aliases", "a", "/node@10");
	set(t, "/aliases", "empty", "", 0);
	return t;
}

/* Set /__overrides__/p of scope to decl and apply p=value. */
static RpiParamResult param(Fdt *base, Fdt *scope, const void *decl, size_t len,
                            const char *value)
{
	set(scope, "/__overrides__", "p", decl, len);
	error[0] = '\0';
	return rpi_overlay_param(base, scope, "p", value, error, sizeof(error));
}

/* p=value on a new base: it must fail with message. */
static void param_fails(const void *decl, size_t len, const char *value,
                        const char *message)
{
	Fdt *base = base_tree(false);
	RpiParamResult r = param(base, base, decl, len, value);

	if (r != RPI_PARAM_FAILED || strcmp(error, message) != 0)
		fprintf(stderr, "p=%s: %d \"%s\", expected \"%s\"\n",
		        value != NULL ? value : "(bare)", (int)r, error, message);
	CHECK(r == RPI_PARAM_FAILED);
	CHECK(strcmp(error, message) == 0);
	fdt_free(base);
}

/* p=value on a new base, which it must change: the caller frees it. */
static Fdt *param_ok(const void *decl, size_t len, const char *value)
{
	Fdt *base = base_tree(false);
	RpiParamResult r = param(base, base, decl, len, value);

	if (r != RPI_PARAM_APPLIED)
		fprintf(stderr, "p=%s: \"%s\"\n", value != NULL ? value : "(bare)",
		        error);
	CHECK(r == RPI_PARAM_APPLIED);
	return base;
}

/* What a declaration that does not hold together says. */
static void test_broken_declarations(void)
{
	static const struct {
		const char *decl;
		size_t len;
		const char *value, *error;
	} cases[] = {
		{D("\0\0"), "1", "a declaration ends where a value should be"},
		{D("\0\0\0\1"), "1", "the declaration ends inside a string"},
		{D("\0\0\0\1"
	       "s"),
	     "1", "the declaration ends inside a string"},
		{D("\0\0\0\1"
	       "s\0"
	       "\0\0\0"),
	     "1", "a declaration ends where a value should be"},
		{D("\0\0\0\1"
	       "u32:0=\0"
	       "\0\0\7"),
	     "1", "a declaration ends where a value should be"},
		{D("\0\0\0\1"
	       "\0"),
	     "1", "\"\" names no property"},
		{D("\0\0\0\1"
	       "u32:\0"),
	     "1", "\"u32:\" has no offset"},
		{D("\0\0\0\1"
	       "u8.65537\0"),
	     "1", "\"u8.65537\" has no offset"},
		{D("\0\0\0\1"
	       "u32:0abc\0"),
	     "1", "\"abc\" is not a value, a literal or a table"},
		{D("\0\0\0\1"
	       "s=\0"),
	     "1", "a declaration ends where a value should be"},
		{D("\0\0\0\1"
	       "s{a=\0"
	       "\0\0\0\5"
	       "}\0"),
	     "a", "a string table holds a cell"},
		{D("\0\0\0\1"
	       "u32:0{a=\0"),
	     "a", "a declaration ends where a value should be"},
		{D("\0\0\0\1"
	       "u32:0{a=\0"
	       "\0\0\0\5"),
	     "a", "a lookup table does not end"},
		{D("\0\0\0\1"
	       "u32:0{a=\0"
	       "\0\0\0\5"
	       "}\0"),
	     "b", "its table has no entry for \"b\""},
		{D("\0\0\0\1"
	       "s{a='x}\0"),
	     "a", "a quote in a lookup table does not end"},
		{D("\0\0\0\1"
	       "s{a=1}x\0"),
	     "a", "something follows a lookup table"},
		{D("\0\0\0\1"
	       "s{}\0"),
	     NULL, "it needs a value: its table has no default"},
		{D("\0\0\0\1"
	       "u32:0\0"),
	     "12abc", "\"12abc\" is not an integer"},
		{D("\0\0\0\1"
	       "flag?\0"),
	     "maybe", "\"maybe\" is not a boolean"},
		{D("\0\0\0\1"
	       "mac[\0"),
	     NULL, "a byte string needs hexadecimal digits"},
		{D("\0\0\0\1"
	       "mac[\0"),
	     "", "a byte string needs hexadecimal digits"},
		{D("\0\0\0\1"
	       "mac[\0"),
	     "aa:", "\"aa:\" is not a byte string"},
		{D("\0\0\0\0"
	       "+1\0"),
	     "1", "the base tree has no fragments"},
		{D("\0\0\0\7"
	       "s\0"),
	     "1", "no node has the phandle 0x7"},
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		param_fails(cases[i].decl, cases[i].len, cases[i].value,
		            cases[i].error);
}

/* "text" with quotes, cut as error is. */
static void quoted(char *out, const char *text, const char *after)
{
	static char full[1400];
	size_t n;

	snprintf(full, sizeof(full), "\"%s\"%s", text, after);
	n = strlen(full) < sizeof(error) - 1 ? strlen(full) : sizeof(error) - 1;
	memcpy(out, full, n);
	out[n] = '\0';
}

/* Values, names and tables as long as they may be, and longer. */
static void test_long(void)
{
	static const char *const forms[] = {"s=", "s{a='", "s{a=", "s{"};
	static const char too_long[] = "a value is longer than 512 bytes";
	static char decl[1200], value[1200];
	char expected[sizeof(error)];
	size_t at;
	Fdt *t;

	/* A property name of 128 characters is one too many. */
	memset(decl, 0, sizeof(decl));
	decl[3] = 1;
	memset(decl + 4, 'n', 128);
	quoted(expected, decl + 4, " names no property");
	param_fails(decl, 4 + 129, "1", expected);

	/* 512 characters of a literal, a quoted value, a value or a key. */
	for (size_t f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
		at = 4 + (size_t)sprintf(decl + 4, "%s", forms[f]);
		memset(decl + at, 'v', 512);
		at += 512;
		at += (size_t)sprintf(decl + at, "%s",
		                      f == 1  ? "'}"
		                      : f > 1 ? "}"
		                              : "");
		decl[at++] = '\0';
		param_fails(decl, at, "a", too_long);
	}

	/* A byte string of 200 bytes; 300 do not fit. */
	for (size_t i = 0; i < 200; i++)
		memcpy(value + 2 * i, i % 2 ? "5a" : "a5", 2);
	value[400] = '\0';
	t = param_ok(D("\0\0\0\1"
	               "mac[\0"),
	             value);
	{
		size_t len;
		const uint8_t *v = get(t, "/node@10", "mac", &len);

		CHECK_EQ(len, 200);
		CHECK(v != NULL && v[0] == 0xa5 && v[199] == 0x5a);
	}
	fdt_free(t);
	for (size_t i = 200; i < 300; i++)
		memcpy(value + 2 * i, "00", 2);
	value[600] = '\0';
	quoted(expected, value, " is not a byte string");
	param_fails(D("\0\0\0\1"
	              "mac[\0"),
	            value, expected);
}

/* The integers, booleans, byte strings and strings that targets get. */
static void test_values(void)
{
	static const struct {
		const char *value;
		uint32_t expected;
	} ints[] = {
		{"y", 1}, {"n", 0},     {"no", 0},          {"0xAf", 0xaf},
		{"9", 9}, {"0X10", 16}, {"-1", 0xffffffff}, {"0xfF", 0xff},
	};
	Fdt *t;
	size_t len;
	const uint8_t *v;

	for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
		t = param_ok(D("\0\0\0\1"
		               "u32:0\0"),
		             ints[i].value);
		if (u32_of(t, "/node@10", "u32") != ints[i].expected)
			fprintf(stderr, "u32=%s\n", ints[i].value);
		CHECK_EQ(u32_of(t, "/node@10", "u32"), ints[i].expected);
		fdt_free(t);
	}

	/* A literal cell at the very end of the declaration. */
	t = param_ok(D("\0\0\0\1"
	               "u32:0=\0"
	               "\0\0\0\7"),
	             "1");
	CHECK_EQ(u32_of(t, "/node@10", "u32"), 7);
	fdt_free(t);

	/* The largest offset. */
	t = param_ok(D("\0\0\0\1"
	               "u8.65536\0"),
	             "3");
	v = get(t, "/node@10", "u8", &len);
	CHECK_EQ(len, 65537);
	CHECK(v != NULL && v[65536] == 3 && v[0] == 0);
	fdt_free(t);

	/* A property name of one character; an empty literal takes a string. */
	t = param_ok(D("\0\0\0\1"
	               "x\0"),
	             "v");
	CHECK(is_s(t, "/node@10", "x", "v"));
	fdt_free(t);
	t = param_ok(D("\0\0\0\1"
	               "s=\0"
	               "lit\0"),
	             "1");
	CHECK(is_s(t, "/node@10", "s", "lit"));
	fdt_free(t);
	t = param_ok(D("\0\0\0\1"
	               "mac[=\0"
	               "0102\0"),
	             "1");
	v = get(t, "/node@10", "mac", &len);
	CHECK(len == 2 && v[0] == 1 && v[1] == 2);
	fdt_free(t);

	/* A boolean is an empty property; a false one leaves the base's. */
	t = param_ok(D("\0\0\0\1"
	               "flag?\0"),
	             "y");
	CHECK(get(t, "/node@10", "flag", &len) != NULL && len == 0);
	CHECK(param(t, t,
	            D("\0\0\0\1"
	              "flag?\0"),
	            "n") == RPI_PARAM_APPLIED);
	CHECK(has(t, "/node@10", "flag"));
	fdt_free(t);

	/* Byte strings, in either case, with a low digit of 0. */
	t = param_ok(D("\0\0\0\1"
	               "mac[\0"),
	             "a0:Fa:fA");
	v = get(t, "/node@10", "mac", &len);
	CHECK(len == 3 && v[0] == 0xa0 && v[1] == 0xfa && v[2] == 0xfa);
	CHECK(param(t, t,
	            D("\0\0\0\1"
	              "mac[\0"),
	            "10") == RPI_PARAM_APPLIED);
	v = get(t, "/node@10", "mac", &len);
	CHECK(len == 1 && v[0] == 0x10);
	fdt_free(t);

	/* reg of a node without a unit address leaves its name. */
	t = param_ok(D("\0\0\0\2"
	               "reg:0\0"),
	             "5");
	CHECK_EQ(u32_of(t, "/plain", "reg"), 5);
	CHECK(strcmp(fdt_name(fdt_find(t, "/plain")), "plain") == 0);
	fdt_free(t);

	/* bootargs: words go after what is there, if anything is. */
	static const struct {
		const char *old, *expected; /* old NULL: no bootargs */
	} args[] = {{"", "w"}, {"a", "a w"}, {NULL, "w"}};
	for (size_t i = 0; i < sizeof(args) / sizeof(args[0]); i++) {
		t = base_tree(false);
		if (args[i].old != NULL)
			set_s(t, "/node@10", "bootargs", args[i].old);
		CHECK(param(t, t,
		            D("\0\0\0\1"
		              "bootargs\0"),
		            "w") == RPI_PARAM_APPLIED);
		CHECK(is_s(t, "/node@10", "bootargs", args[i].expected));
		fdt_free(t);
	}
}

/* Lookup tables: cells, defaults, empty entries and quotes. */
static void test_tables(void)
{
	static const struct {
		const char *value;
		uint32_t expected;
	} cells[] = {{"a", 5}, {"b", 2}, {"c", 3}, {"zz", 4}};
	Fdt *t;

	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		t = param_ok(D("\0\0\0\1"
		               "u32:0{a=\0"
		               "\0\0\0\5"
		               "b=2,c=3,=4}\0"),
		             cells[i].value);
		CHECK_EQ(u32_of(t, "/node@10", "u32"), cells[i].expected);
		fdt_free(t);
	}
	t = param_ok(D("\0\0\0\1"
	               "s{}\0"),
	             "zz");
	CHECK(is_s(t, "/node@10", "s", "zz"));
	fdt_free(t);
	t = param_ok(D("\0\0\0\1"
	               "s{a=''}\0"),
	             "a");
	CHECK(is_s(t, "/node@10", "s", ""));
	fdt_free(t);
	t = param_ok(D("\0\0\0\1"
	               "s{a=1,=2}\0"),
	             "a");
	CHECK(is_s(t, "/node@10", "s", "1"));
	fdt_free(t);
	t = param_ok(D("\0\0\0\1"
	               "s{a=1,=2}\0"),
	             "q");
	CHECK(is_s(t, "/node@10", "s", "2"));
	fdt_free(t);
}

/* The parameters of an overlay: on its nodes, the base's, and fragments. */
static void test_overlay_params(void)
{
	Fdt *base = base_tree(false), *ov = empty_tree();
	size_t len;

	set_u32(ov, "/fragment@0", "target", 1);
	set_u32(ov, "/fragment@0/__overlay__", "phandle", 3);
	set(ov, "/fragment@0/__overlay__", "flag", "", 0);
	set_u32(ov, "/fragment@12", "target", 1);
	mk(ov, "/fragment@12/__dormant__");
	set(base, "/node@10", "flag", "", 0);

	/* A false boolean takes it out of the overlay, never the base. */
	CHECK(param(base, ov,
	            D("\0\0\0\3"
	              "flag?=\0"
	              "no\0"),
	            NULL) == RPI_PARAM_APPLIED);
	CHECK(!has(ov, "/fragment@0/__overlay__", "flag"));
	CHECK(param(base, ov,
	            D("\0\0\0\1"
	              "flag?\0"),
	            "no") == RPI_PARAM_APPLIED);
	CHECK(has(base, "/node@10", "flag"));
	CHECK(get(base, "/node@10", "flag", &len) != NULL && len == 0);

	/* Fragments with numbers of two digits. */
	CHECK(param(base, ov,
	            D("\0\0\0\0"
	              "+12\0"),
	            NULL) == RPI_PARAM_APPLIED);
	CHECK(fdt_find(ov, "/fragment@12/__overlay__") != NULL);
	CHECK(fdt_find(ov, "/fragment@12/__dormant__") == NULL);
	static const char *const bad[] = {"x1", "+"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		char decl[16], expected[64];

		memset(decl, 0, 4);
		size_t n = 4 + (size_t)sprintf(decl + 4, "%s", bad[i]) + 1;
		snprintf(expected, sizeof(expected),
		         "\"%s\" is not a list of fragment operations", bad[i]);
		CHECK(param(base, ov, decl, n, NULL) == RPI_PARAM_FAILED);
		if (strcmp(error, expected) != 0)
			fprintf(stderr, "%s: \"%s\"\n", bad[i], error);
		CHECK(strcmp(error, expected) == 0);
	}
	fdt_free(ov);
	fdt_free(base);
}

/*
 * An overlay to prepare: fragment@0 targets the label node; b refers to
 * the base's node at offset 12 of refs, and to a and itself in pair.
 */
static Fdt *overlay_tree(void)
{
	static const uint8_t refs[16] = {
		[15] = 0xff, [14] = 0xff, [13] = 0xff, [12] = 0xff};
	static const uint8_t pair[12] = {[3] = 1, [7] = 7, [11] = 2};
	static const char fixups[] = "/fragment@0:target:0\0"
								 "/fragment@0/__overlay__/b:refs:12";
	static const uint8_t offsets[8] = {[7] = 8};
	Fdt *t = empty_tree();

	set_u32(t, "/fragment@0", "target", 0xffffffff);
	set_u32(t, "/fragment@0/__overlay__/a", "phandle", 1);
	set_u32(t, "/fragment@0/__overlay__/b", "phandle", 2);
	set(t, "/fragment@0/__overlay__/b", "refs", refs, sizeof(refs));
	set(t, "/fragment@0/__overlay__/b", "pair", pair, sizeof(pair));
	set(t, "/__fixups__", "node", fixups, sizeof(fixups));
	set(t, "/__local_fixups__/fragment@0/__overlay__/b", "pair", offsets,
	    sizeof(offsets));
	return t;
}

static bool prepare(Fdt *base, Fdt *ov)
{
	error[0] = '\0';
	return rpi_overlay_prepare(base, ov, error, sizeof(error));
}

static void prepare_fails(Fdt *ov, const char *message)
{
	Fdt *base = base_tree(false);

	CHECK(!prepare(base, ov));
	if (strcmp(error, message) != 0)
		fprintf(stderr, "\"%s\", expected \"%s\"\n", error, message);
	CHECK(strcmp(error, message) == 0);
	fdt_free(base);
	fdt_free(ov);
}

/* Fixups and local fixups: what they patch, and what is wrong with them. */
static void test_prepare(void)
{
	static const struct {
		const char *where, *error;
	} fixups[] = {
		{"nocolon", "__fixups__: \"nocolon\" is not path:property:offset"},
		{"/fragment@0:target",
	     "__fixups__: \"/fragment@0:target\" is not path:property:offset"},
		{"/fragment@0:target:", "__fixups__: \"/fragment@0:target:\" names "
	                            "nothing"},
		{"/fragment@0:target:4x",
	     "__fixups__: \"/fragment@0:target:4x\" names nothing"},
		{"/nothing:target:0",
	     "__fixups__: \"/nothing:target:0\" names nothing"},
		{"/fragment@0:target:1",
	     "__fixups__: \"/fragment@0:target:1\" is outside its property"},
		{"/fragment@0:target:8",
	     "__fixups__: \"/fragment@0:target:8\" is outside its property"},
	};
	static const struct {
		uint32_t offset;
		const char *error;
	} locals[] = {
		{9, "__local_fixups__: offset 9 is outside pair"},
		{16, "__local_fixups__: offset 16 is outside pair"},
	};
	Fdt *base = base_tree(false), *ov = overlay_tree();
	size_t len;
	const uint8_t *v;

	/* The base's largest phandle is 2: a and b become 3 and 4. */
	CHECK(prepare(base, ov));
	CHECK_EQ(u32_of(ov, "/fragment@0", "target"), 1);
	CHECK_EQ(u32_of(ov, "/fragment@0/__overlay__/a", "phandle"), 3);
	CHECK_EQ(u32_of(ov, "/fragment@0/__overlay__/b", "phandle"), 4);
	v = get(ov, "/fragment@0/__overlay__/b", "refs", &len);
	CHECK(len == 16 && load_be32(v + 12) == 1 && load_be32(v) == 0);
	v = get(ov, "/fragment@0/__overlay__/b", "pair", &len);
	CHECK(len == 12 && load_be32(v) == 3 && load_be32(v + 4) == 7 &&
	      load_be32(v + 8) == 4);
	fdt_free(ov);
	fdt_free(base);

	/* Without phandles in the base, those of the overlay stay. */
	base = base_tree(true);
	set_s(base, "/__symbols__", "node", "/node@10");
	ov = overlay_tree();
	CHECK(prepare(base, ov));
	CHECK_EQ(u32_of(ov, "/fragment@0/__overlay__/a", "phandle"), 1);
	CHECK_EQ(u32_of(base, "/node@10", "phandle"), 3); /* one is given */
	fdt_free(ov);
	fdt_free(base);

	/* A phandle of 0xffffffff in the base does not count. */
	base = base_tree(false);
	set_u32(base, "/odd", "phandle", 0xffffffff);
	ov = overlay_tree();
	CHECK(prepare(base, ov));
	CHECK_EQ(u32_of(ov, "/fragment@0/__overlay__/a", "phandle"), 3);
	fdt_free(ov);
	fdt_free(base);

	/* The labels: in __symbols__, or else in aliases. */
	base = base_tree(true);
	ov = overlay_tree();
	CHECK(!prepare(base, ov));
	CHECK(strcmp(error, "the base tree has no label node") == 0);
	set_s(base, "/aliases", "node", "/node@10");
	CHECK(prepare(base, ov));
	fdt_free(ov);
	fdt_free(base);
	ov = overlay_tree();
	set(ov, "/__fixups__", "empty", "/fragment@0:target:0", 21);
	prepare_fails(ov, "the base tree has no label empty");

	for (size_t i = 0; i < sizeof(fixups) / sizeof(fixups[0]); i++) {
		ov = overlay_tree();
		set_s(ov, "/__fixups__", "node", fixups[i].where);
		prepare_fails(ov, fixups[i].error);
	}
	ov = overlay_tree();
	set(ov, "/__fixups__", "node", "/fragment@0:target:0", 20);
	prepare_fails(ov, "__fixups__: node is not a list of strings");

	for (size_t i = 0; i < sizeof(locals) / sizeof(locals[0]); i++) {
		ov = overlay_tree();
		set_u32(ov, "/__local_fixups__/fragment@0/__overlay__/b", "pair",
		        locals[i].offset);
		prepare_fails(ov, locals[i].error);
	}
	ov = overlay_tree();
	set_u32(ov, "/__local_fixups__/fragment@0/__overlay__/b", "nosuch", 0);
	prepare_fails(ov, "__local_fixups__ names nosuch of b, which it has not");
	ov = overlay_tree();
	set(ov, "/__local_fixups__/fragment@0/__overlay__/b", "pair", "\0\0", 2);
	prepare_fails(ov, "__local_fixups__ names pair of b, which it has not");
	ov = overlay_tree();
	mk(ov, "/__local_fixups__/fragment@0/__overlay__/c");
	prepare_fails(ov, "__local_fixups__ names a node c that is missing");
}

static bool merge(Fdt *base, Fdt *ov)
{
	error[0] = '\0';
	return rpi_overlay_merge(base, ov, error, sizeof(error));
}

/* Targets, phandles and bootargs of fragments, and what goes wrong. */
static void test_merge(void)
{
	static char long_alias[200];
	static const struct {
		const char *path; /* NULL: an empty property */
		bool bare;
		const char *error;
	} paths[] = {
		{NULL, false, "fragment@0 has no target"},
		{"", false, "fragment@0: the base tree has no "},
		{"a", true, "fragment@0: the base tree has no a"},
		{"empty", false, "fragment@0: the base tree has no empty"},
		{"a/nothing", false, "fragment@0: the base tree has no a/nothing"},
	};
	Fdt *base, *ov;
	size_t len;

	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		base = base_tree(paths[i].bare);
		ov = empty_tree();
		if (paths[i].path != NULL)
			set_s(ov, "/fragment@0", "target-path", paths[i].path);
		else
			set(ov, "/fragment@0", "target-path", "", 0);
		mk(ov, "/fragment@0/__overlay__");
		CHECK(!merge(base, ov));
		if (strcmp(error, paths[i].error) != 0)
			fprintf(stderr, "\"%s\", expected \"%s\"\n", error, paths[i].error);
		CHECK(strcmp(error, paths[i].error) == 0);
		fdt_free(ov);
		fdt_free(base);
	}

	/* An alias of 128 characters is too long to be one. */
	base = base_tree(false);
	ov = empty_tree();
	memset(long_alias, 'a', 128);
	set_s(base, "/aliases", long_alias, "/node@10");
	set_s(ov, "/fragment@0", "target-path", long_alias);
	mk(ov, "/fragment@0/__overlay__");
	CHECK(!merge(base, ov));
	CHECK(strstr(error, "the base tree has no aaa") != NULL);
	fdt_free(ov);
	fdt_free(base);

	base = base_tree(false);
	ov = empty_tree();
	set_u32(ov, "/fragment@0", "target", 0x9999);
	mk(ov, "/fragment@0/__overlay__");
	CHECK(!merge(base, ov));
	CHECK(strcmp(error, "fragment@0: no node has the phandle 0x9999") == 0);
	fdt_free(ov);

	/*
	 * A node keeps the phandle it has and gets the overlay's if it has
	 * none; an empty bootargs is not a string, and replaces.
	 */
	ov = empty_tree();
	set_u32(ov, "/fragment@0", "target", 1);
	set_u32(ov, "/fragment@0/__overlay__", "phandle", 5);
	set(ov, "/fragment@0/__overlay__", "bootargs", "", 0);
	set_s(ov, "/fragment@1", "target-path", "/aliases");
	set_u32(ov, "/fragment@1/__overlay__", "phandle", 6);
	set_s(base, "/node@10", "bootargs", "x");
	CHECK(merge(base, ov));
	CHECK_EQ(u32_of(base, "/node@10", "phandle"), 1);
	CHECK_EQ(u32_of(base, "/aliases", "phandle"), 6);
	CHECK(get(base, "/node@10", "bootargs", &len) != NULL && len == 0);
	fdt_free(ov);
	fdt_free(base);
}

/* __exports__: the labels go to the base where their fragments put them. */
static void test_exports(void)
{
	static const char *const names[] = {"lab",     "top",   "weird",
	                                    "nowhere", "rootc", "roottop",
	                                    "intra",   "first", "missing"};
	Fdt *base = base_tree(false), *ov = empty_tree();

	set_u32(ov, "/fragment@0", "target", 1);
	mk(ov, "/fragment@0/__overlay__");
	set_s(ov, "/fragment@1", "target-path", "/plain");
	set_u32(ov, "/fragment@1/__overlay__/x", "phandle", 7);
	set_s(ov, "/fragment@2", "target-path", "/");
	mk(ov, "/fragment@2/__overlay__/c");
	set_u32(ov, "/fragment@3", "target", 7);
	mk(ov, "/fragment@3/__overlay__/i");
	set_s(ov, "/__symbols__", "lab", "/fragment@1/__overlay__/x");
	set_s(ov, "/__symbols__", "top", "/fragment@1/__overlay__");
	set_s(ov, "/__symbols__", "weird", "/fragment@1/__overlay__x");
	set_s(ov, "/__symbols__", "nowhere", "/elsewhere");
	set_s(ov, "/__symbols__", "rootc", "/fragment@2/__overlay__/c");
	set_s(ov, "/__symbols__", "roottop", "/fragment@2/__overlay__");
	set_s(ov, "/__symbols__", "intra", "/fragment@3/__overlay__/i");
	set_s(ov, "/__symbols__", "first", "/fragment@0/__overlay__");
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		set(ov, "/__exports__", names[i], "", 0);
	CHECK(merge(base, ov));
	CHECK(is_s(base, "/__symbols__", "lab", "/plain/x"));
	CHECK(is_s(base, "/__symbols__", "top", "/plain"));
	CHECK(is_s(base, "/__symbols__", "rootc", "/c"));
	CHECK(is_s(base, "/__symbols__", "roottop", "/"));
	CHECK(is_s(base, "/__symbols__", "first", "/node@10"));
	CHECK(!has(base, "/__symbols__", "weird"));
	CHECK(!has(base, "/__symbols__", "nowhere"));
	CHECK(!has(base, "/__symbols__", "intra"));
	CHECK(!has(base, "/__symbols__", "missing"));
	CHECK(fdt_find(base, "/plain/x/i") != NULL);
	CHECK_EQ(u32_of(base, "/plain", "phandle"), 2);
	CHECK_EQ(u32_of(base, "/plain/x", "phandle"), 7);
	fdt_free(ov);
	fdt_free(base);

	/* Without both __exports__ and __symbols__, nothing is exported. */
	for (int which = 0; which < 2; which++) {
		base = base_tree(true);
		ov = empty_tree();
		set_s(ov, "/fragment@0", "target-path", "/plain");
		mk(ov, "/fragment@0/__overlay__/x");
		if (which == 0)
			set_s(ov, "/__symbols__", "x", "/fragment@0/__overlay__/x");
		else
			set(ov, "/__exports__", "x", "", 0);
		CHECK(merge(base, ov));
		CHECK(fdt_find(base, "/__symbols__") == NULL);
		fdt_free(ov);
		fdt_free(base);
	}
}

/* The map's entries for a platform, renamed and deprecated. */
static void test_map(void)
{
	Fdt *map = empty_tree();
	char out[64];

	set_s(map, "/m1", "bcm2835", "");
	set_s(map, "/m2", "bcm2835", "z");
	set(map, "/m3", "bcm2835", "ab", 2);
	set(map, "/m4", "renamed", "", 0);
	set(map, "/m4", "deprecated", "", 0);
	set_s(map, "/m5", "deprecated", "why");
	CHECK(rpi_overlay_map(map, "bcm2835", "m1", out, sizeof(out)) ==
	      RPI_MAP_LOAD);
	CHECK(strcmp(out, "m1") == 0);
	CHECK(rpi_overlay_map(map, "bcm2835", "m2", out, sizeof(out)) ==
	      RPI_MAP_LOAD);
	CHECK(strcmp(out, "z") == 0);
	CHECK(rpi_overlay_map(map, "bcm2835", "m3", out, sizeof(out)) ==
	      RPI_MAP_LOAD);
	CHECK(strcmp(out, "m3") == 0);
	CHECK(rpi_overlay_map(map, "bcm2835", "m4", out, sizeof(out)) ==
	      RPI_MAP_UNSUPPORTED);
	CHECK(rpi_overlay_map(map, "bcm2835", "m5", out, sizeof(out)) ==
	      RPI_MAP_DEPRECATED);
	CHECK(strcmp(out, "why") == 0);
	fdt_free(map);
}

static char warnings[2048];

static void warn(void *ctx, const char *message)
{
	(void)ctx;
	snprintf(warnings + strlen(warnings), sizeof(warnings) - strlen(warnings),
	         "%s\n", message);
}

/* "good" is an overlay with a parameter s for the base's node. */
static Fdt *load(void *ctx, const char *name, char *why, size_t size)
{
	Fdt *t;

	(void)ctx;
	if (strcmp(name, "good") != 0) {
		snprintf(why, size, "no %s.dtbo", name);
		return NULL;
	}
	t = empty_tree();
	set_u32(t, "/fragment@0", "target", 1);
	mk(t, "/fragment@0/__overlay__");
	set(t, "/__overrides__", "s", "\0\0\0\1s", 6);
	return t;
}

/* config.txt's lines: long and empty parameters, and failed overlays. */
static void test_lines(void)
{
	static char line[700], expected[800];
	RpiOverlays o = {.platform = "bcm2835", .load = load, .warn = warn};

	o.base = base_tree(false);
	set(o.base, "/__overrides__", "s", "\0\0\0\1s", 6);
	warnings[0] = '\0';
	rpi_overlays_dtoverlay(&o, "good,,x,s=1,");
	CHECK(strcmp(warnings, "Unknown dtparam 'x' - ignored\n") == 0);
	CHECK(is_s(o.base, "/node@10", "s", "1"));
	rpi_overlays_end(&o);

	/* A parameter of 512 characters is too long. */
	for (size_t n = 600; n >= 512; n -= 88) {
		memset(line, 'p', n);
		line[n] = '\0';
		warnings[0] = '\0';
		rpi_overlays_dtparam(&o, line);
		CHECK(strcmp(warnings,
		             "dtparam: a parameter is too long - ignored\n") == 0);
	}

	/* After a failed overlay, dtparam waits for the next dtoverlay. */
	warnings[0] = '\0';
	rpi_overlays_dtoverlay(&o, "none");
	rpi_overlays_dtparam(&o, "s=dropped");
	CHECK(is_s(o.base, "/node@10", "s", "1"));
	rpi_overlays_dtoverlay(&o, "");
	rpi_overlays_dtparam(&o, "s=later");
	CHECK(is_s(o.base, "/node@10", "s", "later"));
	CHECK(strcmp(warnings, "Failed to load overlay 'none': no none.dtbo\n") ==
	      0);

	/* A name of 64 characters is too long. */
	memset(line, 'n', 64);
	strcpy(line + 64, ",s=x");
	warnings[0] = '\0';
	rpi_overlays_dtoverlay(&o, line);
	rpi_overlays_dtparam(&o, "s=dropped");
	CHECK(is_s(o.base, "/node@10", "s", "later"));
	snprintf(expected, sizeof(expected),
	         "Failed to load overlay '%s': the name is too long\n", line);
	CHECK(strcmp(warnings, expected) == 0);
	rpi_overlays_end(&o);
	fdt_free(o.base);
}

int main(void)
{
	RUN_TEST(test_broken_declarations);
	RUN_TEST(test_long);
	RUN_TEST(test_values);
	RUN_TEST(test_tables);
	RUN_TEST(test_overlay_params);
	RUN_TEST(test_prepare);
	RUN_TEST(test_merge);
	RUN_TEST(test_exports);
	RUN_TEST(test_map);
	RUN_TEST(test_lines);
	return test_summary();
}
