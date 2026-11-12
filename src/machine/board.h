/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Boards: the machine types that -M selects. A board builds a Machine
 * with its CPUs, memory and devices, and boots it.
 */

#ifndef MACHINE_BOARD_H
#define MACHINE_BOARD_H

#include <stddef.h>

#include "core/char_stream.h"
#include "core/machine.h"

/* What the command line asks of a board. */
typedef struct BoardOptions {
	const char *kernel; /* -kernel: image to boot, or NULL */

	/*
	 * -serial: the back end of the serial console, or NULL for none. The
	 * board owns it from the call to create() on, even if create() fails.
	 */
	CharStream *serial;
} BoardOptions;

typedef struct Board {
	const char *name;        /* for -M */
	const char *description; /* for -M help */

	/*
	 * Build and boot the machine. On failure, writes a message to error
	 * and returns NULL.
	 */
	Machine *(*create)(const BoardOptions *options, char *error,
	                   size_t error_size);
} Board;

/* Every board, in the order -M help lists them, then NULL. */
extern const Board *const board_list[];

/* The board called name, or NULL if there is none. */
const Board *board_find(const char *name);

#endif /* MACHINE_BOARD_H */
