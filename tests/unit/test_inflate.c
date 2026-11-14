/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for src/util/inflate.c and src/util/crc32.c.
 *
 * Usage: test_inflate FILE FILE.gz FILE.gz... (each .gz is FILE, as the
 * host's gzip compressed it). The DEFLATE blocks that are not from zlib
 * or gzip are written bit by bit here, after RFC 1951.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "util/crc32.h"
#include "util/inflate.h"
#include "util/macros.h"

static char error[160];

/* Decompress in[0..size) and check that it holds want[0..want_size). */
static void check_gunzip(const uint8_t *in, size_t size, const void *want,
                         size_t want_size)
{
	uint8_t *out;
	size_t out_size;

	error[0] = '\0';
	CHECK(gunzip(in, size, 1u << 20, &out, &out_size, error, sizeof(error)));
	if (error[0] != '\0')
		fprintf(stderr, "    error: %s\n", error);
	CHECK_EQ(out_size, want_size);
	CHECK(out != NULL && memcmp(out, want, want_size) == 0);
	free(out);
}

/* Check that decompressing in[0..size) fails with message. */
static void check_fails(const uint8_t *in, size_t size, const char *message)
{
	uint8_t *out = (uint8_t *)1;
	size_t out_size = 1;

	error[0] = '\0';
	CHECK(!gunzip(in, size, 1u << 20, &out, &out_size, error, sizeof(error)));
	CHECK(out == NULL);
	CHECK_EQ(out_size, 0);
	if (message != NULL && strcmp(error, message) != 0) {
		fprintf(stderr, "    error: \"%s\", expected \"%s\"\n", error, message);
		CHECK(false);
	}
}

/* Bits written the way DEFLATE reads them: the first one lowest. */
typedef struct Bits {
	uint8_t data[1024];
	size_t pos; /* in bits */
} Bits;

static void put(Bits *w, uint32_t value, unsigned n)
{
	for (unsigned i = 0; i < n; i++, w->pos++)
		w->data[w->pos / 8] |= (uint8_t)((value >> i & 1) << (w->pos % 8));
}

/* A Huffman code: its first bit is its highest (3.1.1). */
static void put_code(Bits *w, uint32_t code, unsigned len)
{
	while (len-- > 0)
		put(w, code >> len, 1);
}

static size_t bytes(const Bits *w)
{
	return (w->pos + 7) / 8;
}

/* The codes that lengths give (RFC 1951, 3.2.2). */
static void canonical(const uint8_t *lengths, unsigned n, uint16_t *codes)
{
	unsigned count[16] = {0}, next[16] = {0}, code = 0;

	for (unsigned i = 0; i < n; i++)
		count[lengths[i]]++;
	count[0] = 0;
	for (unsigned bits = 1; bits < 16; bits++) {
		code = (code + count[bits - 1]) << 1;
		next[bits] = code;
	}
	for (unsigned i = 0; i < n; i++)
		if (lengths[i] != 0)
			codes[i] = (uint16_t)next[lengths[i]]++;
}

/* The fixed codes (3.2.6). */
static uint8_t fixed_len[288];
static uint16_t fixed_code[288];

static void make_fixed(void)
{
	for (unsigned i = 0; i < 288; i++)
		fixed_len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
	canonical(fixed_len, 288, fixed_code);
}

static void put_fixed(Bits *w, unsigned symbol)
{
	put_code(w, fixed_code[symbol], fixed_len[symbol]);
}

/*
 * A stored block of len bytes (3.2.4): to a byte boundary, with pad's
 * bits, which are ignored; LEN; NLEN.
 */
static void put_stored(Bits *w, bool last, uint32_t pad, const void *data,
                       unsigned len)
{
	const uint8_t *p = data;

	put(w, last, 1);
	put(w, 0, 2);
	put(w, pad, (unsigned)(8 - w->pos % 8) % 8);
	put(w, len, 16);
	put(w, ~len & 0xffff, 16);
	for (unsigned i = 0; i < len; i++)
		put(w, p[i], 8);
}

/* A gzip file around the DEFLATE data of w, which holds data[0..size). */
static size_t wrap(uint8_t *out, const Bits *w, const void *data, size_t size)
{
	static const uint8_t header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
	uint32_t crc = crc32(data, size);
	size_t n = bytes(w);

	memcpy(out, header, sizeof(header));
	memcpy(out + 10, w->data, n);
	for (int i = 0; i < 4; i++) {
		out[10 + n + i] = (uint8_t)(crc >> 8 * i);
		out[14 + n + i] = (uint8_t)(size >> 8 * i);
	}
	return n + 18;
}

/*
 * Every file of gz's header, a part of its DEFLATE data and its trailer:
 * the data ends too early, whatever block or code it ends in.
 */
static void check_cuts(const uint8_t *gz, size_t size)
{
	static uint8_t cut[2048];

	CHECK(size <= sizeof(cut));
	for (size_t i = 0; i + 18 < size; i++) {
		memcpy(cut, gz, 10 + i);
		memcpy(cut + 10 + i, gz + size - 8, 8);
		check_fails(cut, i + 18, "its data ends too early");
	}
}

/*
 * From Python's zlib: 400 bytes, (i * 7 + i / 13) % 41 + 48, in one block
 * with dynamic codes.
 */
static const uint8_t dynamic_gz[] = {
	0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x4d, 0xce,
	0x57, 0x12, 0x82, 0x30, 0x18, 0x45, 0xe1, 0x2d, 0x81, 0x1a, 0x8a, 0x58,
	0x28, 0x52, 0x0c, 0x60, 0xa1, 0xba, 0xff, 0x95, 0x48, 0x26, 0x4c, 0xfe,
	0xf3, 0x7c, 0x66, 0xee, 0xfd, 0xbc, 0xf0, 0x56, 0x76, 0xa3, 0x1f, 0xdd,
	0xab, 0x7e, 0x3a, 0x9c, 0xb3, 0xe6, 0xbd, 0x9c, 0x92, 0xfc, 0xf9, 0x59,
	0xd5, 0xe5, 0xd1, 0x0e, 0x9e, 0x8b, 0xaf, 0xf9, 0x28, 0xf1, 0x17, 0x5c,
	0x25, 0xc6, 0x69, 0x2d, 0xb1, 0xd0, 0x5f, 0x89, 0xdb, 0xa0, 0xc4, 0x6d,
	0x50, 0xa2, 0x7d, 0xdb, 0xa3, 0x7d, 0xdb, 0xa3, 0xa3, 0x98, 0xe8, 0x28,
	0x26, 0xd2, 0x19, 0xd3, 0x59, 0xd0, 0xd9, 0xd3, 0xb9, 0xd2, 0x19, 0xd1,
	0x99, 0xd3, 0xd9, 0xd1, 0xb9, 0xd0, 0x19, 0xd2, 0x99, 0xd1, 0xd9, 0xd2,
	0x39, 0xd3, 0x19, 0xd0, 0x99, 0xd2, 0xa9, 0xe9, 0x9c, 0xe8, 0x54, 0xe2,
	0xfc, 0x03, 0xaa, 0xb5, 0x9c, 0x2b, 0x90, 0x01, 0x00, 0x00};

/* The CRC of gzip, from its usual check values. */
static void test_crc32(void)
{
	CHECK_EQ(crc32("123456789", 9), 0xcbf43926u);
	CHECK_EQ(crc32("", 0), 0);
	CHECK_EQ(crc32("a", 1), 0xe8b7be43u);
}

/* From Python's zlib: fixed codes, a sync flush, dynamic codes. */
static void test_zlib(void)
{
	static const uint8_t empty[] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3,
	                                0x03, 0,    0, 0, 0, 0, 0, 0, 0, 0};
	static const uint8_t fixed[] = {
		0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
		0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0xb9,
		0x00, 0x00, 0x88, 0x59, 0x0b, 0x18, 0x00, 0x00, 0x00};
	static const uint8_t sync[] = {
		0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0a,
		0xae, 0xcc, 0x4b, 0x56, 0x48, 0xcb, 0x29, 0x2d, 0xce, 0xb0, 0x52,
		0xc8, 0xcf, 0x4b, 0x55, 0x48, 0xca, 0xc9, 0x4f, 0xce, 0xd6, 0x51,
		0x48, 0xcc, 0x53, 0x48, 0xcd, 0x2d, 0x28, 0x01, 0x00, 0x00, 0x00,
		0xff, 0xff, 0xab, 0x54, 0x28, 0x2e, 0xc9, 0x2f, 0x4a, 0x4d, 0x81,
		0x09, 0x97, 0x64, 0xa4, 0xe6, 0x81, 0x08, 0x85, 0x9c, 0xc4, 0xe2,
		0x12, 0x90, 0x7a, 0x3d, 0x85, 0x60, 0xb8, 0x01, 0x8a, 0x5c, 0x00,
		0xbe, 0xd0, 0x0a, 0x03, 0x4d, 0x00, 0x00, 0x00};
	static const char sync_text[] = "Sync flush: one block, an empty stored "
									"block, then the last one. Sync flush!\n";
	uint8_t text[400];

	check_gunzip(empty, sizeof(empty), "", 0);
	check_gunzip(fixed, sizeof(fixed), "hello hello hello hello\n", 24);
	check_gunzip(sync, sizeof(sync), sync_text, sizeof(sync_text) - 1);
	for (unsigned i = 0; i < sizeof(text); i++)
		text[i] = (uint8_t)((i * 7 + i / 13) % 41 + 48);
	check_gunzip(dynamic_gz, sizeof(dynamic_gz), text, sizeof(text));
	check_cuts(fixed, sizeof(fixed));
	check_cuts(sync, sizeof(sync));
	check_cuts(dynamic_gz, sizeof(dynamic_gz));
}

/* Stored blocks: the longest, after a Huffman block that ends mid-byte. */
static void test_stored(void)
{
	static uint8_t data[70003], gz[sizeof(data) + 64];
	static Bits w;
	size_t n = 10, len;

	make_fixed();
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 13 + i / 255);
	/* A fixed block with one literal, which ends in the middle of a byte. */
	memset(&w, 0, sizeof(w));
	put(&w, 0, 1);
	put(&w, 1, 2);
	put_fixed(&w, data[0]);
	put_fixed(&w, 256);
	/* The first stored block's 3 bits follow, in the same byte. */
	put(&w, 0, 3);
	memcpy(gz, (const uint8_t[]){0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3}, 10);
	memcpy(gz + n, w.data, bytes(&w));
	n += bytes(&w);
	/* 65535 bytes, the longest stored block, then the rest. */
	for (size_t at = 1; at < sizeof(data); at += len) {
		bool last = sizeof(data) - at <= 65535;

		len = last ? sizeof(data) - at : 65535;
		if (at > 1)
			gz[n++] = last;
		gz[n++] = (uint8_t)len;
		gz[n++] = (uint8_t)(len >> 8);
		gz[n++] = (uint8_t)~len;
		gz[n++] = (uint8_t)(~len >> 8);
		memcpy(gz + n, data + at, len);
		n += len;
	}
	for (int i = 0; i < 4; i++) {
		gz[n + i] = (uint8_t)(crc32(data, sizeof(data)) >> 8 * i);
		gz[n + 4 + i] = (uint8_t)(sizeof(data) >> 8 * i);
	}
	check_gunzip(gz, n + 8, data, sizeof(data));

	/* Stored blocks that are not the last, then a fixed block. */
	memset(&w, 0, sizeof(w));
	put_stored(&w, false, 0, "xy", 2);
	put_stored(&w, false, 0, "z", 1);
	put(&w, 1, 1);
	put(&w, 1, 2);
	put_fixed(&w, 'w');
	put_fixed(&w, 256);
	n = wrap(gz, &w, "xyzw", 4);
	check_gunzip(gz, n, "xyzw", 4);
	check_cuts(gz, n);

	/*
	 * The bits that pad a byte before a stored block are ignored, 1s
	 * too (3.2.4); the block after it starts afresh.
	 */
	memset(&w, 0, sizeof(w));
	put(&w, 0, 1);
	put(&w, 1, 2);
	put_fixed(&w, 'x');
	put_fixed(&w, 256);
	put_stored(&w, false, 0xff, "y", 1);
	put(&w, 1, 1);
	put(&w, 1, 2);
	put_fixed(&w, 'z');
	put_fixed(&w, 256);
	check_gunzip(gz, wrap(gz, &w, "xyz", 3), "xyz", 3);

	/* A stored block past what the trailer says, after a byte. */
	memset(&w, 0, sizeof(w));
	put(&w, 0, 1);
	put(&w, 1, 2);
	put_fixed(&w, 'a');
	put_fixed(&w, 256);
	put_stored(&w, true, 0, "aaa", 3);
	check_fails(gz, wrap(gz, &w, "aaaa", 3),
	            "it holds more than the 3 bytes its trailer says");

	/* An empty stored block alone. */
	memset(&w, 0, sizeof(w));
	put(&w, 1, 3);
	put(&w, 0, 5);
	put(&w, 0xffff0000u, 32);
	check_gunzip(gz, wrap(gz, &w, "", 0), "", 0);
}

/* Fixed codes, written here: lengths and distances with extra bits. */
static void test_fixed(void)
{
	uint8_t gz[200], want[9 + 258 + 257];
	Bits w = {0};

	make_fixed();
	/* "a", then 3 bytes 1 back: "aaaa". */
	put(&w, 1, 1);
	put(&w, 1, 2);
	put_fixed(&w, 'a');
	put_fixed(&w, 257);
	put_code(&w, 0, 5);
	put_fixed(&w, 256);
	check_gunzip(gz, wrap(gz, &w, "aaaa", 4), "aaaa", 4);
	check_cuts(gz, wrap(gz, &w, "aaaa", 4));

	/* Literals at the edges of the lengths: 0 and 143 of 8 bits, 144 and 255
	 * of 9. */
	memset(&w, 0, sizeof(w));
	put(&w, 1, 1);
	put(&w, 1, 2);
	put_fixed(&w, 0);
	put_fixed(&w, 143);
	put_fixed(&w, 144);
	put_fixed(&w, 255);
	put_fixed(&w, 256);
	check_gunzip(gz, wrap(gz, &w, "\x00\x8f\x90\xff", 4), "\x00\x8f\x90\xff",
	             4);

	/*
	 * "abcdef"; 3 bytes 6 back (distance code 4 and 1 extra bit); 258
	 * bytes 1 back (length code 285); 257 bytes 3 back (length code 284
	 * and 5 extra bits of 30).
	 */
	memset(&w, 0, sizeof(w));
	put(&w, 1, 1);
	put(&w, 1, 2);
	for (const char *p = "abcdef"; *p != '\0'; p++)
		put_fixed(&w, (unsigned char)*p);
	put_fixed(&w, 257);
	put_code(&w, 4, 5);
	put(&w, 1, 1);
	put_fixed(&w, 285);
	put_code(&w, 0, 5);
	put_fixed(&w, 284);
	put(&w, 30, 5);
	put_code(&w, 2, 5);
	put_fixed(&w, 256);
	memcpy(want, "abcdefabc", 9);
	memset(want + 9, 'c', 258 + 257);
	check_gunzip(gz, wrap(gz, &w, want, sizeof(want)), want, sizeof(want));
	check_cuts(gz, wrap(gz, &w, want, sizeof(want)));
}

/* A code length of a dynamic block: a symbol, and its extra bits. */
typedef struct Op {
	unsigned symbol, extra;
} Op;

/*
 * The code length code of the dynamic blocks here: 0 and 18 of 2 bits,
 * 1, 2 and 3 of 3 bits, 16 and 17 of 4 bits.
 */
static const uint8_t cl_lengths[19] = {
	[0] = 2, [1] = 3, [2] = 3, [3] = 3, [16] = 4, [17] = 4, [18] = 2};

/* A dynamic block's header (3.2.7), with the code lengths of ops. */
static void put_header(Bits *w, bool last, unsigned hlit, unsigned hdist,
                       unsigned hclen, const uint8_t cl[19], const Op *ops,
                       size_t n)
{
	static const uint8_t order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
	                                  11, 4,  12, 3, 13, 2, 14, 1, 15};
	uint16_t codes[19] = {0};

	put(w, last, 1);
	put(w, 2, 2);
	put(w, hlit - 257, 5);
	put(w, hdist - 1, 5);
	put(w, hclen - 4, 4);
	for (unsigned i = 0; i < hclen; i++)
		put(w, cl[order[i]], 3);
	canonical(cl, 19, codes);
	for (size_t i = 0; i < n; i++) {
		put_code(w, codes[ops[i].symbol], cl[ops[i].symbol]);
		if (ops[i].symbol >= 16)
			put(w, ops[i].extra,
			    ops[i].symbol == 16   ? 2
			    : ops[i].symbol == 17 ? 3
			                          : 7);
	}
}

/*
 * Literals a to d of 3 bits (98 to 100 by repeating 97's), 256 and 257
 * of 2 bits: 258 code lengths with 16, 17 and 18.
 */
static const Op abcd[] = {{18, 86},  {3, 0},  {16, 0}, {17, 7},
                          {18, 127}, {17, 4}, {2, 0},  {2, 0}};

/* Dynamic codes, written here: repeats, one distance code, edge cases. */
static void test_dynamic(void)
{
	Op ops[16];
	uint8_t gz[200];
	Bits w = {0};

	/* "abcd" with no distance code. */
	memcpy(ops, abcd, sizeof(abcd));
	ops[8] = (Op){0, 0};
	put_header(&w, true, 258, 1, 18, cl_lengths, ops, 9);
	put_code(&w, 4, 3);
	put_code(&w, 5, 3);
	put_code(&w, 6, 3);
	put_code(&w, 7, 3);
	put_code(&w, 0, 2);
	check_gunzip(gz, wrap(gz, &w, "abcd", 4), "abcd", 4);

	/* One distance code of 1 bit: "a", then 3 bytes 1 back. */
	memset(&w, 0, sizeof(w));
	ops[8] = (Op){1, 0};
	put_header(&w, true, 258, 1, 18, cl_lengths, ops, 9);
	put_code(&w, 4, 3);
	put_code(&w, 1, 2);
	put_code(&w, 0, 1);
	put_code(&w, 0, 2);
	check_gunzip(gz, wrap(gz, &w, "aaaa", 4), "aaaa", 4);

	/* No distance code, but a length: 15 bits find no code. */
	memset(&w, 0, sizeof(w));
	ops[8] = (Op){0, 0};
	put_header(&w, true, 258, 1, 18, cl_lengths, ops, 9);
	put_code(&w, 4, 3);
	put_code(&w, 1, 2);
	put(&w, 0, 16);
	check_fails(gz, wrap(gz, &w, "aaaa", 4),
	            "its data has a code that its block does not have");

	/* Only the end of block, one code of 1 bit: an empty block. */
	memset(&w, 0, sizeof(w));
	put_header(&w, true, 257, 1, 18, cl_lengths,
	           (const Op[]){{18, 127}, {18, 107}, {1, 0}, {0, 0}}, 4);
	put_code(&w, 0, 1);
	check_gunzip(gz, wrap(gz, &w, "", 0), "", 0);
}

/* A code length code for any length: 1 to 15 of 4 bits, 0 and 18 of 5. */
static const uint8_t deep_cl[19] = {5, 4, 4, 4, 4, 4, 4, 4, 4, 4,
                                    4, 4, 4, 4, 4, 4, 0, 0, 5};

/*
 * The literal/length code lengths of a code as deep as can be: a of 1
 * bit, b of 2, and so on to n of 14; o (if o) and 256 of 15. 286 codes.
 */
static size_t deep_ops(Op *ops, bool o, uint8_t lengths[286])
{
	size_t n = 0;

	memset(lengths, 0, 286);
	ops[n++] = (Op){18, 86}; /* 0 to 96 */
	for (unsigned i = 0; i < 15; i++) {
		lengths['a' + i] = i < 14 || o ? (uint8_t)(i + 1) : 0;
		ops[n++] = (Op){lengths['a' + i], 0};
	}
	for (int i = 0; i < 6; i++)
		ops[n++] = (Op){0, 0}; /* 112 to 117 */
	ops[n++] = (Op){18, 127};  /* 118 to 255 */
	ops[n++] = (Op){15, 0};    /* 256 */
	lengths[256] = 15;
	ops[n++] = (Op){18, 18}; /* 257 to 285 */
	return n;
}

/*
 * A code with 15-bit codes, all 19 code length codes, 286 literal/length
 * codes and 30 distance codes, the last of them by a repeat; then a
 * block with fewer, which must not see the first one's.
 */
static void test_deep(void)
{
	static const char want[] = "abcdefghijklmnoabcd";
	uint8_t gz[300], lengths[286];
	uint16_t codes[286];
	Op ops[40];
	Bits w = {0};
	size_t n = deep_ops(ops, true, lengths);

	ops[n++] = (Op){18, 19}; /* 30 distance codes, to the last exactly */
	put_header(&w, false, 286, 30, 19, deep_cl, ops, n);
	canonical(lengths, 286, codes);
	for (const char *p = "abcdefghijklmno"; *p != '\0'; p++)
		put_code(&w, codes[(unsigned char)*p], lengths[(unsigned char)*p]);
	put_code(&w, codes[256], 15);
	memcpy(ops, abcd, sizeof(abcd));
	ops[8] = (Op){0, 0};
	put_header(&w, true, 258, 1, 18, cl_lengths, ops, 9);
	put_code(&w, 4, 3);
	put_code(&w, 5, 3);
	put_code(&w, 6, 3);
	put_code(&w, 7, 3);
	put_code(&w, 0, 2);
	n = wrap(gz, &w, want, sizeof(want) - 1);
	check_gunzip(gz, n, want, sizeof(want) - 1);
	check_cuts(gz, n);
}

/* Codes that miss one code of 15 bits, and a code that is not there. */
static void test_deep_errors(void)
{
	uint8_t gz[300], lengths[286], aaaa[64];
	Op ops[60];
	Bits w = {0};
	size_t n;

	/* a to n and 256: one code of 15 bits is missing. */
	n = deep_ops(ops, false, lengths);
	ops[n++] = (Op){0, 0};
	put_header(&w, true, 286, 1, 19, deep_cl, ops, n);
	put(&w, 0, 16);
	check_fails(gz, wrap(gz, &w, "", 0),
	            "a block's literal/length code is not complete");

	/* Distance codes of 1 to 15 bits, one of each: the same. */
	memset(&w, 0, sizeof(w));
	n = deep_ops(ops, true, lengths);
	for (unsigned i = 1; i <= 15; i++)
		ops[n++] = (Op){i, 0};
	put_header(&w, true, 286, 15, 19, deep_cl, ops, n);
	put(&w, 0, 16);
	check_fails(gz, wrap(gz, &w, "", 0),
	            "a block's distance code is not complete");

	/* A distance code of 1 bit and one of 15: two codes, not one. */
	memset(&w, 0, sizeof(w));
	n = deep_ops(ops, true, lengths);
	ops[n++] = (Op){1, 0};
	ops[n++] = (Op){15, 0};
	put_header(&w, true, 286, 2, 19, deep_cl, ops, n);
	put(&w, 0, 16);
	check_fails(gz, wrap(gz, &w, "", 0),
	            "a block's distance code is not complete");

	/*
	 * a of 1 bit, 256 and 257 of 2, no distance code: after a length,
	 * 15 bits, the last of the data, find no distance code.
	 */
	memset(&w, 0, sizeof(w));
	put_header(
		&w, true, 258, 1, 19, deep_cl,
		(const Op[]){
			{18, 86}, {1, 0}, {18, 127}, {18, 9}, {2, 0}, {2, 0}, {0, 0}},
		7);
	while ((w.pos + 2 + 15) % 8 != 0)
		put_code(&w, 0, 1); /* a */
	put_code(&w, 3, 2);     /* 257 */
	put(&w, 0, 15);
	memset(aaaa, 'a', sizeof(aaaa));
	check_fails(gz, wrap(gz, &w, aaaa, sizeof(aaaa)),
	            "its data has a code that its block does not have");
}

/* Dynamic block headers that are wrong. */
static void test_dynamic_errors(void)
{
	const struct {
		unsigned hlit, hdist;
		const uint8_t *cl;
		Op ops[12];
		size_t n;
		const char *message;
	} cases[] = {
		{287,
	     1,
	     cl_lengths,
	     {{0}},
	     0,
	     "a block has 287 literal/length and 1 distance codes"},
		{257,
	     31,
	     cl_lengths,
	     {{0}},
	     0,
	     "a block has 257 literal/length and 31 distance codes"},
		{257,
	     1,
	     (const uint8_t[19]){[18] = 1},
	     {{0}},
	     0,
	     "a block's code length code is not complete"},
		{257,
	     1,
	     (const uint8_t[19]){[0] = 1, [1] = 1, [18] = 1},
	     {{0}},
	     0,
	     "a block's code length code is not complete"},
		{257,
	     1,
	     cl_lengths,
	     {{16, 0}},
	     1,
	     "a block repeats a code length before the first"},
		{257,
	     1,
	     cl_lengths,
	     {{18, 127}, {18, 127}},
	     2,
	     "a block repeats code lengths past its last code"},
		{257,
	     1,
	     cl_lengths,
	     {{3, 0}, {16, 3}, {18, 127}, {18, 127}},
	     4,
	     "a block repeats code lengths past its last code"},
		/* 256 has no code. */
		{257,
	     1,
	     cl_lengths,
	     {{18, 86}, {3, 0}, {16, 0}, {17, 7}, {18, 127}, {17, 5}, {0, 0}},
	     7,
	     "a block has no end-of-block code"},
		/* a to d and 256: 3/4 of the codes. */
		{257,
	     1,
	     cl_lengths,
	     {{18, 86},
	      {3, 0},
	      {16, 0},
	      {17, 7},
	      {18, 127},
	      {17, 4},
	      {2, 0},
	      {0, 0}},
	     8,
	     "a block's literal/length code is not complete"},
		/* a to d, 256 to 258: 5/4. */
		{259,
	     1,
	     cl_lengths,
	     {{18, 86},
	      {3, 0},
	      {16, 0},
	      {17, 7},
	      {18, 127},
	      {17, 4},
	      {2, 0},
	      {2, 0},
	      {2, 0},
	      {0, 0}},
	     10,
	     "a block's literal/length code is not complete"},
		/* One distance code of 2 bits. */
		{258,
	     2,
	     cl_lengths,
	     {{18, 86},
	      {3, 0},
	      {16, 0},
	      {17, 7},
	      {18, 127},
	      {17, 4},
	      {2, 0},
	      {2, 0},
	      {2, 0},
	      {0, 0}},
	     10,
	     "a block's distance code is not complete"},
		/* Three distance codes of 1 bit. */
		{258,
	     3,
	     cl_lengths,
	     {{18, 86},
	      {3, 0},
	      {16, 0},
	      {17, 7},
	      {18, 127},
	      {17, 4},
	      {2, 0},
	      {2, 0},
	      {1, 0},
	      {1, 0},
	      {1, 0}},
	     11,
	     "a block's distance code is not complete"},
	};
	uint8_t gz[200];

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		Bits w = {0};

		put_header(&w, true, cases[i].hlit, cases[i].hdist, 18, cases[i].cl,
		           cases[i].ops, cases[i].n);
		put(&w, 0, 16);
		check_fails(gz, wrap(gz, &w, "", 0), cases[i].message);
	}
}

/* gzip headers and trailers, and DEFLATE data that is wrong. */
static void test_errors(void)
{
	static const uint8_t aaaa[] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
	uint8_t gz[200], *out;
	size_t n, out_size;
	Bits w = {0};

	make_fixed();
	/* A good one to change: "aaaa" in a fixed block. */
	put(&w, 1, 1);
	put(&w, 1, 2);
	put_fixed(&w, 'a');
	put_fixed(&w, 257);
	put_code(&w, 0, 5);
	put_fixed(&w, 256);
	n = wrap(gz, &w, "aaaa", 4);
	check_gunzip(gz, n, "aaaa", 4);

	check_fails(gz, 17, "it is not gzip");
	gz[1] = 0x8c;
	check_fails(gz, n, "it is not gzip");
	gz[1] = 0x8b;
	gz[2] = 7;
	check_fails(gz, n, "its compression method is 7, not 8 (DEFLATE)");
	gz[2] = 8;
	gz[3] = 0x20;
	check_fails(gz, n, "its header has reserved flags set: 0x20");
	gz[3] = 0;
	gz[n - 8] ^= 1;
	check_fails(gz, n, "its CRC is wrong");
	gz[n - 8] ^= 1;
	gz[n - 4] = 5;
	check_fails(gz, n, "it holds 4 bytes, but its trailer says 5");
	gz[n - 4] = 3;
	check_fails(gz, n, "it holds more than the 3 bytes its trailer says");
	gz[n - 4] = 4;
	CHECK(!gunzip(gz, n, 3, &out, &out_size, error, sizeof(error)));
	CHECK(strcmp(error, "it holds 4 bytes, more than 3") == 0);
	CHECK(gunzip(gz, n, 4, &out, &out_size, error, sizeof(error)));
	free(out);
	/* A byte between the DEFLATE data and the trailer. */
	memmove(gz + n - 7, gz + n - 8, 8);
	gz[n - 8] = 0;
	check_fails(gz, n + 1, "more data follows its member: 1 bytes");

	/* Every field of the header: FEXTRA, FNAME, FCOMMENT and FHCRC. */
	memcpy(gz, aaaa, 10);
	gz[3] = 0x1e;
	memcpy(gz + 10,
	       "\x02\x00xy"
	       "kernel8.img\0"
	       "c\0",
	       18);
	n = 28;
	gz[n] = (uint8_t)crc32(gz, n);
	gz[n + 1] = (uint8_t)(crc32(gz, n) >> 8);
	n += 2;
	memcpy(gz + n, w.data, bytes(&w));
	n += bytes(&w);
	for (int i = 0; i < 4; i++) {
		gz[n + i] = (uint8_t)(crc32("aaaa", 4) >> 8 * i);
		gz[n + 4 + i] = i == 0 ? 4 : 0;
	}
	check_gunzip(gz, n + 8, "aaaa", 4);
	gz[28] ^= 1;
	check_fails(gz, n + 8, "its header CRC is wrong");
	/* Fields past the end of the header. */
	memcpy(gz, aaaa, 10);
	gz[3] = 0x04;
	memcpy(gz + 10, "\x09\x00xy", 4);
	check_fails(gz, 22, "its header ends too early");
	check_fails(gz, 19, "its header ends too early");
	gz[3] = 0x08;
	memcpy(gz + 10, "name", 4);
	check_fails(gz, 22, "its header ends too early");
	gz[3] = 0x10;
	check_fails(gz, 22, "its header ends too early");
	gz[3] = 0x02;
	check_fails(gz, 19, "its header ends too early");

	/* Block type 3. */
	memset(&w, 0, sizeof(w));
	put(&w, 1, 1);
	put(&w, 3, 2);
	check_fails(gz, wrap(gz, &w, "", 0),
	            "a block has type 3, which is reserved");

	/* Stored blocks: NLEN wrong, cut short, too long. */
	memset(&w, 0, sizeof(w));
	put(&w, 1, 3);
	put(&w, 0, 5);
	put(&w, 0x00000001, 32);
	check_fails(gz, wrap(gz, &w, "", 0),
	            "a stored block's length is not checked by NLEN");
	memset(&w, 0, sizeof(w));
	put(&w, 1, 3);
	put(&w, 0, 5);
	put(&w, 0, 8);
	check_fails(gz, wrap(gz, &w, "", 0), "its data ends too early");
	memset(&w, 0, sizeof(w));
	put(&w, 1, 3);
	put(&w, 0, 5);
	put(&w, 0xfffa0005, 32);
	put(&w, 'a', 8);
	check_fails(gz, wrap(gz, &w, "", 0), "its data ends too early");
	memset(&w, 0, sizeof(w));
	put(&w, 1, 3);
	put(&w, 0, 5);
	put(&w, 0xfffb0004, 32);
	put(&w, 0x61616161, 32);
	check_fails(gz, wrap(gz, &w, "aaa", 3),
	            "it holds more than the 3 bytes its trailer says");

	/* Fixed codes: length code 286, distance code 30, too far back. */
	memset(&w, 0, sizeof(w));
	put(&w, 3, 3); /* the last block, fixed codes */
	put_fixed(&w, 286);
	check_fails(gz, wrap(gz, &w, "", 0), "its data has length code 286");
	memset(&w, 0, sizeof(w));
	put(&w, 3, 3); /* the last block, fixed codes */
	put_fixed(&w, 287);
	check_fails(gz, wrap(gz, &w, "", 0), "its data has length code 287");
	memset(&w, 0, sizeof(w));
	put(&w, 3, 3); /* the last block, fixed codes */
	put_fixed(&w, 'a');
	put_fixed(&w, 257);
	put_code(&w, 30, 5);
	check_fails(gz, wrap(gz, &w, "aaaa", 4), "its data has distance code 30");
	memset(&w, 0, sizeof(w));
	put(&w, 3, 3); /* the last block, fixed codes */
	put_fixed(&w, 'a');
	put_fixed(&w, 257);
	put_code(&w, 31, 5);
	check_fails(gz, wrap(gz, &w, "aaaa", 4), "its data has distance code 31");
	memset(&w, 0, sizeof(w));
	put(&w, 3, 3); /* the last block, fixed codes */
	put_fixed(&w, 'a');
	put_fixed(&w, 257);
	put_code(&w, 1, 5);
	check_fails(gz, wrap(gz, &w, "aaaa", 4),
	            "its data refers 2 bytes back, before its start");
	/* A copy past what the trailer says. */
	memset(&w, 0, sizeof(w));
	put(&w, 3, 3); /* the last block, fixed codes */
	put_fixed(&w, 'a');
	put_fixed(&w, 257);
	put_code(&w, 0, 5);
	put_fixed(&w, 256);
	check_fails(gz, wrap(gz, &w, "aaa", 3),
	            "it holds more than the 3 bytes its trailer says");
}

/* A gzip file of "aaaa", with flags and the fields after the header. */
static size_t with_fields(uint8_t *gz, uint8_t flags, const void *fields,
                          size_t len, bool hcrc)
{
	static const uint8_t header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
	Bits w = {0};
	size_t n = 10 + len;
	uint32_t crc;

	make_fixed();
	put(&w, 3, 3);
	put_fixed(&w, 'a');
	put_fixed(&w, 257);
	put_code(&w, 0, 5);
	put_fixed(&w, 256);
	memcpy(gz, header, sizeof(header));
	gz[3] = flags | (hcrc ? 0x02 : 0);
	memcpy(gz + 10, fields, len);
	if (hcrc) {
		crc = crc32(gz, n);
		gz[n++] = (uint8_t)crc;
		gz[n++] = (uint8_t)(crc >> 8);
	}
	memcpy(gz + n, w.data, bytes(&w));
	n += bytes(&w);
	crc = crc32("aaaa", 4);
	for (int i = 0; i < 4; i++) {
		gz[n + i] = (uint8_t)(crc >> 8 * i);
		gz[n + 4 + i] = i == 0 ? 4 : 0;
	}
	return n + 8;
}

/* Each field of the header alone, and fields that end where the data starts. */
static void test_header(void)
{
	static uint8_t extra[2 + 258], gz[400];
	size_t n;

	n = with_fields(gz, 0x01, "", 0, false); /* FTEXT, a hint */
	check_gunzip(gz, n, "aaaa", 4);
	extra[0] = 0x02; /* XLEN 258 */
	extra[1] = 0x01;
	for (int i = 0; i < 258; i++)
		extra[2 + i] = (uint8_t)(i + 1);
	n = with_fields(gz, 0x04, extra, sizeof(extra), false);
	check_gunzip(gz, n, "aaaa", 4);
	n = with_fields(gz, 0x08, "kernel8.img", 12, false);
	check_gunzip(gz, n, "aaaa", 4);
	n = with_fields(gz, 0x10, "a comment", 10, false);
	check_gunzip(gz, n, "aaaa", 4);
	n = with_fields(gz, 0, "", 0, true);
	CHECK(gz[10] != gz[11]); /* the CRC's two bytes tell apart */
	check_gunzip(gz, n, "aaaa", 4);

	/* No data after: the header is whole, the data ends too early. */
	n = with_fields(gz, 0, "", 0, false);
	memmove(gz + 10, gz + n - 8, 8);
	check_fails(gz, 18, "its data ends too early");
	n = with_fields(gz, 0x04, "\0\0", 2, false);
	memmove(gz + 12, gz + n - 8, 8);
	check_fails(gz, 20, "its data ends too early");
	n = with_fields(gz, 0x04, "\3\0xyz", 5, false);
	memmove(gz + 15, gz + n - 8, 8);
	check_fails(gz, 23, "its data ends too early");
	n = with_fields(gz, 0, "", 0, true);
	memmove(gz + 12, gz + n - 8, 8);
	check_fails(gz, 20, "its data ends too early");

	/* A size of more than 16 MB in the trailer. */
	n = with_fields(gz, 0, "", 0, false);
	gz[n - 1] = 1;
	check_fails(gz, n, "it holds 16777220 bytes, more than 1048576");
}

/* Any file cut short fails; so does any changed bit of it but MTIME, XFL and
 * OS. */
static void test_damage(void)
{
	static uint8_t gz[sizeof(dynamic_gz)];

	for (size_t n = 0; n < sizeof(dynamic_gz); n++)
		check_fails(dynamic_gz, n, NULL);
	for (size_t i = 0; i < sizeof(gz); i++) {
		uint8_t *out;
		size_t out_size;
		bool ok;

		memcpy(gz, dynamic_gz, sizeof(gz));
		gz[i] ^= i == 3 ? 2 : 1;
		ok = gunzip(gz, sizeof(gz), 1u << 20, &out, &out_size, error,
		            sizeof(error));
		if (ok != (i >= 4 && i < 10))
			fprintf(stderr, "    byte %zu changed: %s\n", i,
			        ok ? "decompressed" : error);
		CHECK_EQ(ok, i >= 4 && i < 10);
		if (ok)
			free(out);
	}
}

static int file_args;
static char **file_argv;

static uint8_t *read_all(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *data = NULL;
	long n;

	if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0 || (data = malloc((size_t)n + 1)) == NULL ||
	    fread(data, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "    cannot read %s\n", path);
		exit(1);
	}
	fclose(f);
	*size = (size_t)n;
	return data;
}

/* Files that the host's gzip compressed decompress to the original. */
static void test_files(void)
{
	size_t size, gz_size, out_size;
	uint8_t *data, *gz, *out;

	CHECK(file_args >= 2);
	if (file_args < 2)
		return;
	data = read_all(file_argv[0], &size);
	for (int i = 1; i < file_args; i++) {
		gz = read_all(file_argv[i], &gz_size);
		CHECK(gunzip(gz, gz_size, size, &out, &out_size, error, sizeof(error)));
		CHECK_EQ(out_size, size);
		CHECK(memcmp(out, data, size) == 0);
		free(out);
		free(gz);
	}
	free(data);
}

int main(int argc, char **argv)
{
	file_args = argc - 1;
	file_argv = argv + 1;
	RUN_TEST(test_crc32);
	RUN_TEST(test_zlib);
	RUN_TEST(test_stored);
	RUN_TEST(test_fixed);
	RUN_TEST(test_dynamic);
	RUN_TEST(test_deep);
	RUN_TEST(test_deep_errors);
	RUN_TEST(test_dynamic_errors);
	RUN_TEST(test_errors);
	RUN_TEST(test_header);
	RUN_TEST(test_damage);
	RUN_TEST(test_files);
	return test_summary();
}
