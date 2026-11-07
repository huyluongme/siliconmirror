/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Interrupt lines.
 *
 * A line is the input side of a wire: it belongs to whatever receives the
 * interrupt (an interrupt controller or a CPU), which decides what a level
 * change means. Whatever sends the interrupt (a device) only keeps a
 * pointer to the line and calls irq_set(). The board connects the two by
 * setting that pointer.
 *
 * A device output that is not connected is a NULL pointer, and irq_set()
 * on NULL does nothing, so devices need no special case for it.
 */

#ifndef CORE_IRQ_H
#define CORE_IRQ_H

#include <stdbool.h>

/* Called when the level of input n changes. */
typedef void (*IrqHandler)(void *ctx, unsigned n, bool level);

typedef struct IrqLine {
	IrqHandler handler;
	void *ctx;
	unsigned n; /* input number passed to the handler */
	bool level; /* current level: true when asserted */
} IrqLine;

/* Set up an input line, initially low. */
void irq_init(IrqLine *line, IrqHandler handler, void *ctx, unsigned n);

/* Drive the line; the handler only runs when the level changes. */
void irq_set(IrqLine *line, bool level);

/* Current level of the line; false for NULL. */
bool irq_level(const IrqLine *line);

static inline void irq_raise(IrqLine *line)
{
	irq_set(line, true);
}

static inline void irq_lower(IrqLine *line)
{
	irq_set(line, false);
}

#endif /* CORE_IRQ_H */
