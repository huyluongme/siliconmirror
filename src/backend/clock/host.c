/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#define _POSIX_C_SOURCE 200809L /* clock_gettime(), nanosleep() */

#include "backend/clock/host.h"

#include <time.h>

#define NS_PER_S 1000000000u

static uint64_t host_now(void *ctx)
{
	struct timespec t;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * NS_PER_S + (uint64_t)t.tv_nsec;
}

static void host_sleep(void *ctx, uint64_t ns)
{
	struct timespec t = {.tv_sec = (time_t)(ns / NS_PER_S),
	                     .tv_nsec = (long)(ns % NS_PER_S)};

	(void)ctx;
	nanosleep(&t, NULL);
}

const MachineHostClock host_clock = {.now = host_now, .sleep = host_sleep};
