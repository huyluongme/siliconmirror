/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for flattened devicetrees (src/util/fdt.c), on blobs that
 * fdt_blob.h builds.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fdt_blob.h"
#include "test.h"
#include "util/byteorder.h"
#include "util/fdt.h"
#include "util/macros.h"

/* A small tree like a board's: the blob of the tests. */
static size_t sample(uint8_t **blob)
{
	static Builder b;

	memset(&b, 0, sizeof(b));
	b.reservations[0][0] = 0x1000;
	b.reservations[0][1] = 0x2000;
	b.num_reservations = 1;
	word(&b, 4); /* FDT_NOP, ignored */
	begin(&b, "");
	prop(&b, "model", "Test board", 11);
	prop_u32(&b, "#address-cells", 1);
	begin(&b, "chosen");
	prop(&b, "bootargs", "quiet", 6);
	prop(&b, "empty", "", 0);
	end(&b);
	begin(&b, "memory@0");
	prop(&b, "device_type", "memory", 7);
	prop_u32(&b, "reg", 0);
	end(&b);
	begin(&b, "cpus");
	begin(&b, "cpu@0");
	prop_u32(&b, "reg", 0);
	end(&b);
	begin(&b, "cpu@1");
	prop_u32(&b, "reg", 1);
	word(&b, 4); /* FDT_NOP */
	end(&b);
	end(&b);
	begin(&b, "serial");
	end(&b);
	begin(&b, "serial@7e201000");
	end(&b);
	end(&b);
	return finish(&b, blob);
}

static Fdt *parse(const uint8_t *blob, size_t size)
{
	char error[128] = "";
	Fdt *fdt = fdt_parse(blob, size, error, sizeof(error));

	CHECK(fdt != NULL);
	CHECK(strcmp(error, "") == 0);
	return fdt;
}

/* Paths, with and without unit addresses, and properties. */
static void test_find(void)
{
	uint8_t *blob;
	size_t size = sample(&blob);
	Fdt *fdt = parse(blob, size);
	FdtNode *root = fdt_find(fdt, "/"), *node;
	const char *value;
	size_t len = 99;
	uint32_t cell = 99;

	CHECK(root != NULL);
	value = fdt_get(root, "model", &len);
	CHECK(value != NULL && strcmp(value, "Test board") == 0);
	CHECK_EQ(len, 11);
	CHECK(fdt_get_u32(root, "#address-cells", &cell));
	CHECK_EQ(cell, 1);
	CHECK(!fdt_get_u32(root, "model", &cell)); /* not 4 bytes */
	CHECK(!fdt_get_u32(root, "missing", &cell));
	CHECK(fdt_get(root, "missing", &len) == NULL);

	node = fdt_find(fdt, "/chosen");
	CHECK(node != NULL);
	CHECK(fdt_find(fdt, "//chosen/") == node);
	CHECK(fdt_child(root, "chosen") == node);
	value = fdt_get(node, "bootargs", &len);
	CHECK(value != NULL && strcmp(value, "quiet") == 0);
	CHECK(fdt_get(node, "empty", &len) != NULL);
	CHECK_EQ(len, 0);

	/* The unit address may be left out when the path stays clear. */
	node = fdt_find(fdt, "/memory");
	CHECK(node != NULL);
	CHECK(fdt_find(fdt, "/memory@0") == node);
	CHECK(fdt_find(fdt, "/memory@1") == NULL);
	CHECK(fdt_find(fdt, "/memor") == NULL);
	CHECK(fdt_find(fdt, "/cpus/cpu") == NULL); /* two cpu nodes */
	node = fdt_find(fdt, "/cpus/cpu@1");
	CHECK(node != NULL && fdt_get_u32(node, "reg", &cell));
	CHECK_EQ(cell, 1);
	CHECK(fdt_find(fdt, "/cpus/cpu@1/x") == NULL);
	/* A node named exactly so wins over one with a unit address. */
	node = fdt_find(fdt, "/serial");
	CHECK(node != NULL && node == fdt_child(root, "serial"));
	CHECK(node != fdt_find(fdt, "/serial@7e201000"));
	CHECK(fdt_find(fdt, "chosen") == NULL);
	CHECK(fdt_find(fdt, "") == NULL);
	fdt_free(fdt);
	free(blob);
}

/* What is read and not changed is written back byte for byte. */
static void test_round_trip(void)
{
	uint8_t *blob, *out;
	size_t size = sample(&blob);
	Fdt *fdt = parse(blob, size);

	/* The NOPs of the input are dropped. */
	CHECK_EQ(fdt_size(fdt), size - 8);
	out = malloc(size);
	CHECK(out != NULL);
	CHECK(!fdt_write(fdt, out, fdt_size(fdt) - 1));
	CHECK(fdt_write(fdt, out, size));
	fdt_free(fdt);
	free(blob);

	/* The same tree without the NOPs: the same bytes. */
	Builder b;

	memset(&b, 0, sizeof(b));
	b.reservations[0][0] = 0x1000;
	b.reservations[0][1] = 0x2000;
	b.num_reservations = 1;
	begin(&b, "");
	prop(&b, "model", "Test board", 11);
	prop_u32(&b, "#address-cells", 1);
	begin(&b, "chosen");
	prop(&b, "bootargs", "quiet", 6);
	prop(&b, "empty", "", 0);
	end(&b);
	begin(&b, "memory@0");
	prop(&b, "device_type", "memory", 7);
	prop_u32(&b, "reg", 0);
	end(&b);
	begin(&b, "cpus");
	begin(&b, "cpu@0");
	prop_u32(&b, "reg", 0);
	end(&b);
	begin(&b, "cpu@1");
	prop_u32(&b, "reg", 1);
	end(&b);
	end(&b);
	begin(&b, "serial");
	end(&b);
	begin(&b, "serial@7e201000");
	end(&b);
	end(&b);
	size = finish(&b, &blob);
	CHECK_EQ(size, (size_t)load_be32(out + 4));
	CHECK(memcmp(blob, out, size) == 0);
	free(blob);
	free(out);
}

/* Changes, then a blob that reads back with them. */
static void test_edit(void)
{
	uint8_t *blob, *out;
	size_t size = sample(&blob), len, out_size;
	Fdt *fdt = parse(blob, size);
	FdtNode *chosen = fdt_find(fdt, "/chosen"),
			*memory = fdt_find(fdt, "/memory");
	FdtNode *root = fdt_find(fdt, "/"), *system;
	static const uint32_t reg[] = {0, 0x3b400000};
	const uint8_t *value;
	uint32_t cell;

	CHECK(fdt_set_string(chosen, "bootargs", "console=serial0,115200 quiet"));
	CHECK(fdt_set_cells(memory, "reg", reg, 2));
	CHECK(fdt_set(chosen, "linux,initrd-start", "\0\0\0\1", 4));
	CHECK(fdt_set_cells(chosen, "nothing", NULL, 0));
	fdt_remove(chosen, "empty");
	fdt_remove(chosen, "absent");
	CHECK(fdt_add_node(root, "chosen") == chosen); /* already there */
	system = fdt_add_node(root, "system");
	CHECK(system != NULL);
	CHECK(fdt_set_cells(system, "linux,revision", (uint32_t[]){0xa02082}, 1));
	CHECK(fdt_set_cells(system, "reg", (uint32_t[]){7}, 1));

	out_size = fdt_size(fdt);
	out = malloc(out_size);
	CHECK(out != NULL && fdt_write(fdt, out, out_size));
	fdt_free(fdt);
	free(blob);

	fdt = parse(out, out_size);
	chosen = fdt_find(fdt, "/chosen");
	value = fdt_get(chosen, "bootargs", &len);
	CHECK(value != NULL && len == 29);
	CHECK(memcmp(value, "console=serial0,115200 quiet", 29) == 0);
	CHECK(fdt_get(chosen, "empty", &len) == NULL);
	CHECK(fdt_get_u32(chosen, "linux,initrd-start", &cell));
	CHECK_EQ(cell, 1);
	CHECK(fdt_get(chosen, "nothing", &len) != NULL);
	CHECK_EQ(len, 0);
	value = fdt_get(fdt_find(fdt, "/memory@0"), "reg", &len);
	CHECK(value != NULL && len == 8);
	CHECK_EQ(load_be32(value), 0);
	CHECK_EQ(load_be32(value + 4), 0x3b400000);
	CHECK(fdt_get_u32(fdt_find(fdt, "/system"), "linux,revision", &cell));
	CHECK_EQ(cell, 0xa02082);
	CHECK(fdt_get_u32(fdt_find(fdt, "/system"), "reg", &cell));
	CHECK_EQ(cell, 7);
	CHECK(fdt_get_u32(fdt_find(fdt, "/cpus/cpu@1"), "reg", &cell));
	CHECK_EQ(cell, 1);
	/* The header: version 17, compatible with 16, the reservations kept. */
	CHECK_EQ(load_be32(out + 20), 17);
	CHECK_EQ(load_be32(out + 24), 16);
	CHECK_EQ(load_be32(out + 28), 2);
	CHECK_EQ(load_be32(out + 16), 40);
	CHECK_EQ(load_be64(out + 40), 0x1000);
	CHECK_EQ(load_be64(out + 48), 0x2000);
	CHECK_EQ(load_be64(out + 56), 0);
	CHECK_EQ(load_be64(out + 64), 0);
	CHECK_EQ(load_be32(out + 8), 72);
	/* The strings block holds "reg" once, though 4 nodes have it. */
	CHECK_EQ(load_be32(out + 12) + load_be32(out + 32), out_size);
	{
		const char *strings = (const char *)out + load_be32(out + 12);
		size_t n = load_be32(out + 32), count = 0;

		for (size_t off = 0; off < n; off += strlen(strings + off) + 1)
			count += strcmp(strings + off, "reg") == 0;
		CHECK_EQ(count, 1);
	}
	fdt_free(fdt);
	free(out);

	/* Removing the first and the last property of a node. */
	size = sample(&blob);
	fdt = parse(blob, size);
	chosen = fdt_find(fdt, "/chosen");
	fdt_remove(chosen, "bootargs");
	fdt_remove(chosen, "empty");
	CHECK(fdt_get(chosen, "bootargs", &len) == NULL);
	CHECK(fdt_set_string(chosen, "a", "b"));
	CHECK(fdt_set_string(chosen, "c", "d"));
	fdt_remove(chosen, "c");
	CHECK(fdt_set_string(chosen, "e", "f"));
	out_size = fdt_size(fdt);
	out = malloc(out_size);
	CHECK(out != NULL && fdt_write(fdt, out, out_size));
	fdt_free(fdt);
	fdt = parse(out, out_size);
	chosen = fdt_find(fdt, "/chosen");
	CHECK(fdt_get(chosen, "a", &len) != NULL && len == 2);
	CHECK(fdt_get(chosen, "c", &len) == NULL);
	CHECK(fdt_get(chosen, "e", &len) != NULL && len == 2);
	fdt_free(fdt);
	free(out);
	free(blob);
	fdt_free(NULL);
}

/* Version 16 has no size_dt_struct: the block ends with FDT_END. */
static void test_version_16(void)
{
	uint8_t *blob;
	size_t size = sample(&blob);
	Fdt *fdt;

	store_be32(blob + 20, 16);
	store_be32(blob + 36, 0);
	fdt = parse(blob, size);
	CHECK(fdt_find(fdt, "/cpus/cpu@1") != NULL);
	fdt_free(fdt);
	free(blob);
}

/* A broken blob is refused with a message. */
static void test_errors(void)
{
	static const struct {
		size_t offset; /* a word of the sample to change */
		uint32_t value;
		const char *message;
	} header[] = {
		{0, 0xd00dfeee, "not a devicetree blob"},
		{20, 15, "devicetree version 15, compatible with 16"},
		{24, 18, "compatible with 18, is not supported"},
		{4, 39, "the blob's total size, 39, is wrong"},
		{4, 0x10000, "the blob's total size, 65536, is wrong"},
		{36, 0x10000, "the structure block is outside"},
		{8, 0x10000, "the structure block is outside"},
		{8, 74, "a block of the blob is misplaced"},
		{16, 44, "a block of the blob is misplaced"},
		{32, 0x10000, "a block of the blob is misplaced"},
		{12, 0x10000, "a block of the blob is misplaced"},
	};
	uint8_t *blob;
	size_t size = sample(&blob);
	char error[128];

	for (unsigned i = 0; i < ARRAY_SIZE(header); i++) {
		uint8_t *copy = malloc(size);

		memcpy(copy, blob, size);
		store_be32(copy + header[i].offset, header[i].value);
		strcpy(error, "");
		CHECK(fdt_parse(copy, size, error, sizeof(error)) == NULL);
		CHECK(strstr(error, header[i].message) != NULL);
		free(copy);
	}
	CHECK(fdt_parse(blob, 39, error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "not a devicetree blob") == 0);
	CHECK(fdt_parse(blob, size - 1, error, sizeof(error)) == NULL);
	CHECK(strstr(error, "total size") != NULL);
	free(blob);
}

/* Structure blocks that break the rules of 5.4. */
static void test_structure_errors(void)
{
	static const struct {
		const char *what;
		const char *message;
	} cases[] = {
		{"second-root", "a second root node"},
		{"named-root", "the root node has a name"},
		{"prop-outside", "a property outside the nodes"},
		{"prop-after-child", "a property after a node's children"},
		{"end-without-begin", "the end of a node that did not begin"},
		{"open-node", "the structure block ends inside a node"},
		{"no-root", "no root node"},
		{"bad-token", "unknown token 0x7 at offset 56"},
		{"name-offset", "a property name is outside the strings block"},
		{"prop-too-long", "a property runs past the structure block"},
		{"prop-just-past", "a property runs past the structure block"},
		{"deep", "nodes nest deeper than 64"},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		static Builder b;
		const char *w = cases[i].what;
		uint8_t *blob;
		size_t size;
		char error[128] = "";

		memset(&b, 0, sizeof(b));
		if (strcmp(w, "second-root") == 0) {
			begin(&b, "");
			end(&b);
			begin(&b, "");
			end(&b);
		} else if (strcmp(w, "named-root") == 0) {
			begin(&b, "x");
			end(&b);
		} else if (strcmp(w, "prop-outside") == 0) {
			prop_u32(&b, "reg", 0);
		} else if (strcmp(w, "prop-after-child") == 0) {
			begin(&b, "");
			begin(&b, "a");
			end(&b);
			prop_u32(&b, "reg", 0);
			end(&b);
		} else if (strcmp(w, "end-without-begin") == 0) {
			end(&b);
		} else if (strcmp(w, "open-node") == 0) {
			begin(&b, "");
			begin(&b, "a");
			end(&b);
		} else if (strcmp(w, "no-root") == 0) {
			word(&b, 4);
		} else if (strcmp(w, "bad-token") == 0) {
			word(&b, 7);
		} else if (strcmp(w, "name-offset") == 0) {
			begin(&b, "");
			word(&b, 3);
			word(&b, 0);
			word(&b, 100);
			end(&b);
		} else if (strcmp(w, "prop-too-long") == 0) {
			begin(&b, "");
			word(&b, 3);
			word(&b, 1000);
			word(&b, 0);
			b.strings[0] = 'x';
			b.strings_len = 2;
		} else if (strcmp(w, "prop-just-past") == 0) {
			begin(&b, "");
			word(&b, 3);
			word(&b, 16); /* 4 bytes more than the block holds */
			word(&b, 0);
			word(&b, 0);
			word(&b, 9); /* FDT_END */
			b.strings[0] = 'x';
			b.strings_len = 2;
		} else {
			for (unsigned d = 0; d < 65; d++)
				begin(&b, d ? "n" : "");
		}
		size = finish(&b, &blob);
		CHECK(fdt_parse(blob, size, error, sizeof(error)) == NULL);
		if (strstr(error, cases[i].message) == NULL)
			fprintf(stderr, "%s: %s\n", w, error);
		CHECK(strstr(error, cases[i].message) != NULL);
		free(blob);
	}
}

/* Blocks cut short: strings, names and tokens that run off the end. */
static void test_truncated(void)
{
	static Builder b;
	uint8_t *blob;
	size_t size;
	char error[128];

	/* A property name without its terminating 0. */
	memset(&b, 0, sizeof(b));
	begin(&b, "");
	prop_u32(&b, "reg", 0);
	end(&b);
	size = finish(&b, &blob);
	store_be32(blob + 32, 3); /* the strings block loses its 0 */
	CHECK(fdt_parse(blob, size, error, sizeof(error)) == NULL);
	CHECK(strstr(error, "a property name at offset") != NULL);
	CHECK(strstr(error, "has no terminating 0") != NULL);
	free(blob);

	/* A node name that runs to the end of the structure block. */
	memset(&b, 0, sizeof(b));
	word(&b, 1);
	memcpy(b.structure + b.struct_len, "abcd", 4);
	b.struct_len += 4;
	size = finish(&b, &blob);
	store_be32(blob + 36, 8); /* without the FDT_END */
	CHECK(fdt_parse(blob, size, error, sizeof(error)) == NULL);
	CHECK(strstr(error, "a node name at offset 60 has no terminating 0"));
	free(blob);

	/* A structure block that ends inside a token. */
	memset(&b, 0, sizeof(b));
	begin(&b, "");
	end(&b);
	size = finish(&b, &blob);
	store_be32(blob + 36, 10);
	CHECK(fdt_parse(blob, size, error, sizeof(error)) == NULL);
	CHECK(strstr(error, "the structure block ends inside a token") != NULL);
	free(blob);

	/* A memory reservation block without its end. */
	memset(&b, 0, sizeof(b));
	begin(&b, "");
	end(&b);
	size = finish(&b, &blob);
	store_be32(blob + 16, (uint32_t)size - 8);
	CHECK(fdt_parse(blob, size, error, sizeof(error)) == NULL);
	CHECK(strstr(error, "the memory reservation block has no end") != NULL);
	store_be32(blob + 16, (uint32_t)size + 8);
	CHECK(fdt_parse(blob, size, error, sizeof(error)) == NULL);
	CHECK(strstr(error, "the memory reservation block has no end") != NULL);
	free(blob);
}

/* A root with property "a" of 4 bytes, then a child "c" with "a" too. */
static size_t tiny(uint8_t **blob)
{
	static Builder b;

	memset(&b, 0, sizeof(b));
	begin(&b, "");
	prop_u32(&b, "a", 1);
	begin(&b, "c");
	prop_u32(&b, "a", 2);
	end(&b);
	end(&b);
	return finish(&b, blob);
}

/* Parse blob, size bytes; NULL with the message in error. */
static Fdt *try_parse(const uint8_t *blob, size_t size, char *error)
{
	error[0] = '\0';
	return fdt_parse(blob, size, error, 128);
}

static void expect_error(const uint8_t *blob, size_t size, const char *message)
{
	char error[128];
	Fdt *fdt = try_parse(blob, size, error);

	CHECK(fdt == NULL);
	if (strstr(error, message) == NULL)
		fprintf(stderr, "expected \"%s\", got \"%s\"\n", message, error);
	CHECK(strstr(error, message) != NULL);
	fdt_free(fdt);
}

/* A copy of blob in a buffer of exactly size bytes, for ASan. */
static uint8_t *exact(const uint8_t *blob, size_t size)
{
	uint8_t *c = malloc(size);

	CHECK(c != NULL);
	memcpy(c, blob, size);
	return c;
}

/* The header's blocks at their limits. */
static void test_block_limits(void)
{
	uint8_t *blob, *c;
	size_t size = tiny(&blob);
	uint32_t total = (uint32_t)size, struct_off = load_be32(blob + 8);
	uint32_t struct_size = load_be32(blob + 36);
	uint32_t strings_off = load_be32(blob + 12);
	char error[128];
	Fdt *fdt;

	/* The structure block ends 1, 2 bytes into FDT_END; or right away. */
	for (uint32_t cut = 1; cut <= 2; cut++) {
		c = exact(blob, size);
		store_be32(c + 36, struct_size - cut);
		expect_error(c, size, "the structure block ends inside a token");
		free(c);
	}
	c = exact(blob, size);
	store_be32(c + 36, 0);
	expect_error(c, size, "the structure block ends inside a token");
	/* From offset 0, all of the blob: the magic is a token. */
	store_be32(c + 8, 0);
	store_be32(c + 36, total);
	expect_error(c, size, "unknown token 0xd00dfeed at offset 0");
	/* 4 bytes past the blob. */
	store_be32(c + 8, struct_off);
	store_be32(c + 36, total - struct_off + 4);
	expect_error(c, size, "the structure block is outside");
	free(c);

	/* The strings block: the whole blob, or 4 bytes past it. */
	c = exact(blob, size);
	store_be32(c + 12, 0);
	store_be32(c + 32, total);
	store_be32(c + struct_off + 4 * 4, strings_off);  /* "a" of the root */
	store_be32(c + struct_off + 4 * 10, strings_off); /* "a" of "c" */
	fdt = try_parse(c, size, error);
	CHECK(fdt != NULL);
	CHECK(fdt_get(fdt_find(fdt, "/c"), "a", &(size_t){0}) != NULL);
	fdt_free(fdt);
	store_be32(c + 12, strings_off);
	store_be32(c + 32, total - strings_off + 4);
	expect_error(c, size, "a block of the blob is misplaced");
	free(c);

	/* A last compatible version of 17 is fine. */
	c = exact(blob, size);
	store_be32(c + 24, 17);
	fdt = try_parse(c, size, error);
	CHECK(fdt != NULL);
	fdt_free(fdt);
	/* The header alone, its blocks empty at its end. */
	store_be32(c + 4, 40);
	store_be32(c + 8, 40);
	store_be32(c + 12, 40);
	store_be32(c + 16, 40);
	store_be32(c + 32, 0);
	store_be32(c + 36, 0);
	expect_error(c, 40, "the memory reservation block has no end");
	free(c);
	free(blob);
}

/* Properties at the end of the structure block, and their names. */
static void test_property_limits(void)
{
	uint8_t *blob, *c;
	size_t size = tiny(&blob);
	uint32_t struct_off = load_be32(blob + 8);
	uint32_t strings_size = load_be32(blob + 32);

	/* The root's value of 4 bytes ends where the block does. */
	c = exact(blob, size);
	store_be32(c + 36, 4 * 6);
	expect_error(c, size, "the structure block ends inside a token");
	/* A length past the block. */
	store_be32(c + 36, load_be32(blob + 36));
	store_be32(c + struct_off + 4 * 3, 0xffffff00);
	expect_error(c, size, "a property runs past the structure block");
	free(c);
	/* A name offset at the end of the strings block. */
	c = exact(blob, size);
	store_be32(c + struct_off + 4 * 4, strings_size);
	expect_error(c, size, "a property name is outside the strings block");
	free(c);
	free(blob);
}

/* Memory reservations: several, one at address 0, and the block last. */
static void test_reservations(void)
{
	static Builder b;
	uint8_t *blob, *out, *c;
	size_t size, padded;
	Fdt *fdt;
	char error[128];

	memset(&b, 0, sizeof(b));
	b.reservations[0][0] = 0x1000;
	b.reservations[0][1] = 0x2000;
	b.reservations[1][0] = 0;
	b.reservations[1][1] = 0x10;
	b.reservations[2][0] = 0x30000000;
	b.reservations[2][1] = 0x400;
	b.num_reservations = 3;
	begin(&b, "");
	end(&b);
	size = finish(&b, &blob);
	fdt = try_parse(blob, size, error);
	CHECK(fdt != NULL);
	CHECK_EQ(fdt_size(fdt), size);
	out = malloc(size);
	CHECK(fdt_write(fdt, out, size));
	CHECK(memcmp(out, blob, size) == 0);
	fdt_free(fdt);
	free(out);
	free(blob);

	/* The block moved to the end, its end entry the last 16 bytes. */
	size = tiny(&blob);
	padded = (size + 7) & ~(size_t)7;
	c = calloc(1, padded + 16);
	memcpy(c, blob, size);
	store_be32(c + 16, (uint32_t)padded);
	store_be32(c + 4, (uint32_t)(padded + 16));
	fdt = try_parse(c, padded + 16, error);
	CHECK(fdt != NULL);
	fdt_free(fdt);
	/* One byte short. */
	store_be32(c + 4, (uint32_t)(padded + 15));
	out = exact(c, padded + 15);
	expect_error(out, padded + 15, "the memory reservation block has no end");
	free(out);
	free(c);
	free(blob);
}

/* Lookups: unit addresses, and paths below a missing node. */
static void test_lookup_edges(void)
{
	static Builder b;
	uint8_t *blob;
	size_t size;
	Fdt *fdt;
	char error[128];

	memset(&b, 0, sizeof(b));
	begin(&b, "");
	begin(&b, "a@b@c");
	end(&b);
	end(&b);
	size = finish(&b, &blob);
	fdt = try_parse(blob, size, error);
	CHECK(fdt != NULL);
	CHECK(fdt_find(fdt, "/a@b@c") != NULL);
	CHECK(fdt_find(fdt, "/a@b") == NULL);
	CHECK(fdt_find(fdt, "/nope/x") == NULL);
	fdt_free(fdt);
	free(blob);
}

/* The strings block holds each name once; many names make it grow. */
static void test_names(void)
{
	uint8_t *blob, *out;
	size_t size = tiny(&blob);
	char error[128];
	Fdt *fdt = try_parse(blob, size, error), *back;

	CHECK(fdt != NULL);
	CHECK_EQ(fdt_size(fdt), size); /* "a" once */
	out = malloc(size);
	CHECK(fdt_write(fdt, out, size));
	CHECK_EQ(load_be32(out + 32), 2);
	free(out);
	for (unsigned i = 0; i < 70; i++) {
		char name[8];

		snprintf(name, sizeof(name), "p%u", i);
		CHECK(fdt_set_cells(fdt_find(fdt, "/c"), name, &i, 1));
	}
	size = fdt_size(fdt);
	out = malloc(size);
	CHECK(fdt_write(fdt, out, size));
	back = try_parse(out, size, error);
	CHECK(back != NULL);
	for (unsigned i = 0; i < 70; i++) {
		char name[8];
		uint32_t v = 99;

		snprintf(name, sizeof(name), "p%u", i);
		CHECK(fdt_get_u32(fdt_find(back, "/c"), name, &v));
		CHECK_EQ(v, i);
	}
	fdt_free(back);
	fdt_free(fdt);
	free(out);
	free(blob);
}

int main(void)
{
	RUN_TEST(test_find);
	RUN_TEST(test_round_trip);
	RUN_TEST(test_edit);
	RUN_TEST(test_version_16);
	RUN_TEST(test_errors);
	RUN_TEST(test_structure_errors);
	RUN_TEST(test_truncated);
	RUN_TEST(test_block_limits);
	RUN_TEST(test_property_limits);
	RUN_TEST(test_reservations);
	RUN_TEST(test_lookup_edges);
	RUN_TEST(test_names);
	return test_summary();
}
