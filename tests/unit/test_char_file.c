/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for file character streams (src/backend/char/file.c). */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "backend/char/file.h"
#include "test.h"

static void test_writes_reach_the_file(void)
{
	FILE *f = tmpfile();
	char back[16] = {0};

	CHECK(f != NULL);

	CharStream *s = char_file_create(f, false);

	CHECK(s != NULL);
	char_stream_write(s, (const uint8_t *)"Hel", 3);
	char_stream_write(s, (const uint8_t *)"lo", 2);
	char_stream_destroy(s);

	/* The stream did not own the file, so it is still open. */
	rewind(f);
	CHECK_EQ(fread(back, 1, sizeof(back) - 1, f), 5);
	CHECK(strcmp(back, "Hello") == 0);
	fclose(f);
}

static void test_owned_file_is_closed(void)
{
	FILE *f = tmpfile();

	CHECK(f != NULL);

	/* AddressSanitizer reports a leak if the file is not closed. */
	CharStream *s = char_file_create(f, true);

	char_stream_write(s, (const uint8_t *)"x", 1);
	char_stream_destroy(s);
}

static void test_null_stream(void)
{
	/* Not connected: writes are dropped and destroy does nothing. */
	char_stream_write(NULL, (const uint8_t *)"x", 1);
	char_stream_destroy(NULL);
}

int main(void)
{
	RUN_TEST(test_writes_reach_the_file);
	RUN_TEST(test_owned_file_is_closed);
	RUN_TEST(test_null_stream);
	return test_summary();
}
