/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * gzip (RFC 1952) and DEFLATE (RFC 1951) decompression: the firmware
 * loads a kernel8.img that is a gzip file.
 *
 * DEFLATE data is a series of blocks: stored as they are, or Huffman
 * coded with the fixed codes of section 3.2.6 or codes that the block
 * describes first (3.2.7). A Huffman code is canonical (3.2.2): its
 * lengths alone give it. Codes of the same length are consecutive
 * numbers, in the order of their symbols, and each length starts where
 * the shorter ones left off, doubled. decode() reads a code bit by bit
 * and, at each length, checks whether it is one of that length.
 */

#include "util/inflate.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/crc32.h"

#define MAX_BITS 15      /* the longest code */
#define LITLEN_CODES 288 /* literal/length symbols, 286 and 287 unused */
#define DIST_CODES 32    /* distance symbols, 30 and 31 unused */
#define CODE_LENGTH_CODES 19

/* A canonical Huffman code. */
typedef struct Huffman {
	uint16_t count[MAX_BITS + 1];  /* how many codes of each length */
	uint16_t symbol[LITLEN_CODES]; /* the symbols, in code order */
} Huffman;

typedef struct Stream {
	const uint8_t *in;
	size_t in_size, in_pos;
	uint32_t bits;  /* bits read and not used, the next one lowest */
	unsigned nbits; /* how many */
	uint8_t *out;
	size_t out_size, out_pos;
	char *error;
	size_t error_size;
} Stream;

static bool fail(Stream *s, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(s->error, s->error_size, format, ap);
	va_end(ap);
	return false;
}

/*
 * Read n bits, n <= 16, the first one lowest (section 3.1.1). *value is
 * 0 when the data ends first.
 */
static bool get_bits(Stream *s, unsigned n, uint32_t *value)
{
	*value = 0;
	while (s->nbits < n) {
		if (s->in_pos == s->in_size)
			return fail(s, "its data ends too early");
		s->bits |= (uint32_t)s->in[s->in_pos++] << s->nbits;
		s->nbits += 8;
	}
	*value = s->bits & ((1u << n) - 1);
	s->bits >>= n;
	s->nbits -= n;
	return true;
}

/*
 * Make the code of lengths[0..n), 0 for an unused symbol. Returns how
 * many codes of the longest length are left unused: 0 for a complete
 * code, below 0 for one with too many codes.
 */
static int make_code(Huffman *h, const uint8_t *lengths, unsigned n)
{
	uint16_t next[MAX_BITS + 1];
	int left = 1;

	memset(h->count, 0, sizeof(h->count));
	for (unsigned i = 0; i < n; i++)
		h->count[lengths[i]]++;
	for (unsigned len = 1; len <= MAX_BITS; len++) {
		left = 2 * left - h->count[len];
		if (left < 0)
			return left;
	}
	next[1] = 0;
	for (unsigned len = 1; len < MAX_BITS; len++)
		next[len + 1] = next[len] + h->count[len];
	for (unsigned i = 0; i < n; i++)
		if (lengths[i] != 0)
			h->symbol[next[lengths[i]]++] = (uint16_t)i;
	return left;
}

/* Whether a code that is not complete is one code of 1 bit (3.2.7). */
static bool one_code(const Huffman *h)
{
	unsigned codes = 0;

	for (unsigned len = 1; len <= MAX_BITS; len++)
		codes += h->count[len];
	return codes == 1 && h->count[1] == 1;
}

/* Read one symbol of code h; 0 when it fails. */
static bool decode(Stream *s, const Huffman *h, unsigned *symbol)
{
	uint32_t code = 0, first = 0, index = 0;

	*symbol = 0;

	for (unsigned len = 1; len <= MAX_BITS; len++) {
		uint32_t bit;

		if (!get_bits(s, 1, &bit))
			return false;
		code |= bit;
		if (code - first < h->count[len]) {
			*symbol = h->symbol[index + code - first];
			return true;
		}
		index += h->count[len];
		first = (first + h->count[len]) << 1;
		code <<= 1;
	}
	return fail(s, "its data has a code that its block does not have");
}

/* Lengths and distances: a base, and how many extra bits (3.2.5). */
static const uint16_t length_base[29] = {
	3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
	31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                         1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                         4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[30] = {
	1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
	33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
	1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
	6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* The symbols of a Huffman block, up to its end (3.2.5). */
static bool codes(Stream *s, const Huffman *litlen, const Huffman *dist)
{
	for (;;) {
		unsigned symbol, length, distance;
		uint32_t extra;

		if (!decode(s, litlen, &symbol))
			return false;
		if (symbol < 256) {
			if (s->out_pos == s->out_size)
				return fail(s,
				            "it holds more than the %zu bytes its trailer says",
				            s->out_size);
			s->out[s->out_pos++] = (uint8_t)symbol;
			continue;
		}
		if (symbol == 256)
			return true;
		symbol -= 257;
		if (symbol >= 29)
			return fail(s, "its data has length code %u", symbol + 257);
		if (!get_bits(s, length_extra[symbol], &extra))
			return false;
		length = length_base[symbol] + extra;
		if (!decode(s, dist, &symbol))
			return false;
		if (symbol >= 30)
			return fail(s, "its data has distance code %u", symbol);
		if (!get_bits(s, distance_extra[symbol], &extra))
			return false;
		distance = distance_base[symbol] + extra;
		if (distance > s->out_pos)
			return fail(s, "its data refers %u bytes back, before its start",
			            distance);
		if (length > s->out_size - s->out_pos)
			return fail(s, "it holds more than the %zu bytes its trailer says",
			            s->out_size);
		/* The copy may overlap what it writes: one byte at a time. */
		for (unsigned i = 0; i < length; i++, s->out_pos++)
			s->out[s->out_pos] = s->out[s->out_pos - distance];
	}
}

/* A stored block (3.2.4): to a byte boundary, LEN, NLEN, LEN bytes. */
static bool stored(Stream *s)
{
	unsigned len;

	s->bits = 0; /* the rest of the current byte */
	s->nbits = 0;
	if (s->in_size - s->in_pos < 4)
		return fail(s, "its data ends too early");
	len = s->in[s->in_pos] | (unsigned)s->in[s->in_pos + 1] << 8;
	if ((s->in[s->in_pos + 2] | (unsigned)s->in[s->in_pos + 3] << 8) !=
	    (~len & 0xffff))
		return fail(s, "a stored block's length is not checked by NLEN");
	s->in_pos += 4;
	if (s->in_size - s->in_pos < len)
		return fail(s, "its data ends too early");
	if (len > s->out_size - s->out_pos)
		return fail(s, "it holds more than the %zu bytes its trailer says",
		            s->out_size);
	memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
	s->in_pos += len;
	s->out_pos += len;
	return true;
}

/* A block with the fixed codes (3.2.6). */
static bool fixed(Stream *s)
{
	static Huffman litlen, dist;
	static bool made;

	if (!made) {
		uint8_t lengths[LITLEN_CODES];
		unsigned i;

		for (i = 0; i < 144; i++)
			lengths[i] = 8;
		for (; i < 256; i++)
			lengths[i] = 9;
		for (; i < 280; i++)
			lengths[i] = 7;
		for (; i < LITLEN_CODES; i++)
			lengths[i] = 8;
		make_code(&litlen, lengths, LITLEN_CODES);
		memset(lengths, 5, DIST_CODES);
		make_code(&dist, lengths, DIST_CODES);
		made = true;
	}
	return codes(s, &litlen, &dist);
}

/* A block with its own codes (3.2.7). */
static bool dynamic(Stream *s)
{
	static const uint8_t order[CODE_LENGTH_CODES] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	uint8_t lengths[LITLEN_CODES + DIST_CODES];
	Huffman lencode, litlen, dist;
	uint32_t hlit, hdist, hclen, value;
	unsigned i, n;
	int left;

	if (!get_bits(s, 5, &hlit) || !get_bits(s, 5, &hdist) ||
	    !get_bits(s, 4, &hclen))
		return false;
	hlit += 257;
	hdist += 1;
	hclen += 4;
	if (hlit > 286 || hdist > 30)
		return fail(s, "a block has %u literal/length and %u distance codes",
		            hlit, hdist);
	memset(lengths, 0, CODE_LENGTH_CODES);
	for (i = 0; i < hclen; i++) {
		if (!get_bits(s, 3, &value))
			return false;
		lengths[order[i]] = (uint8_t)value;
	}
	if (make_code(&lencode, lengths, CODE_LENGTH_CODES) != 0)
		return fail(s, "a block's code length code is not complete");

	n = hlit + hdist;
	for (i = 0; i < n;) {
		unsigned symbol, repeat, length = 0;

		if (!decode(s, &lencode, &symbol))
			return false;
		if (symbol < 16) {
			lengths[i++] = (uint8_t)symbol;
			continue;
		}
		if (symbol == 16) {
			if (i == 0)
				return fail(s, "a block repeats a code length before the "
				               "first");
			length = lengths[i - 1];
			if (!get_bits(s, 2, &value))
				return false;
			repeat = 3 + value;
		} else if (symbol == 17) {
			if (!get_bits(s, 3, &value))
				return false;
			repeat = 3 + value;
		} else {
			if (!get_bits(s, 7, &value))
				return false;
			repeat = 11 + value;
		}
		if (repeat > n - i)
			return fail(s, "a block repeats code lengths past its last code");
		memset(lengths + i, (int)length, repeat);
		i += repeat;
	}
	if (lengths[256] == 0)
		return fail(s, "a block has no end-of-block code");

	left = make_code(&litlen, lengths, hlit);
	if (left < 0 || (left > 0 && !one_code(&litlen)))
		return fail(s, "a block's literal/length code is not complete");
	left = make_code(&dist, lengths + hlit, hdist);
	if (left < 0 || (left > 0 && dist.count[0] != hdist && !one_code(&dist)))
		return fail(s, "a block's distance code is not complete");
	return codes(s, &litlen, &dist);
}

/* The DEFLATE data, up to the end of its last block. */
static bool inflate(Stream *s)
{
	uint32_t last, type;

	do {
		if (!get_bits(s, 1, &last) || !get_bits(s, 2, &type))
			return false;
		switch (type) {
		case 0:
			if (!stored(s))
				return false;
			break;
		case 1:
			if (!fixed(s))
				return false;
			break;
		case 2:
			if (!dynamic(s))
				return false;
			break;
		default:
			return fail(s, "a block has type 3, which is reserved");
		}
	} while (!last);
	return true;
}

/* The flags of the gzip header (RFC 1952, 2.3.1). */
#define FHCRC 0x02
#define FEXTRA 0x04
#define FNAME 0x08
#define FCOMMENT 0x10
#define FRESERVED 0xe0

#define HEADER_SIZE 10
#define TRAILER_SIZE 8 /* CRC32 and ISIZE */

/* Skip a zero-terminated field of the header from *pos. */
static bool skip_string(const uint8_t *in, size_t end, size_t *pos)
{
	const uint8_t *nul = memchr(in + *pos, 0, end - *pos);

	if (nul == NULL)
		return false;
	*pos = (size_t)(nul - in) + 1;
	return true;
}

bool gunzip(const uint8_t *in, size_t size, size_t max, uint8_t **out,
            size_t *out_size, char *error, size_t error_size)
{
	Stream s = {.in = in, .error = error, .error_size = error_size};
	size_t pos = HEADER_SIZE, end;
	uint32_t isize, crc;
	uint8_t flags;

	*out = NULL;
	*out_size = 0;
	if (size < HEADER_SIZE + TRAILER_SIZE || in[0] != 0x1f || in[1] != 0x8b)
		return fail(&s, "it is not gzip");
	if (in[2] != 8)
		return fail(&s, "its compression method is %u, not 8 (DEFLATE)", in[2]);
	flags = in[3];
	if (flags & FRESERVED)
		return fail(&s, "its header has reserved flags set: 0x%02x", flags);
	end = size - TRAILER_SIZE;
	if (flags & FEXTRA) {
		size_t xlen;

		if (end - pos < 2)
			return fail(&s, "its header ends too early");
		xlen = in[pos] | in[pos + 1] << 8;
		pos += 2;
		if (end - pos < xlen)
			return fail(&s, "its header ends too early");
		pos += xlen;
	}
	if (((flags & FNAME) && !skip_string(in, end, &pos)) ||
	    ((flags & FCOMMENT) && !skip_string(in, end, &pos)))
		return fail(&s, "its header ends too early");
	if (flags & FHCRC) {
		if (end - pos < 2)
			return fail(&s, "its header ends too early");
		if ((in[pos] | (uint32_t)in[pos + 1] << 8) != (crc32(in, pos) & 0xffff))
			return fail(&s, "its header CRC is wrong");
		pos += 2;
	}

	crc = in[end] | in[end + 1] << 8 | in[end + 2] << 16 |
	      (uint32_t)in[end + 3] << 24;
	isize = in[end + 4] | in[end + 5] << 8 | in[end + 6] << 16 |
	        (uint32_t)in[end + 7] << 24;
	if (isize > max)
		return fail(&s, "it holds %" PRIu32 " bytes, more than %zu", isize,
		            max);
	s.out = malloc(isize > 0 ? isize : 1);
	if (s.out == NULL)
		return fail(&s, "out of memory for its %" PRIu32 " bytes", isize);
	s.out_size = isize;
	s.in_pos = pos;
	s.in_size = end;
	if (!inflate(&s))
		goto failed;
	if (s.in_pos != end) {
		fail(&s, "more data follows its member: %zu bytes", end - s.in_pos);
		goto failed;
	}
	if (s.out_pos != isize) {
		fail(&s, "it holds %zu bytes, but its trailer says %" PRIu32, s.out_pos,
		     isize);
		goto failed;
	}
	if (crc32(s.out, s.out_pos) != crc) {
		fail(&s, "its CRC is wrong");
		goto failed;
	}
	*out = s.out;
	*out_size = s.out_pos;
	return true;

failed:
	free(s.out);
	return false;
}
