/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The BCM2835's temperature sensor; see bcm2835_thermal.h. */

#include "hw/misc/bcm2835_thermal.h"

#include <inttypes.h>
#include <stdlib.h>

#define REG_TSENSCTL 0x0 /* and TSENSSTAT at 0x4 */

#define TSENSSTAT_VALID (1u << 10)

struct Bcm2835Thermal {
	Machine *machine;
};

static void stop(Bcm2835Thermal *t, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(t->machine, MACHINE_STOP_ERROR,
	             "bcm2835-thermal: %u-byte %s offset 0x%" PRIx64 " %s", size,
	             access, offset, why);
}

static uint64_t thermal_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Thermal *t = ctx;

	if (size != 4 || offset % 4 != 0) {
		stop(t, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	if (offset == REG_TSENSCTL)
		return BCM2835_THERMAL_CTL_BOOT;
	return TSENSSTAT_VALID | BCM2835_THERMAL_ADC;
}

static void thermal_write(void *ctx, uint64_t offset, uint64_t value,
                          unsigned size)
{
	(void)value;
	stop(ctx, "write to", offset, size,
	     "is not emulated: the firmware set the sensor up");
}

static const BusMmioOps thermal_mmio_ops = {
	.read = thermal_read,
	.write = thermal_write,
};

Bcm2835Thermal *bcm2835_thermal_create(Machine *m)
{
	Bcm2835Thermal *t = calloc(1, sizeof(*t));

	if (t == NULL)
		return NULL;
	t->machine = m;
	return t;
}

void bcm2835_thermal_destroy(Bcm2835Thermal *t)
{
	free(t);
}

BusStatus bcm2835_thermal_map(Bcm2835Thermal *t, Bus *bus, const char *name,
                              uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_THERMAL_MMIO_SIZE,
	                    &thermal_mmio_ops, t);
}
