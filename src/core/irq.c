/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "core/irq.h"

#include <stddef.h>

void irq_init(IrqLine *line, IrqHandler handler, void *ctx, unsigned n)
{
	line->handler = handler;
	line->ctx = ctx;
	line->n = n;
	line->level = false;
}

void irq_set(IrqLine *line, bool level)
{
	if (line == NULL || line->level == level)
		return;
	line->level = level;
	if (line->handler != NULL)
		line->handler(line->ctx, line->n, level);
}

bool irq_level(const IrqLine *line)
{
	return line != NULL && line->level;
}
