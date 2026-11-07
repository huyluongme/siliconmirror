/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for interrupt lines (src/core/irq.c). */

#include <stdbool.h>
#include <stddef.h>

#include "core/irq.h"
#include "test.h"

/* Records the calls an interrupt controller would receive. */
typedef struct FakeController {
	unsigned calls;
	unsigned last_n;
	bool last_level;
} FakeController;

static void fake_handler(void *ctx, unsigned n, bool level)
{
	FakeController *ctrl = ctx;

	ctrl->calls++;
	ctrl->last_n = n;
	ctrl->last_level = level;
}

static void test_starts_low(void)
{
	FakeController ctrl = {0};
	IrqLine line;

	irq_init(&line, fake_handler, &ctrl, 3);
	CHECK(!irq_level(&line));
	CHECK_EQ(ctrl.calls, 0);
}

static void test_handler_runs_on_change_only(void)
{
	FakeController ctrl = {0};
	IrqLine line;

	irq_init(&line, fake_handler, &ctrl, 3);

	irq_set(&line, true);
	CHECK_EQ(ctrl.calls, 1);
	CHECK_EQ(ctrl.last_n, 3);
	CHECK(ctrl.last_level);
	CHECK(irq_level(&line));

	/* Same level again: nothing changes, so no call. */
	irq_set(&line, true);
	CHECK_EQ(ctrl.calls, 1);

	irq_set(&line, false);
	CHECK_EQ(ctrl.calls, 2);
	CHECK(!ctrl.last_level);
	CHECK(!irq_level(&line));
}

static void test_raise_and_lower(void)
{
	FakeController ctrl = {0};
	IrqLine line;

	irq_init(&line, fake_handler, &ctrl, 0);
	irq_raise(&line);
	CHECK(irq_level(&line));
	irq_lower(&line);
	CHECK(!irq_level(&line));
	CHECK_EQ(ctrl.calls, 2);
}

static void test_unconnected_line(void)
{
	/* A device output that is not connected: nothing happens. */
	irq_set(NULL, true);
	irq_raise(NULL);
	CHECK(!irq_level(NULL));
}

static void test_line_without_handler(void)
{
	IrqLine line;

	irq_init(&line, NULL, NULL, 0);
	irq_raise(&line);
	CHECK(irq_level(&line));
}

int main(void)
{
	RUN_TEST(test_starts_low);
	RUN_TEST(test_handler_runs_on_change_only);
	RUN_TEST(test_raise_and_lower);
	RUN_TEST(test_unconnected_line);
	RUN_TEST(test_line_without_handler);
	return test_summary();
}
