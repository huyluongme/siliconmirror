/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Pending events are kept in a list sorted by time. A machine has only a
 * handful of them (one per active timer), so a list is simpler than a heap
 * and fast enough.
 */

#include "core/event.h"

#include <assert.h>
#include <stddef.h>

void event_queue_init(EventQueue *q)
{
	q->now = 0;
	q->head = NULL;
}

uint64_t event_queue_now(const EventQueue *q)
{
	return q->now;
}

uint64_t event_queue_next(const EventQueue *q)
{
	return q->head != NULL ? q->head->when : EVENT_NEVER;
}

void event_queue_run_until(EventQueue *q, uint64_t t)
{
	assert(t >= q->now);

	while (q->head != NULL && q->head->when <= t) {
		Event *ev = q->head;

		q->head = ev->next;
		ev->next = NULL;
		ev->pending = false;
		q->now = ev->when;
		ev->fn(ev->ctx);
	}
	q->now = t;
}

void event_init(Event *ev, EventFn fn, void *ctx)
{
	ev->fn = fn;
	ev->ctx = ctx;
	ev->when = 0;
	ev->pending = false;
	ev->next = NULL;
}

void event_cancel(EventQueue *q, Event *ev)
{
	if (!ev->pending)
		return;

	Event **link = &q->head;

	while (*link != ev)
		link = &(*link)->next;
	*link = ev->next;
	ev->next = NULL;
	ev->pending = false;
}

void event_schedule(EventQueue *q, Event *ev, uint64_t when)
{
	event_cancel(q, ev);
	if (when < q->now)
		when = q->now;

	/* Insert after every event due at the same time or earlier. */
	Event **link = &q->head;

	while (*link != NULL && (*link)->when <= when)
		link = &(*link)->next;
	ev->when = when;
	ev->pending = true;
	ev->next = *link;
	*link = ev;
}

bool event_pending(const Event *ev)
{
	return ev->pending;
}
