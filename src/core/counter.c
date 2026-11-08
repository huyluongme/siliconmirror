/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "core/counter.h"

#include <stddef.h>

#include "core/event.h"

void system_counter_init(SystemCounter *sc, const SystemCounterOps *ops,
                         void *ctx)
{
	sc->ops = ops;
	sc->ctx = ctx;
	sc->num_listeners = 0;
}

uint64_t system_counter_read(const SystemCounter *sc, uint64_t now)
{
	return sc == NULL ? 0 : sc->ops->read(sc->ctx, now);
}

uint64_t system_counter_time_of(const SystemCounter *sc, uint64_t now,
                                uint64_t count)
{
	if (sc == NULL)
		return count == 0 ? now : EVENT_NEVER;
	return sc->ops->time_of(sc->ctx, now, count);
}

bool system_counter_listen(SystemCounter *sc, SystemCounterListener fn,
                           void *ctx)
{
	if (sc == NULL)
		return true;
	if (sc->num_listeners == SYSTEM_COUNTER_MAX_LISTENERS)
		return false;
	sc->listeners[sc->num_listeners].fn = fn;
	sc->listeners[sc->num_listeners].ctx = ctx;
	sc->num_listeners++;
	return true;
}

void system_counter_changed(SystemCounter *sc, uint64_t before, uint64_t after)
{
	for (unsigned i = 0; i < sc->num_listeners; i++)
		sc->listeners[i].fn(sc->listeners[i].ctx, before, after);
}
