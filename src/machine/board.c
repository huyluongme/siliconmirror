/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "machine/board.h"

#include <string.h>

#include "machine/raspi3b.h"

const Board *const board_list[] = {
	&raspi3b_board,
	NULL,
};

const Board *board_find(const char *name)
{
	for (const Board *const *b = board_list; *b != NULL; b++)
		if (strcmp((*b)->name, name) == 0)
			return *b;
	return NULL;
}
