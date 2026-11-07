/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Character streams that write to a stdio FILE, such as the terminal. */

#ifndef BACKEND_CHAR_FILE_H
#define BACKEND_CHAR_FILE_H

#include <stdbool.h>
#include <stdio.h>

#include "core/char_stream.h"

/*
 * A stream that writes to file, flushing after every write so that output
 * shows up at once. If owns_file is true, destroying the stream closes the
 * file. Returns NULL when out of memory.
 */
CharStream *char_file_create(FILE *file, bool owns_file);

/* A stream that writes to standard output. */
CharStream *char_stdio_create(void);

#endif /* BACKEND_CHAR_FILE_H */
