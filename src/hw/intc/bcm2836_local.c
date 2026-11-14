/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The ARM local peripherals of the BCM2836 and BCM2837. Section numbers
 * refer to Broadcom's "BCM2836 ARM-local peripherals", rev 3.4.
 *
 * The core timer (sections 3.1 and 4.2 to 4.3) counts the edges of its
 * clock, the crystal or the APB clock (control bit 8), through a 32-bit
 * prescaler that divides by 2^31 / prescaler: 0x80000000 divides by 1,
 * and 0 stops the timer. The document gives this ratio and allows
 * fractional ones; this model gets them with an accumulator, the usual
 * way to build such a divider: each clock edge adds the prescaler to it,
 * and each time it reaches 2^31, 2^31 is taken off and the timer steps
 * by 1, or by 2 when control bit 9 is set. Its details are not documented
 * and are this model's choice. So are these:
 *  - Writing the timer's high half "sets the pre-scaler to zero"
 *    (section 3.1.2). The prescaler register itself becoming 0 would
 *    stop the timer, so this model reads it as the divider starting
 *    over: the accumulator becomes 0.
 *  - A prescaler above 2^31, which the document says not to use, and
 *    accesses other than 32-bit ones stop the machine, as accesses to
 *    registers not implemented yet do.
 *
 * Clock edges happen at fixed points of virtual time, k * 1e9 / hz ns
 * for the k-th edge of a clock of hz Hz, whichever clock the timer uses.
 * The timer's value is only computed when read; the model keeps its
 * value and accumulator at the last change of its settings.
 *
 * Interrupt routing (sections 3.2 and 4.4 to 4.10). Each core has an
 * IRQ and an FIQ output. The interrupts of a core, its four timers, its
 * four mailboxes and its performance monitors, go to its own IRQ or FIQ,
 * the FIQ bit of their routing winning over the IRQ bit. The GPU's IRQ
 * and FIQ, and the local timer, go to the core that their routing names.
 * The AXI outstanding interrupt goes to the IRQ of core 0. The source
 * registers show what drives each output.
 *
 * Mailboxes (sections 3.3 and 4.8): sixteen 32-bit registers, four per
 * core, with write-set and read and write-clear addresses. A mailbox
 * interrupts while it is not 0.
 *
 * The local timer (section 4.11) counts down on every edge of the
 * crystal clock, 38.4 million pulses a second from 19.2 MHz. When it
 * gets to 0 it reloads and sets its interrupt flag, which stays set until
 * software clears it. This model takes "gets to 0" as the pulse that
 * would make the count 0, so that the flag sets every reload-value
 * pulses; a reload value of 0 acts as 1.
 *
 * The AXI outstanding interrupt (section 4.9) comes when the cores have
 * had no AXI transfer outstanding for a time-out. This model has no
 * caches, so a core that runs makes transfers all the time: the bus is
 * quiet only while no core runs, which the machine's idle listener tells,
 * and an access to this block is a transfer. The document does not say
 * how the interrupt ends; since reading the source registers is a
 * transfer too, this model keeps it until software clears its enable
 * bit. The outstanding counters read as one read, the read of them.
 *
 * What else the document leaves open, this model chooses:
 *  - Reads of write-only registers and reserved bits give 0; writes to
 *    read-only registers and reserved bits are ignored. The two PMU
 *    routing registers both read as the routing.
 *  - 0x18 and 0x3c, unused, read as 0 and ignore writes. The deprecated
 *    0x04 and the routing of local interrupts 8 to 15 at 0x28 have
 *    contents the document does not give: they stop the machine.
 *  - In the mailbox routing, the document says that bit 4 overrides bit
 *    1; this model follows the pattern of every other bit, bit 5.
 *
 * The cores take turns running through the same stretch of virtual
 * time, so a core can access the block at an earlier time than the core
 * that changed it last. The block then sees the time of that change: its
 * time never goes back.
 */

#include "hw/intc/bcm2836_local.h"

#include <inttypes.h>
#include <stdlib.h>

#include "util/muldiv.h"

/* Registers, as offsets from the base. */
enum {
	REG_CONTROL = 0x00,
	REG_DEPRECATED = 0x04,
	REG_PRESCALER = 0x08,
	REG_GPU_ROUTING = 0x0c,
	REG_PMU_SET = 0x10,
	REG_PMU_CLEAR = 0x14,
	REG_UNUSED_18 = 0x18,
	REG_TIMER_LS = 0x1c,
	REG_TIMER_MS = 0x20,
	REG_LOCAL_ROUTING = 0x24,
	REG_LOCAL_ROUTING_8_15 = 0x28,
	REG_AXI_COUNTERS = 0x2c,
	REG_AXI_IRQ = 0x30,
	REG_LOCAL_TIMER = 0x34,
	REG_LOCAL_TIMER_FLAGS = 0x38,
	REG_UNUSED_3C = 0x3c,
	/* One register per core, 4 bytes apart. */
	REG_TIMER_ROUTING = 0x40,
	REG_MAILBOX_ROUTING = 0x50,
	REG_IRQ_SOURCE = 0x60,
	REG_FIQ_SOURCE = 0x70,
	/* One register per mailbox, core * 4 + mailbox, 4 bytes apart. */
	REG_MAILBOX_SET = 0x80,
	REG_MAILBOX_CLEAR = 0xc0,
};

#define CONTROL_APB (1u << 8)   /* the timer runs from the APB clock */
#define CONTROL_STEP2 (1u << 9) /* the timer increments by 2 */
#define PRESCALER_ONE (UINT32_C(1) << 31)
#define NS_PER_SECOND UINT64_C(1000000000)

/* Bits of the source registers, section 4.10. */
#define SOURCE_MAILBOX0 (1u << 4)
#define SOURCE_GPU (1u << 8)
#define SOURCE_PMU (1u << 9)
#define SOURCE_AXI (1u << 10)
#define SOURCE_LOCAL_TIMER (1u << 11)

#define ROUTING_FIQ (1u << 2) /* in the local interrupt routing */

#define LOCAL_TIMER_RELOAD 0x0fffffffu
#define LOCAL_TIMER_ENABLE (1u << 28)
#define LOCAL_TIMER_IRQ_ENABLE (1u << 29)
#define LOCAL_TIMER_FLAG (UINT32_C(1) << 31)
#define LOCAL_TIMER_RELOAD_NOW (1u << 30) /* in the write flags */

#define AXI_TIMEOUT 0xfffffu
#define AXI_IRQ_ENABLE (1u << 20)

#define NUM_MAILBOXES (4 * BCM2836_LOCAL_NUM_CORES)

/* Interrupt input lines: a core's are core * 8 + input, then the GPU's. */
#define LINE_GPU_IRQ (8 * BCM2836_LOCAL_NUM_CORES)
#define LINE_GPU_FIQ (LINE_GPU_IRQ + 1)

struct Bcm2836Local {
	Machine *machine;
	Bcm2836LocalConfig config;
	SystemCounter counter;

	uint32_t control;
	uint32_t prescaler;

	/* The core timer at base_time, the time of the last change. */
	uint64_t base_time;
	uint64_t base_count;
	uint32_t base_acc; /* the accumulator, below 2^31 */

	uint32_t read_hold;  /* high half latched by a read of the low half */
	uint32_t write_hold; /* low half waiting for a write of the high half */

	/* Routing. */
	uint32_t gpu_routing;
	uint32_t pmu_routing;
	uint32_t local_routing;
	uint32_t timer_routing[BCM2836_LOCAL_NUM_CORES];
	uint32_t mailbox_routing[BCM2836_LOCAL_NUM_CORES];
	uint32_t mailbox[NUM_MAILBOXES];

	IrqLine inputs[BCM2836_LOCAL_NUM_CORES][BCM2836_LOCAL_NUM_INPUTS];
	IrqLine gpu_irq, gpu_fiq;
	IrqLine *irq_out[BCM2836_LOCAL_NUM_CORES];
	IrqLine *fiq_out[BCM2836_LOCAL_NUM_CORES];

	/*
	 * The local timer: its control register, without the flag; its
	 * flag; the next pulse at which it gets to 0 while it is enabled;
	 * the time it was brought to last, which never goes back.
	 */
	uint32_t local_timer;
	bool local_flag;
	uint64_t local_next_zero;
	uint64_t local_time;
	Event local_event;

	/*
	 * The AXI outstanding interrupt: its register, whether the time-out
	 * passed, and whether the bus is quiet, since when.
	 */
	uint32_t axi_irq;
	bool axi_expired;
	bool quiet;
	uint64_t quiet_since;
	Event axi_event;
};

static uint64_t clock_hz(const Bcm2836Local *local)
{
	return local->control & CONTROL_APB ? local->config.apb_hz
	                                    : local->config.crystal_hz;
}

/* Edges of a clock of hz Hz from time 0 up to time t, both included. */
static uint64_t edges_at(uint64_t t, uint64_t hz)
{
	return mul_div_floor_ns(t, hz);
}

/* Where the timer is at time now: its count and its accumulator. */
static void timer_at(const Bcm2836Local *local, uint64_t now, uint64_t *count,
                     uint32_t *acc)
{
	if (now < local->base_time)
		now = local->base_time;

	uint64_t hz = clock_hz(local);
	uint64_t edges = edges_at(now, hz) - edges_at(local->base_time, hz);
	uint128_t total = (uint128_t)edges * local->prescaler + local->base_acc;
	uint64_t steps = (uint64_t)(total >> 31);

	*count =
		local->base_count + steps * (local->control & CONTROL_STEP2 ? 2 : 1);
	*acc = (uint32_t)(total & (PRESCALER_ONE - 1));
}

/* Move the base to now, before a change of the settings. */
static void rebase(Bcm2836Local *local, uint64_t now)
{
	if (now < local->base_time)
		now = local->base_time;
	timer_at(local, now, &local->base_count, &local->base_acc);
	local->base_time = now;
}

static uint64_t counter_read(void *ctx, uint64_t now)
{
	uint64_t count;
	uint32_t acc;

	timer_at(ctx, now, &count, &acc);
	return count;
}

static uint64_t counter_time_of(void *ctx, uint64_t now, uint64_t target)
{
	Bcm2836Local *local = ctx;
	uint64_t count, hz = clock_hz(local);
	uint32_t acc;

	if (now < local->base_time)
		now = local->base_time;
	timer_at(local, now, &count, &acc);
	if (count >= target)
		return now;
	if (local->prescaler == 0)
		return EVENT_NEVER;

	/* Steps still to go, then clock edges after now to make them. */
	unsigned step = local->control & CONTROL_STEP2 ? 2 : 1;
	uint64_t steps = (target - count + step - 1) / step;
	uint128_t needed = ((uint128_t)steps << 31) - acc;
	uint128_t edges = (needed + local->prescaler - 1) / local->prescaler;
	uint128_t edge = edges_at(now, hz) + edges;

	/* The edge-th edge is at ceil(edge * 1e9 / hz). */
	if (edge > UINT64_MAX)
		return EVENT_NEVER;
	uint64_t when = mul_div_ceil((uint64_t)edge, NS_PER_SECOND, hz);

	return when < now ? now : when;
}

static const SystemCounterOps counter_ops = {
	.read = counter_read,
	.time_of = counter_time_of,
};

/*
 * A core interrupt with an IRQ bit and an FIQ bit 4 above it in control
 * (section 3.2.1): add source to the IRQ or FIQ sources.
 */
static void route_core(uint32_t control, unsigned bit, uint32_t source,
                       uint32_t *irq, uint32_t *fiq)
{
	if (control & (1u << (bit + 4)))
		*fiq |= source;
	else if (control & (1u << bit))
		*irq |= source;
}

/* The IRQ and FIQ source registers of a core, section 4.10. */
static void sources(const Bcm2836Local *local, unsigned core, uint32_t *irq,
                    uint32_t *fiq)
{
	const IrqLine *in = local->inputs[core];

	*irq = 0;
	*fiq = 0;
	for (unsigned i = 0; i < 4; i++) {
		if (irq_level(&in[i]))
			route_core(local->timer_routing[core], i, 1u << i, irq, fiq);
		if (local->mailbox[4 * core + i] != 0)
			route_core(local->mailbox_routing[core], i, SOURCE_MAILBOX0 << i,
			           irq, fiq);
	}
	if (irq_level(&in[BCM2836_LOCAL_PMU]))
		route_core(local->pmu_routing, core, SOURCE_PMU, irq, fiq);
	if (irq_level(&local->gpu_irq) && (local->gpu_routing & 3) == core)
		*irq |= SOURCE_GPU;
	if (irq_level(&local->gpu_fiq) && (local->gpu_routing >> 2 & 3) == core)
		*fiq |= SOURCE_GPU;
	if (local->local_flag && (local->local_timer & LOCAL_TIMER_IRQ_ENABLE) &&
	    (local->local_routing & 3) == core) {
		if (local->local_routing & ROUTING_FIQ)
			*fiq |= SOURCE_LOCAL_TIMER;
		else
			*irq |= SOURCE_LOCAL_TIMER;
	}
	if (core == 0 && local->axi_expired)
		*irq |= SOURCE_AXI;
}

/* Drive the outputs after a change. */
static void update(Bcm2836Local *local)
{
	for (unsigned core = 0; core < BCM2836_LOCAL_NUM_CORES; core++) {
		uint32_t irq, fiq;

		sources(local, core, &irq, &fiq);
		irq_set(local->irq_out[core], irq != 0);
		irq_set(local->fiq_out[core], fiq != 0);
	}
}

static void input_changed(void *ctx, unsigned n, bool level)
{
	(void)n;
	(void)level;
	update(ctx);
}

/* The local timer gets a pulse on each edge of the crystal clock. */
static uint64_t pulse_hz(const Bcm2836Local *local)
{
	return 2 * local->config.crystal_hz;
}

static uint64_t local_period(const Bcm2836Local *local)
{
	uint32_t reload = local->local_timer & LOCAL_TIMER_RELOAD;

	return reload != 0 ? reload : 1;
}

/* Bring the local timer to now: each time it got to 0 set the flag. */
static void local_timer_sync(Bcm2836Local *local, uint64_t now)
{
	if (now < local->local_time)
		now = local->local_time;
	local->local_time = now;
	if (!(local->local_timer & LOCAL_TIMER_ENABLE))
		return;

	uint64_t pulse = edges_at(now, pulse_hz(local));

	if (pulse >= local->local_next_zero) {
		uint64_t period = local_period(local);

		local->local_flag = true;
		local->local_next_zero +=
			period * ((pulse - local->local_next_zero) / period + 1);
	}
}

/*
 * Wait for the local timer to get to 0 while that changes something: it
 * is enabled and its flag is clear.
 */
static void local_timer_schedule(Bcm2836Local *local)
{
	EventQueue *q = &local->machine->events;

	if ((local->local_timer & LOCAL_TIMER_ENABLE) && !local->local_flag)
		event_schedule(q, &local->local_event,
		               mul_div_ceil(local->local_next_zero, NS_PER_SECOND,
		                            pulse_hz(local)));
	else
		event_cancel(q, &local->local_event);
}

/* Reload the local timer now: it gets to 0 a period of pulses later. */
static void local_timer_reload(Bcm2836Local *local)
{
	local->local_next_zero =
		edges_at(local->local_time, pulse_hz(local)) + local_period(local);
}

static void local_timer_fired(void *ctx)
{
	Bcm2836Local *local = ctx;

	local_timer_sync(local, machine_now(local->machine));
	local_timer_schedule(local);
	update(local);
}

/* Wait for the AXI time-out while the bus is quiet and that matters. */
static void axi_schedule(Bcm2836Local *local)
{
	EventQueue *q = &local->machine->events;

	if (local->quiet && (local->axi_irq & AXI_IRQ_ENABLE) &&
	    !local->axi_expired) {
		/* time_out * 16 + 15 clocks of the APB clock, section 4.9 */
		uint64_t hz = local->config.apb_hz;
		uint64_t clocks = (uint64_t)(local->axi_irq & AXI_TIMEOUT) * 16 + 15;
		uint64_t edge = edges_at(local->quiet_since, hz) + clocks;

		event_schedule(q, &local->axi_event,
		               mul_div_ceil(edge, NS_PER_SECOND, hz));
	} else {
		event_cancel(q, &local->axi_event);
	}
}

static void axi_fired(void *ctx)
{
	Bcm2836Local *local = ctx;

	local->axi_expired = true;
	update(local);
}

static void idle_changed(void *ctx, bool idle, uint64_t since)
{
	Bcm2836Local *local = ctx;

	local->quiet = idle;
	local->quiet_since = since;
	axi_schedule(local);
}

/* An access to the block is an AXI transfer: the bus is not quiet. */
static void axi_transfer(Bcm2836Local *local)
{
	local->quiet = false;
	axi_schedule(local);
}

static void not_implemented(Bcm2836Local *local, const char *access,
                            uint64_t offset, unsigned size)
{
	machine_stop(local->machine, MACHINE_STOP_ERROR,
	             "bcm2836-local: %u-byte %s offset 0x%02" PRIx64
	             " is not implemented yet",
	             size, access, offset);
}

static uint64_t local_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2836Local *local = ctx;
	uint64_t now = machine_now(local->machine);
	unsigned index = (unsigned)(offset >> 2 & 3);
	uint32_t irq, fiq;

	if (size != 4 || offset == REG_DEPRECATED ||
	    offset == REG_LOCAL_ROUTING_8_15) {
		not_implemented(local, "read from", offset, size);
		return 0;
	}
	axi_transfer(local);
	local_timer_sync(local, now);
	update(local);
	switch (offset) {
	case REG_CONTROL:
		return local->control;
	case REG_PRESCALER:
		return local->prescaler;
	case REG_GPU_ROUTING:
		return local->gpu_routing;
	case REG_PMU_SET:
	case REG_PMU_CLEAR:
		return local->pmu_routing;
	case REG_TIMER_LS: {
		uint64_t count = counter_read(local, now);

		local->read_hold = (uint32_t)(count >> 32);
		return (uint32_t)count;
	}
	case REG_TIMER_MS:
		return local->read_hold;
	case REG_LOCAL_ROUTING:
		return local->local_routing;
	case REG_AXI_COUNTERS:
		return 1; /* this read, and no write */
	case REG_AXI_IRQ:
		return local->axi_irq;
	case REG_LOCAL_TIMER:
		return local->local_timer | (local->local_flag ? LOCAL_TIMER_FLAG : 0);
	default:
		break;
	}
	if (offset >= REG_MAILBOX_CLEAR)
		return local->mailbox[(offset - REG_MAILBOX_CLEAR) >> 2];
	if (offset >= REG_MAILBOX_SET)
		return 0; /* write-only */
	if (offset >= REG_IRQ_SOURCE) {
		sources(local, index, &irq, &fiq);
		return offset >= REG_FIQ_SOURCE ? fiq : irq;
	}
	if (offset >= REG_MAILBOX_ROUTING)
		return local->mailbox_routing[index];
	if (offset >= REG_TIMER_ROUTING)
		return local->timer_routing[index];
	return 0; /* unused, or the write-only local timer flags */
}

static void local_write(void *ctx, uint64_t offset, uint64_t value,
                        unsigned size)
{
	Bcm2836Local *local = ctx;
	uint64_t now = machine_now(local->machine);
	unsigned index = (unsigned)(offset >> 2 & 3);
	uint32_t v = (uint32_t)value;
	uint64_t before;

	if (size != 4 || offset == REG_DEPRECATED ||
	    offset == REG_LOCAL_ROUTING_8_15) {
		not_implemented(local, "write to", offset, size);
		return;
	}
	axi_transfer(local);
	local_timer_sync(local, now);
	switch (offset) {
	case REG_CONTROL:
		rebase(local, now);
		local->control = v & (CONTROL_APB | CONTROL_STEP2);
		system_counter_changed(&local->counter, local->base_count,
		                       local->base_count);
		break;
	case REG_PRESCALER:
		if (v > PRESCALER_ONE) {
			machine_stop(local->machine, MACHINE_STOP_ERROR,
			             "bcm2836-local: prescaler 0x%08" PRIx32
			             " is above 2^31, which the chip's document says "
			             "not to use",
			             v);
			return;
		}
		rebase(local, now);
		local->prescaler = v;
		system_counter_changed(&local->counter, local->base_count,
		                       local->base_count);
		break;
	case REG_GPU_ROUTING:
		local->gpu_routing = v & 0xf;
		break;
	case REG_PMU_SET:
		local->pmu_routing |= v & 0xff;
		break;
	case REG_PMU_CLEAR:
		local->pmu_routing &= ~v & 0xff;
		break;
	case REG_TIMER_LS:
		local->write_hold = v;
		break;
	case REG_TIMER_MS:
		rebase(local, now);
		before = local->base_count;
		local->base_count = (uint64_t)v << 32 | local->write_hold;
		local->base_acc = 0;
		system_counter_changed(&local->counter, before, local->base_count);
		break;
	case REG_LOCAL_ROUTING:
		local->local_routing = v & 7;
		break;
	case REG_AXI_IRQ:
		local->axi_irq = v & (AXI_IRQ_ENABLE | AXI_TIMEOUT);
		if (!(local->axi_irq & AXI_IRQ_ENABLE))
			local->axi_expired = false;
		axi_schedule(local);
		break;
	case REG_LOCAL_TIMER: {
		bool was_enabled = local->local_timer & LOCAL_TIMER_ENABLE;

		local->local_timer = v & (LOCAL_TIMER_IRQ_ENABLE | LOCAL_TIMER_ENABLE |
		                          LOCAL_TIMER_RELOAD);
		if (!was_enabled)
			local_timer_reload(local);
		local_timer_schedule(local);
		break;
	}
	case REG_LOCAL_TIMER_FLAGS:
		if (v & LOCAL_TIMER_FLAG)
			local->local_flag = false;
		if (v & LOCAL_TIMER_RELOAD_NOW)
			local_timer_reload(local);
		local_timer_schedule(local);
		break;
	default:
		if (offset >= REG_MAILBOX_CLEAR)
			local->mailbox[(offset - REG_MAILBOX_CLEAR) >> 2] &= ~v;
		else if (offset >= REG_MAILBOX_SET)
			local->mailbox[(offset - REG_MAILBOX_SET) >> 2] |= v;
		else if (offset >= REG_IRQ_SOURCE)
			break; /* read-only */
		else if (offset >= REG_MAILBOX_ROUTING)
			local->mailbox_routing[index] = v & 0xff;
		else if (offset >= REG_TIMER_ROUTING)
			local->timer_routing[index] = v & 0xff;
		/* the rest are read-only or unused */
		break;
	}
	update(local);
}

static const BusMmioOps local_mmio_ops = {
	.read = local_read,
	.write = local_write,
};

Bcm2836Local *bcm2836_local_create(Machine *m, const Bcm2836LocalConfig *config)
{
	Bcm2836Local *local = calloc(1, sizeof(*local));

	if (local == NULL)
		return NULL;
	local->machine = m;
	local->config = *config;
	system_counter_init(&local->counter, &counter_ops, local);
	for (unsigned core = 0; core < BCM2836_LOCAL_NUM_CORES; core++)
		for (unsigned i = 0; i < BCM2836_LOCAL_NUM_INPUTS; i++)
			irq_init(&local->inputs[core][i], input_changed, local,
			         8 * core + i);
	irq_init(&local->gpu_irq, input_changed, local, LINE_GPU_IRQ);
	irq_init(&local->gpu_fiq, input_changed, local, LINE_GPU_FIQ);
	event_init(&local->local_event, local_timer_fired, local);
	event_init(&local->axi_event, axi_fired, local);
	local->quiet = m->idle;
	local->quiet_since = machine_now(m);
	machine_set_idle_listener(m, idle_changed, local);
	bcm2836_local_reset(local);
	return local;
}

void bcm2836_local_reset(Bcm2836Local *local)
{
	uint64_t now = machine_now(local->machine);
	uint64_t before = counter_read(local, now);

	/* Sections 4.2 and 4.3: these reset to 0, so the timer is stopped. */
	local->control = 0;
	local->prescaler = 0;
	local->base_time = now;
	local->base_count = 0;
	local->base_acc = 0;
	local->read_hold = 0;
	local->write_hold = 0;
	system_counter_changed(&local->counter, before, 0);

	/*
	 * Section 3.2: every routing bit resets to 0. The mailboxes, the
	 * local timer and the AXI interrupt reset to 0 too (4.8, 4.9, 4.11).
	 */
	local->gpu_routing = 0;
	local->pmu_routing = 0;
	local->local_routing = 0;
	for (unsigned core = 0; core < BCM2836_LOCAL_NUM_CORES; core++) {
		local->timer_routing[core] = 0;
		local->mailbox_routing[core] = 0;
	}
	for (unsigned i = 0; i < NUM_MAILBOXES; i++)
		local->mailbox[i] = 0;
	local->local_timer = 0;
	local->local_flag = false;
	local->local_next_zero = 0;
	local->local_time = now;
	local->axi_irq = 0;
	local->axi_expired = false;
	local_timer_schedule(local);
	axi_schedule(local);
	update(local);
}

void bcm2836_local_destroy(Bcm2836Local *local)
{
	EventQueue *q = &local->machine->events;

	event_cancel(q, &local->local_event);
	event_cancel(q, &local->axi_event);
	if (local->machine->idle_listener == idle_changed)
		machine_set_idle_listener(local->machine, NULL, NULL);
	free(local);
}

BusStatus bcm2836_local_map(Bcm2836Local *local, Bus *bus, const char *name,
                            uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2836_LOCAL_MMIO_SIZE,
	                    &local_mmio_ops, local);
}

SystemCounter *bcm2836_local_counter(Bcm2836Local *local)
{
	return &local->counter;
}

IrqLine *bcm2836_local_input(Bcm2836Local *local, unsigned core,
                             Bcm2836LocalInput input)
{
	return &local->inputs[core][input];
}

IrqLine *bcm2836_local_gpu_input(Bcm2836Local *local, bool fiq)
{
	return fiq ? &local->gpu_fiq : &local->gpu_irq;
}

void bcm2836_local_connect(Bcm2836Local *local, unsigned core, IrqLine *irq,
                           IrqLine *fiq)
{
	uint32_t irq_sources, fiq_sources;

	local->irq_out[core] = irq;
	local->fiq_out[core] = fiq;
	sources(local, core, &irq_sources, &fiq_sources);
	irq_set(irq, irq_sources != 0);
	irq_set(fiq, fiq_sources != 0);
}
