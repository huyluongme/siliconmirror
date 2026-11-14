/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for the host's clock (src/backend/clock/host.c). */

#include <stdint.h>

#include "backend/clock/host.h"
#include "test.h"

/* Nanoseconds that a sleep of ns takes on the clock. */
static uint64_t slept(uint64_t ns)
{
	uint64_t start = host_clock.now(host_clock.ctx);

	host_clock.sleep(host_clock.ctx, ns);
	return host_clock.now(host_clock.ctx) - start;
}

/* Sleeps take their time, and the clock counts it in nanoseconds. */
static void test_sleep(void)
{
	uint64_t t = slept(20000000); /* 20 ms */

	CHECK(t >= 20000000);
	CHECK(t < 1000000000);
	t = slept(1200000000); /* 1.2 s: across a second of the clock */
	CHECK(t >= 1200000000);
	CHECK(t < 3000000000u);
}

int main(void)
{
	RUN_TEST(test_sleep);
	return test_summary();
}
