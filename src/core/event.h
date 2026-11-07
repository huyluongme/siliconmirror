/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Virtual time and timed events.
 *
 * Virtual time counts nanoseconds from the moment the machine was
 * created. It only moves while the machine runs, so it does not depend on
 * the speed of the host, and running the same guest twice gives the same
 * result.
 *
 * A device that needs something to happen later, such as a timer that
 * expires, schedules an Event. The Event belongs to the device, usually as
 * a member of its state struct, so scheduling never allocates memory.
 * Events due at the same time fire in the order they were scheduled.
 */

#ifndef CORE_EVENT_H
#define CORE_EVENT_H

#include <stdbool.h>
#include <stdint.h>

/* A time that never comes: returned when no event is pending. */
#define EVENT_NEVER UINT64_MAX

typedef void (*EventFn)(void *ctx);

typedef struct Event {
	EventFn fn;
	void *ctx;
	uint64_t when;      /* virtual time it fires at, while pending */
	bool pending;       /* scheduled and not fired or cancelled yet */
	struct Event *next; /* next pending event in the queue */
} Event;

typedef struct EventQueue {
	uint64_t now; /* current virtual time, in nanoseconds */
	Event *head;  /* pending events, earliest first */
} EventQueue;

void event_queue_init(EventQueue *q);

/* Current virtual time. */
uint64_t event_queue_now(const EventQueue *q);

/* Time of the earliest pending event, or EVENT_NEVER. */
uint64_t event_queue_next(const EventQueue *q);

/*
 * Move virtual time forward to t, firing every event due at or before t in
 * order. While an event function runs, the current time is the time of
 * that event. t must not be in the past.
 */
void event_queue_run_until(EventQueue *q, uint64_t t);

/* Set up an event that will call fn(ctx). */
void event_init(Event *ev, EventFn fn, void *ctx);

/*
 * Fire ev at virtual time when; a time in the past means now. If ev is
 * already pending, it is moved to the new time.
 */
void event_schedule(EventQueue *q, Event *ev, uint64_t when);

/* Stop ev from firing. Does nothing if it is not pending. */
void event_cancel(EventQueue *q, Event *ev);

bool event_pending(const Event *ev);

#endif /* CORE_EVENT_H */
