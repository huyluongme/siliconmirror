/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "backend/char/file.h"

#include <stdlib.h>

#include "util/macros.h"

typedef struct CharFile {
	CharStream stream;
	FILE *file;
	bool owns_file;
} CharFile;

static CharFile *char_file_of(CharStream *s)
{
	return container_of(s, CharFile, stream);
}

static void char_file_write(CharStream *s, const uint8_t *buf, size_t len)
{
	CharFile *f = char_file_of(s);

	/* The guest cannot be told about host errors, so they are ignored. */
	fwrite(buf, 1, len, f->file);
	fflush(f->file);
}

static void char_file_destroy(CharStream *s)
{
	CharFile *f = char_file_of(s);

	if (f->owns_file)
		fclose(f->file);
	free(f);
}

static const CharStreamOps char_file_ops = {
	.write = char_file_write,
	.destroy = char_file_destroy,
};

CharStream *char_file_create(FILE *file, bool owns_file)
{
	CharFile *f = calloc(1, sizeof(*f));

	if (f == NULL)
		return NULL;
	f->stream.ops = &char_file_ops;
	f->file = file;
	f->owns_file = owns_file;
	return &f->stream;
}

CharStream *char_stdio_create(void)
{
	return char_file_create(stdout, false);
}
