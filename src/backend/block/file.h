/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Block devices backed by an image file on the host. */

#ifndef BACKEND_BLOCK_FILE_H
#define BACKEND_BLOCK_FILE_H

#include <stddef.h>

#include "core/block.h"

/*
 * Open the image at path for reading and writing: what the guest writes
 * goes to the file, as to a real card. The file is locked, so that two
 * emulators cannot use it at once. On failure, writes a message to error
 * and returns NULL.
 */
BlockDevice *block_file_open(const char *path, char *error, size_t error_size);

#endif /* BACKEND_BLOCK_FILE_H */
