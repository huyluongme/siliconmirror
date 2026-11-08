/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The Generic Timer of the AArch64 model, D12, with the registers of
 * D24.10 as an Armv8.0 PE without FEAT_VHE or FEAT_ECV has them: the
 * counter registers, the four timers of a PE with EL3 and EL2, and the
 * two event streams.
 *
 * The count comes from the system counter that the board connects, at
 * the virtual time of the instruction that reads it (machine_now()). A
 * timer that is enabled and whose condition is not met yet has an event
 * at the time the count will meet it; the system counter tells the PE
 * when its rate changes or it jumps, and the PE then recomputes those
 * times. Each timer drives an interrupt output with ENABLE, IMASK and
 * ISTATUS (D12.2.4); taking the interrupt is up to whatever the board
 * connects it to.
 *
 * Choices where the manual leaves one:
 *  - While ENABLE is 0, ISTATUS reads as 0 and TimerValue reads as if
 *    the timer were enabled. Both are UNKNOWN; the manual says the
 *    TimerValue keeps decrementing while the timer is disabled.
 *  - An event stream generates an event when the selected bit of the
 *    count changes in the selected direction (D12.2.3). This model
 *    finds the next count at which the bit changes as if the count
 *    passed every value; it does not when the system counter steps by 2,
 *    but then an event stream from bit 0 is the only one that differs.
 */

#include "core/machine.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"
#include "util/bits.h"

/* CNTKCTL_EL1 bits, D24.10.15. */
#define KCTL_EL0PCTEN (1u << 0) /* EL0 reads CNTPCT_EL0, and CNTFRQ_EL0 */
#define KCTL_EL0VCTEN (1u << 1) /* EL0 reads CNTVCT_EL0, and CNTFRQ_EL0 */
#define KCTL_EL0VTEN (1u << 8)  /* EL0 accesses CNTV_* */
#define KCTL_EL0PTEN (1u << 9)  /* EL0 accesses CNTP_* */
#define KCTL_MASK 0x3ffu

/* CNTHCTL_EL2 bits, D24.10.2, as they are while HCR_EL2.E2H is 0. */
#define HCTL_EL1PCTEN (1u << 0) /* EL0 and EL1 read CNTPCT_EL0 */
#define HCTL_EL1PCEN (1u << 1)  /* EL0 and EL1 access CNTP_* */
#define HCTL_MASK 0xffu

/* The event stream fields, in the same place in both registers. */
#define EVNTEN (1u << 2)
#define EVNTDIR (1u << 3) /* events on 1 to 0 transitions, not 0 to 1 */
#define EVNTI_SHIFT 4     /* bits 7:4, the bit of the count */

/* CNT*_CTL_EL* bits, D24.10.22. */
#define CTL_ENABLE (1u << 0)
#define CTL_IMASK (1u << 1)
#define CTL_ISTATUS (1u << 2)

#define CNTFRQ_MASK UINT64_C(0xffffffff)

static bool have_el2(const Arm64Cpu *c)
{
	return c->config.highest_el >= 2;
}

static bool have_el3(const Arm64Cpu *c)
{
	return c->config.highest_el == 3;
}

static uint64_t now(const Arm64Cpu *c)
{
	return c->cpu.machine == NULL ? 0 : machine_now(c->cpu.machine);
}

/* PhysicalCountInt(), D12.1.2. */
static uint64_t physical_count(const Arm64Cpu *c)
{
	return system_counter_read(c->config.counter, now(c));
}

/*
 * The offset of the virtual count, CNTVOFF_EL2. With EL2 implemented and
 * no FEAT_VHE, it applies at every Exception level.
 */
static uint64_t virtual_offset(const Arm64Cpu *c)
{
	return have_el2(c) ? c->timer.cntvoff_el2 : 0;
}

/* The offset of a timer: only the virtual timer has one (D12.2.4.1). */
static uint64_t timer_offset(const Arm64Cpu *c, const Arm64Timer *t)
{
	return t == &c->timer.timers[ARM64_TIMER_VIRT] ? virtual_offset(c) : 0;
}

/*
 * The timer condition, ENABLE aside: (count - offset) - CompareValue >= 0
 * with all three values unsigned (D12.2.4.1).
 */
static bool condition_met(const Arm64Cpu *c, const Arm64Timer *t,
                          uint64_t count)
{
	return count - timer_offset(c, t) >= t->cval;
}

static bool istatus(const Arm64Cpu *c, const Arm64Timer *t, uint64_t count)
{
	return (t->ctl & CTL_ENABLE) && condition_met(c, t, count);
}

/*
 * Bring a timer up to date after a change of its registers or of the
 * count: drive its output, and schedule the event of its condition
 * being met.
 */
static void update_timer(Arm64Cpu *c, Arm64Timer *t)
{
	uint64_t count = physical_count(c);
	bool met = istatus(c, t, count);
	Machine *m = c->cpu.machine;

	irq_set(t->irq, met && !(t->ctl & CTL_IMASK));
	if (m == NULL)
		return;
	event_cancel(&m->events, &t->event);
	if (!(t->ctl & CTL_ENABLE) || met)
		return;

	/* The count at which the condition becomes met. */
	uint64_t to_go = t->cval - (count - timer_offset(c, t));
	uint64_t target = count + to_go;

	if (target < count)
		return; /* past the end of the count: never */

	uint64_t when = system_counter_time_of(c->config.counter, now(c), target);

	if (when != EVENT_NEVER)
		event_schedule(&m->events, &t->event, when);
}

static void timer_event(void *ctx)
{
	Arm64Timer *t = ctx;

	update_timer(t->cpu, t);
}

/* The control register of an event stream, or 0 if there is none. */
static uint64_t stream_control(const Arm64Cpu *c, unsigned stream)
{
	if (stream == 0)
		return c->timer.cntkctl_el1;
	return have_el2(c) ? c->timer.cnthctl_el2 : 0;
}

/* The count an event stream watches: virtual for 0, physical for 1. */
static uint64_t stream_offset(const Arm64Cpu *c, unsigned stream)
{
	return stream == 0 ? virtual_offset(c) : 0;
}

/*
 * Whether a change of the count from before to after makes the event
 * stream generate an event, as TestEventCNTV() and TestEventCNTP() do
 * (J1.4.3) for one change of the count.
 */
static bool stream_triggers(const Arm64Cpu *c, unsigned stream, uint64_t before,
                            uint64_t after)
{
	uint64_t control = stream_control(c, stream);
	unsigned n = (control >> EVNTI_SHIFT) & 0xf;
	uint64_t offset = stream_offset(c, stream);
	bool previous = (before - offset) >> n & 1;
	bool sample = (after - offset) >> n & 1;

	if (!(control & EVNTEN))
		return false;
	return control & EVNTDIR ? previous && !sample : !previous && sample;
}

/* Schedule the next event of an event stream. */
static void schedule_stream(Arm64Cpu *c, unsigned stream)
{
	Machine *m = c->cpu.machine;
	uint64_t control = stream_control(c, stream);

	if (m == NULL)
		return;
	event_cancel(&m->events, &c->timer.streams[stream]);
	if (!(control & EVNTEN))
		return;

	/* The next value of the watched count where bit n changes. */
	unsigned n = (control >> EVNTI_SHIFT) & 0xf;
	uint64_t period = UINT64_C(2) << n;
	uint64_t count = physical_count(c);
	uint64_t value = count - stream_offset(c, stream);
	uint64_t next =
		(value & ~(period - 1)) + (control & EVNTDIR ? period : period / 2);

	if (next <= value)
		next += period;

	uint64_t target = count + (next - value);

	if (target < count)
		return; /* past the end of the count: never */

	uint64_t when = system_counter_time_of(c->config.counter, now(c), target);

	if (when != EVENT_NEVER)
		event_schedule(&m->events, &c->timer.streams[stream], when);
}

static void stream_event(Arm64Cpu *c, unsigned stream)
{
	arm64_set_event_register(c);
	schedule_stream(c, stream);
}

static void virtual_stream_event(void *ctx)
{
	stream_event(ctx, 0);
}

static void physical_stream_event(void *ctx)
{
	stream_event(ctx, 1);
}

/* The system counter changed rate or jumped. */
static void counter_changed(void *ctx, uint64_t before, uint64_t after)
{
	Arm64Cpu *c = ctx;

	for (unsigned s = 0; s < 2; s++) {
		if (before != after && stream_triggers(c, s, before, after))
			arm64_set_event_register(c);
		schedule_stream(c, s);
	}
	for (unsigned i = 0; i < ARM64_NUM_TIMERS; i++)
		update_timer(c, &c->timer.timers[i]);
}

bool arm64_timer_init(Arm64Cpu *c)
{
	for (unsigned i = 0; i < ARM64_NUM_TIMERS; i++) {
		Arm64Timer *t = &c->timer.timers[i];

		t->cpu = c;
		t->irq = NULL;
		event_init(&t->event, timer_event, t);
	}
	event_init(&c->timer.streams[0], virtual_stream_event, c);
	event_init(&c->timer.streams[1], physical_stream_event, c);
	return system_counter_listen(c->config.counter, counter_changed, c);
}

void arm64_timer_reset(Arm64Cpu *c)
{
	Machine *m = c->cpu.machine;

	/*
	 * The Cortex-A53's values (A53 TRM 4.3): CNTKCTL_EL1 bits 9:8 and 2:0
	 * clear, CNTHCTL_EL2 bit 2 clear and bits 1:0 set, ENABLE clear in
	 * the timer controls. The rest is UNKNOWN, and clear here.
	 */
	c->timer.cntfrq_el0 = 0;
	c->timer.cntkctl_el1 = 0;
	c->timer.cnthctl_el2 = HCTL_EL1PCEN | HCTL_EL1PCTEN;
	c->timer.cntvoff_el2 = 0;
	for (unsigned s = 0; s < 2 && m != NULL; s++)
		event_cancel(&m->events, &c->timer.streams[s]);
	for (unsigned i = 0; i < ARM64_NUM_TIMERS; i++) {
		Arm64Timer *t = &c->timer.timers[i];

		t->ctl = 0;
		t->cval = 0;
		update_timer(c, t); /* drives the output low */
	}
}

void arm64_cpu_connect_timer(Arm64Cpu *c, Arm64TimerId id, IrqLine *line)
{
	Arm64Timer *t = &c->timer.timers[id];

	t->irq = line;
	irq_set(line, istatus(c, t, physical_count(c)) && !(t->ctl & CTL_IMASK));
}

/*
 * The timer that a CNT*_TVAL, CNT*_CTL or CNT*_CVAL register belongs to,
 * or -1. The three registers of a timer differ only in op2, bits 2:0.
 */
static int timer_index(uint16_t encoding)
{
	switch (encoding & ~7u) {
	case ARM64_CNTP_TVAL_EL0:
		return ARM64_TIMER_PHYS;
	case ARM64_CNTV_TVAL_EL0:
		return ARM64_TIMER_VIRT;
	case ARM64_CNTHP_TVAL_EL2:
		return ARM64_TIMER_HYP;
	case ARM64_CNTPS_TVAL_EL1:
		return ARM64_TIMER_SECURE;
	default:
		return -1;
	}
}

static Arm64Timer *timer_of(Arm64Cpu *c, uint16_t encoding)
{
	int index = timer_index(encoding);

	return index < 0 ? NULL : &c->timer.timers[index];
}

int arm64_timer_trap(const Arm64Cpu *c, uint16_t encoding, bool read)
{
	uint64_t kctl = c->timer.cntkctl_el1, hctl = c->timer.cnthctl_el2;
	bool el2_traps = c->el <= 1 && arm64_el2_enabled(c);

	if (encoding == ARM64_CNTFRQ_EL0) {
		/* Only the highest Exception level writes it. */
		if (!read)
			return c->el == c->config.highest_el ? 0 : -1;
		if (c->el == 0 && !(kctl & (KCTL_EL0PCTEN | KCTL_EL0VCTEN)))
			return (int)arm64_el1_target(c);
		return 0;
	}
	if (encoding == ARM64_CNTPCT_EL0) {
		if (c->el == 0 && !(kctl & KCTL_EL0PCTEN))
			return (int)arm64_el1_target(c);
		if (el2_traps && !(hctl & HCTL_EL1PCTEN))
			return 2;
		return 0;
	}
	if (encoding == ARM64_CNTVCT_EL0) {
		if (c->el == 0 && !(kctl & KCTL_EL0VCTEN))
			return (int)arm64_el1_target(c);
		return 0;
	}
	switch (timer_index(encoding)) {
	case ARM64_TIMER_PHYS:
		if (c->el == 0 && !(kctl & KCTL_EL0PTEN))
			return (int)arm64_el1_target(c);
		if (el2_traps && !(hctl & HCTL_EL1PCEN))
			return 2;
		return 0;
	case ARM64_TIMER_VIRT:
		if (c->el == 0 && !(kctl & KCTL_EL0VTEN))
			return (int)arm64_el1_target(c);
		return 0;
	case ARM64_TIMER_SECURE:
		/* Secure EL1 only with SCR_EL3.ST set; EL3 always. */
		if (!have_el3(c) || c->el == 2)
			return -1;
		if (c->el == 1) {
			if (c->sys.scr_el3 & ARM64_SCR_NS)
				return -1;
			if (!(c->sys.scr_el3 & ARM64_SCR_ST))
				return 3;
		}
		return 0;
	default: /* CNTKCTL_EL1, the EL2 registers: min_el decides */
		return 0;
	}
}

bool arm64_timer_read(Arm64Cpu *c, uint16_t encoding, uint64_t *value)
{
	Arm64Timer *t = timer_of(c, encoding);
	uint64_t count = physical_count(c);

	switch (encoding) {
	case ARM64_CNTFRQ_EL0:
		*value = c->timer.cntfrq_el0;
		return true;
	case ARM64_CNTPCT_EL0:
		*value = count;
		return true;
	case ARM64_CNTVCT_EL0:
		*value = count - virtual_offset(c);
		return true;
	case ARM64_CNTKCTL_EL1:
		*value = c->timer.cntkctl_el1;
		return true;
	case ARM64_CNTHCTL_EL2:
		*value = c->timer.cnthctl_el2;
		return true;
	case ARM64_CNTVOFF_EL2:
		*value = c->timer.cntvoff_el2;
		return true;
	default:
		break;
	}
	if (t == NULL)
		return false;
	switch (encoding & 7) {
	case 0: /* TVAL: CompareValue - (count - offset), bits 31:0 */
		*value = (uint32_t)(t->cval - (count - timer_offset(c, t)));
		return true;
	case 1: /* CTL */
		*value = t->ctl | (istatus(c, t, count) ? CTL_ISTATUS : 0);
		return true;
	default: /* CVAL */
		*value = t->cval;
		return true;
	}
}

bool arm64_timer_write(Arm64Cpu *c, uint16_t encoding, uint64_t value)
{
	Arm64Timer *t = timer_of(c, encoding);

	switch (encoding) {
	case ARM64_CNTFRQ_EL0:
		c->timer.cntfrq_el0 = value & CNTFRQ_MASK;
		return true;
	case ARM64_CNTKCTL_EL1:
		c->timer.cntkctl_el1 = value & KCTL_MASK;
		schedule_stream(c, 0);
		return true;
	case ARM64_CNTHCTL_EL2:
		c->timer.cnthctl_el2 = value & HCTL_MASK;
		schedule_stream(c, 1);
		return true;
	case ARM64_CNTVOFF_EL2:
		c->timer.cntvoff_el2 = value;
		schedule_stream(c, 0);
		update_timer(c, &c->timer.timers[ARM64_TIMER_VIRT]);
		return true;
	default:
		break;
	}
	if (t == NULL)
		return false; /* CNTPCT_EL0 and CNTVCT_EL0 are read-only */
	switch (encoding & 7) {
	case 0: /* TVAL: CompareValue = count - offset + SignExtend(value) */
		t->cval = physical_count(c) - timer_offset(c, t) +
		          sign_extend64(value & UINT32_MAX, 32);
		break;
	case 1: /* CTL: ISTATUS is read-only */
		t->ctl = value & (CTL_ENABLE | CTL_IMASK);
		break;
	default: /* CVAL */
		t->cval = value;
		break;
	}
	update_timer(c, t);
	return true;
}
