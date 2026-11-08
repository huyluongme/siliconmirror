/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The system counter: one 64-bit count that every CPU of the machine
 * sees, and that the timers inside the CPUs compare against (Arm ARM
 * D12.1.2 calls it the system counter of the Generic Timer).
 *
 * Some piece of the board's hardware provides it and decides how fast it
 * counts. The count is a function of virtual time, so it is computed when
 * read rather than stepped. Whoever depends on its rate, such as a timer
 * waiting for a value, registers a listener; the provider calls the
 * listeners after the rate changes or the count jumps.
 */

#ifndef CORE_COUNTER_H
#define CORE_COUNTER_H

#include <stdbool.h>
#include <stdint.h>

#define SYSTEM_COUNTER_MAX_LISTENERS 8

typedef struct SystemCounterOps {
	/* The count at virtual time now. */
	uint64_t (*read)(void *ctx, uint64_t now);

	/*
	 * The earliest virtual time, now or later, at which the count is at
	 * least count, if it keeps its current rate. EVENT_NEVER if it never
	 * gets there: it is stopped, or it would take past the end of time.
	 */
	uint64_t (*time_of)(void *ctx, uint64_t now, uint64_t count);
} SystemCounterOps;

/*
 * Called after the counter changed rate or jumped: before and after are
 * the counts just before and just after the change, equal when only the
 * rate changed.
 */
typedef void (*SystemCounterListener)(void *ctx, uint64_t before,
                                      uint64_t after);

typedef struct SystemCounter {
	const SystemCounterOps *ops;
	void *ctx;
	struct {
		SystemCounterListener fn;
		void *ctx;
	} listeners[SYSTEM_COUNTER_MAX_LISTENERS];
	unsigned num_listeners;
} SystemCounter;

/* Set up a counter with no listeners; ops and ctx provide the count. */
void system_counter_init(SystemCounter *sc, const SystemCounterOps *ops,
                         void *ctx);

/*
 * The count at virtual time now. A NULL counter, which a CPU with no
 * system counter connected has, always reads as 0.
 */
uint64_t system_counter_read(const SystemCounter *sc, uint64_t now);

/* See SystemCounterOps.time_of. A NULL counter never leaves 0. */
uint64_t system_counter_time_of(const SystemCounter *sc, uint64_t now,
                                uint64_t count);

/*
 * Add a listener. Returns false if there are already
 * SYSTEM_COUNTER_MAX_LISTENERS. Listening to a NULL counter does nothing
 * and succeeds.
 */
bool system_counter_listen(SystemCounter *sc, SystemCounterListener fn,
                           void *ctx);

/* For the provider: tell every listener about a change. */
void system_counter_changed(SystemCounter *sc, uint64_t before, uint64_t after);

#endif /* CORE_COUNTER_H */
