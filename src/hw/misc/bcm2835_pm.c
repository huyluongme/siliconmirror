/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The BCM2835's power manager and its watchdog; see bcm2835_pm.h. */

#include "hw/misc/bcm2835_pm.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "core/event.h"
#include "util/muldiv.h"

/* The registers that the drivers use. */
enum { REG_RSTC = 0x1c, REG_RSTS = 0x20, REG_WDOG = 0x24 };

#define PASSWORD_MASK 0xff000000u
#define PASSWORD 0x5a000000u
#define DATA_MASK 0x00ffffffu  /* what the registers keep */
#define WDOG_TICKS 0x000fffffu /* the count, 20 bits */
#define WRCFG_MASK 0x00000030u /* RSTC: the kind of watchdog reset */
#define WRCFG_FULL 0x00000020u
#define TICKS_PER_S 65536u /* Linux and U-Boot: a second is x << 16 */
#define NS_PER_S 1000000000u

struct Bcm2835Pm {
	Machine *machine;
	uint32_t rstc, rsts;
	uint32_t wdog;     /* the count when it was written or stopped */
	uint64_t armed_at; /* when it started to run down, if it runs */
	Event expire;
};

static void stop(Bcm2835Pm *pm, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(pm->machine, MACHINE_STOP_ERROR,
	             "bcm2835-pm: %u-byte %s offset 0x%03" PRIx64 " %s", size,
	             access, offset, why);
}

static bool running(const Bcm2835Pm *pm)
{
	return (pm->rstc & WRCFG_MASK) == WRCFG_FULL;
}

static uint64_t now(const Bcm2835Pm *pm)
{
	return machine_now(pm->machine);
}

/* The count left: whole ticks, rounded up, until the reset. */
static uint32_t ticks_left(const Bcm2835Pm *pm)
{
	uint64_t gone, left;

	if (!running(pm))
		return pm->wdog;
	gone = mul_div_floor(now(pm) - pm->armed_at, TICKS_PER_S, NS_PER_S);
	left = gone < pm->wdog ? pm->wdog - gone : 0;
	return (uint32_t)left;
}

static void expire(void *ctx)
{
	Bcm2835Pm *pm = ctx;

	pm->wdog = 0;
	pm->rstc &= ~WRCFG_MASK;
	machine_request_reset(pm->machine);
}

/* Start the count down from pm->wdog, now. */
static void arm(Bcm2835Pm *pm)
{
	pm->armed_at = now(pm);
	event_schedule(&pm->machine->events, &pm->expire,
	               pm->armed_at +
	                   mul_div_ceil(pm->wdog, NS_PER_S, TICKS_PER_S));
}

static uint64_t pm_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Pm *pm = ctx;

	if (size != 4 || offset % 4 != 0) {
		stop(pm, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	switch (offset) {
	case REG_RSTC:
		return pm->rstc;
	case REG_RSTS:
		return pm->rsts;
	case REG_WDOG:
		return ticks_left(pm);
	default:
		stop(pm, "read of", offset, size,
		     "is not emulated: power domains and reset lines");
		return 0;
	}
}

static void pm_write(void *ctx, uint64_t offset, uint64_t value, unsigned size)
{
	Bcm2835Pm *pm = ctx;
	uint32_t v = (uint32_t)value, left;
	bool was = running(pm);

	if (size != 4 || offset % 4 != 0) {
		stop(pm, "write to", offset, size, "is not a 32-bit register write");
		return;
	}
	if (offset != REG_RSTC && offset != REG_RSTS && offset != REG_WDOG) {
		stop(pm, "write to", offset, size,
		     "is not emulated: power domains and reset lines");
		return;
	}
	if ((v & PASSWORD_MASK) != PASSWORD)
		return; /* the password guards every register */
	switch (offset) {
	case REG_RSTC:
		if ((v & WRCFG_MASK) != 0 && (v & WRCFG_MASK) != WRCFG_FULL) {
			stop(pm, "write to", offset, size,
			     "is not emulated: a watchdog reset other than full");
			return;
		}
		left = ticks_left(pm);
		pm->rstc = v & DATA_MASK;
		if (was && !running(pm)) {
			pm->wdog = left; /* the count stops where it is */
			event_cancel(&pm->machine->events, &pm->expire);
		} else if (!was && running(pm)) {
			arm(pm);
		}
		return;
	case REG_RSTS:
		pm->rsts = v & DATA_MASK;
		return;
	default: /* REG_WDOG: a new count, from now if it runs */
		pm->wdog = v & WDOG_TICKS;
		if (running(pm))
			arm(pm);
		return;
	}
}

static const BusMmioOps pm_mmio_ops = {
	.read = pm_read,
	.write = pm_write,
};

Bcm2835Pm *bcm2835_pm_create(Machine *m)
{
	Bcm2835Pm *pm = calloc(1, sizeof(*pm));

	if (pm == NULL)
		return NULL;
	pm->machine = m;
	event_init(&pm->expire, expire, pm);
	return pm;
}

void bcm2835_pm_destroy(Bcm2835Pm *pm)
{
	event_cancel(&pm->machine->events, &pm->expire);
	free(pm);
}

void bcm2835_pm_reset(Bcm2835Pm *pm)
{
	event_cancel(&pm->machine->events, &pm->expire);
	pm->rstc = 0;
	pm->wdog = 0;
}

BusStatus bcm2835_pm_map(Bcm2835Pm *pm, Bus *bus, const char *name,
                         uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_PM_MMIO_SIZE, &pm_mmio_ops,
	                    pm);
}

unsigned bcm2835_pm_partition(const Bcm2835Pm *pm)
{
	unsigned partition = 0;

	/* Bits 0, 2, 4, 6, 8 and 10 (Linux's bcm2835_wdt.c). */
	for (unsigned i = 0; i < 6; i++)
		partition |= ((pm->rsts >> (2 * i)) & 1u) << i;
	return partition;
}
