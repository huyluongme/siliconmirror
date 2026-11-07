/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Unit tests for virtual time and timed events (src/core/event.c). */

#include <stdint.h>

#include "core/event.h"
#include "test.h"
#include "util/macros.h"

/* Every event that fires is appended here, with the time it fired at. */
typedef struct Fired {
	char name;
	uint64_t time;
} Fired;

static Fired fired[16];
static unsigned num_fired;

typedef struct TestEvent {
	Event ev;
	EventQueue *q;
	char name;
	unsigned repeat; /* reschedule itself this many more times */
	uint64_t period;
} TestEvent;

static void record(void *ctx)
{
	TestEvent *t = ctx;

	if (num_fired < ARRAY_SIZE(fired)) {
		fired[num_fired].name = t->name;
		fired[num_fired].time = event_queue_now(t->q);
	}
	num_fired++;
	if (t->repeat > 0) {
		t->repeat--;
		event_schedule(t->q, &t->ev, event_queue_now(t->q) + t->period);
	}
}

static void make_event(TestEvent *t, EventQueue *q, char name)
{
	*t = (TestEvent){.q = q, .name = name};
	event_init(&t->ev, record, t);
}

static void reset_record(void)
{
	num_fired = 0;
}

static void test_empty_queue(void)
{
	EventQueue q;

	event_queue_init(&q);
	CHECK_EQ(event_queue_now(&q), 0);
	CHECK_EQ(event_queue_next(&q), EVENT_NEVER);
	event_queue_run_until(&q, 500);
	CHECK_EQ(event_queue_now(&q), 500);
}

static void test_fire_in_time_order(void)
{
	EventQueue q;
	TestEvent a, b, c;

	reset_record();
	event_queue_init(&q);
	make_event(&a, &q, 'a');
	make_event(&b, &q, 'b');
	make_event(&c, &q, 'c');

	event_schedule(&q, &c.ev, 300);
	event_schedule(&q, &a.ev, 100);
	event_schedule(&q, &b.ev, 200);
	CHECK_EQ(event_queue_next(&q), 100);

	event_queue_run_until(&q, 1000);
	CHECK_EQ(num_fired, 3);
	CHECK_EQ(fired[0].name, 'a');
	CHECK_EQ(fired[0].time, 100);
	CHECK_EQ(fired[1].name, 'b');
	CHECK_EQ(fired[1].time, 200);
	CHECK_EQ(fired[2].name, 'c');
	CHECK_EQ(fired[2].time, 300);
	CHECK_EQ(event_queue_now(&q), 1000);
	CHECK(!event_pending(&a.ev));
}

static void test_same_time_fires_in_scheduling_order(void)
{
	EventQueue q;
	TestEvent x, y, z;

	reset_record();
	event_queue_init(&q);
	make_event(&x, &q, 'x');
	make_event(&y, &q, 'y');
	make_event(&z, &q, 'z');

	event_schedule(&q, &x.ev, 100);
	event_schedule(&q, &y.ev, 100);
	event_schedule(&q, &z.ev, 100);
	event_queue_run_until(&q, 100);
	CHECK_EQ(num_fired, 3);
	CHECK_EQ(fired[0].name, 'x');
	CHECK_EQ(fired[1].name, 'y');
	CHECK_EQ(fired[2].name, 'z');
}

static void test_run_until_stops_at_t(void)
{
	EventQueue q;
	TestEvent a, b;

	reset_record();
	event_queue_init(&q);
	make_event(&a, &q, 'a');
	make_event(&b, &q, 'b');

	event_schedule(&q, &a.ev, 100);
	event_schedule(&q, &b.ev, 200);
	event_queue_run_until(&q, 150);
	CHECK_EQ(num_fired, 1);
	CHECK_EQ(event_queue_now(&q), 150);
	CHECK_EQ(event_queue_next(&q), 200);
	CHECK(event_pending(&b.ev));
}

static void test_cancel(void)
{
	EventQueue q;
	TestEvent a, b;

	reset_record();
	event_queue_init(&q);
	make_event(&a, &q, 'a');
	make_event(&b, &q, 'b');

	event_schedule(&q, &a.ev, 100);
	event_schedule(&q, &b.ev, 200);
	event_cancel(&q, &a.ev);
	CHECK(!event_pending(&a.ev));
	CHECK_EQ(event_queue_next(&q), 200);

	/* Cancelling an event that is not pending does nothing. */
	event_cancel(&q, &a.ev);

	event_queue_run_until(&q, 1000);
	CHECK_EQ(num_fired, 1);
	CHECK_EQ(fired[0].name, 'b');
}

static void test_cancel_from_middle_of_queue(void)
{
	EventQueue q;
	TestEvent a, b, c;

	reset_record();
	event_queue_init(&q);
	make_event(&a, &q, 'a');
	make_event(&b, &q, 'b');
	make_event(&c, &q, 'c');

	event_schedule(&q, &a.ev, 100);
	event_schedule(&q, &b.ev, 200);
	event_schedule(&q, &c.ev, 300);
	event_cancel(&q, &b.ev);
	event_queue_run_until(&q, 1000);
	CHECK_EQ(num_fired, 2);
	CHECK_EQ(fired[0].name, 'a');
	CHECK_EQ(fired[1].name, 'c');
}

static void test_reschedule_moves_event(void)
{
	EventQueue q;
	TestEvent a, b;

	reset_record();
	event_queue_init(&q);
	make_event(&a, &q, 'a');
	make_event(&b, &q, 'b');

	event_schedule(&q, &a.ev, 100);
	event_schedule(&q, &b.ev, 200);
	event_schedule(&q, &a.ev, 300);
	event_queue_run_until(&q, 1000);
	CHECK_EQ(num_fired, 2);
	CHECK_EQ(fired[0].name, 'b');
	CHECK_EQ(fired[1].name, 'a');
	CHECK_EQ(fired[1].time, 300);
}

static void test_past_time_means_now(void)
{
	EventQueue q;
	TestEvent a;

	reset_record();
	event_queue_init(&q);
	make_event(&a, &q, 'a');

	event_queue_run_until(&q, 1000);
	event_schedule(&q, &a.ev, 500);
	CHECK_EQ(event_queue_next(&q), 1000);
	event_queue_run_until(&q, 1000);
	CHECK_EQ(num_fired, 1);
	CHECK_EQ(fired[0].time, 1000);
}

static void test_periodic_event(void)
{
	EventQueue q;
	TestEvent tick;

	reset_record();
	event_queue_init(&q);
	make_event(&tick, &q, 't');
	tick.repeat = 4;
	tick.period = 100;

	/* Fires at 100, then reschedules itself 4 times, 100 ns apart. */
	event_schedule(&q, &tick.ev, 100);
	event_queue_run_until(&q, 1000);
	CHECK_EQ(num_fired, 5);
	for (unsigned i = 0; i < 5; i++)
		CHECK_EQ(fired[i].time, 100 * (i + 1));
	CHECK(!event_pending(&tick.ev));
}

int main(void)
{
	RUN_TEST(test_empty_queue);
	RUN_TEST(test_fire_in_time_order);
	RUN_TEST(test_same_time_fires_in_scheduling_order);
	RUN_TEST(test_run_until_stops_at_t);
	RUN_TEST(test_cancel);
	RUN_TEST(test_cancel_from_middle_of_queue);
	RUN_TEST(test_reschedule_moves_event);
	RUN_TEST(test_past_time_means_now);
	RUN_TEST(test_periodic_event);
	return test_summary();
}
