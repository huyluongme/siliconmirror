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
	REG_PRESCALER = 0x08,
	REG_TIMER_LS = 0x1c,
	REG_TIMER_MS = 0x20,
};

#define CONTROL_APB (1u << 8)   /* the timer runs from the APB clock */
#define CONTROL_STEP2 (1u << 9) /* the timer increments by 2 */
#define PRESCALER_ONE (UINT32_C(1) << 31)
#define NS_PER_SECOND UINT64_C(1000000000)

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
};

static uint64_t clock_hz(const Bcm2836Local *local)
{
	return local->control & CONTROL_APB ? local->config.apb_hz
	                                    : local->config.crystal_hz;
}

/* Edges of a clock of hz Hz from time 0 up to time t, both included. */
static uint64_t edges_at(uint64_t t, uint64_t hz)
{
	return mul_div_floor(t, hz, NS_PER_SECOND);
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

	if (size == 4) {
		switch (offset) {
		case REG_CONTROL:
			return local->control;
		case REG_PRESCALER:
			return local->prescaler;
		case REG_TIMER_LS: {
			uint64_t count = counter_read(local, machine_now(local->machine));

			local->read_hold = (uint32_t)(count >> 32);
			return (uint32_t)count;
		}
		case REG_TIMER_MS:
			return local->read_hold;
		default:
			break;
		}
	}
	not_implemented(local, "read from", offset, size);
	return 0;
}

static void local_write(void *ctx, uint64_t offset, uint64_t value,
                        unsigned size)
{
	Bcm2836Local *local = ctx;
	uint64_t now = machine_now(local->machine);
	uint64_t before;

	if (size != 4) {
		not_implemented(local, "write to", offset, size);
		return;
	}
	switch (offset) {
	case REG_CONTROL:
		rebase(local, now);
		local->control = (uint32_t)value & (CONTROL_APB | CONTROL_STEP2);
		system_counter_changed(&local->counter, local->base_count,
		                       local->base_count);
		return;
	case REG_PRESCALER:
		if (value > PRESCALER_ONE) {
			machine_stop(local->machine, MACHINE_STOP_ERROR,
			             "bcm2836-local: prescaler 0x%08" PRIx64
			             " is above 2^31, which the chip's document says "
			             "not to use",
			             value);
			return;
		}
		rebase(local, now);
		local->prescaler = (uint32_t)value;
		system_counter_changed(&local->counter, local->base_count,
		                       local->base_count);
		return;
	case REG_TIMER_LS:
		local->write_hold = (uint32_t)value;
		return;
	case REG_TIMER_MS:
		rebase(local, now);
		before = local->base_count;
		local->base_count = value << 32 | local->write_hold;
		local->base_acc = 0;
		system_counter_changed(&local->counter, before, local->base_count);
		return;
	default:
		not_implemented(local, "write to", offset, size);
		return;
	}
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
	bcm2836_local_reset(local);
	return local;
}

void bcm2836_local_reset(Bcm2836Local *local)
{
	uint64_t before = counter_read(local, machine_now(local->machine));

	/* Sections 4.2 and 4.3: these reset to 0, so the timer is stopped. */
	local->control = 0;
	local->prescaler = 0;
	local->base_time = machine_now(local->machine);
	local->base_count = 0;
	local->base_acc = 0;
	local->read_hold = 0;
	local->write_hold = 0;
	system_counter_changed(&local->counter, before, 0);
}

void bcm2836_local_destroy(Bcm2836Local *local)
{
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
