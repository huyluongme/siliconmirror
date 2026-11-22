/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */
/* The BCM2835's HVS, with no display attached; see bcm2835_hvs.h. */

#include "hw/display/bcm2835_hvs.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* vc4_regs.h */
enum {
	REG_DISPCTRL = 0x00,
	REG_DISPSTAT = 0x04,
	REG_DISPECTRL = 0x0c,
	REG_DISPDITHER = 0x14,
	REG_DISPEOLN = 0x18,
	REG_DISPBASE0 = 0x4c,
	REG_DISPBASE1 = 0x5c,
	REG_DISPBASE2 = 0x6c,
	DLIST_START = 0x2000,
	DLIST_SIZE = 0x4000,
};

/* The registers that hold what is written, in this order. */
static const uint32_t kept_regs[] = {
	REG_DISPCTRL,  REG_DISPECTRL, REG_DISPDITHER, REG_DISPEOLN,
	REG_DISPBASE0, REG_DISPBASE1, REG_DISPBASE2,
};

#define NUM_KEPT (sizeof(kept_regs) / sizeof(kept_regs[0]))

struct Bcm2835Hvs {
	Machine *machine;
	uint32_t regs[NUM_KEPT];
	uint32_t dlist[DLIST_SIZE / 4];
};

static void stop(Bcm2835Hvs *hvs, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(hvs->machine, MACHINE_STOP_ERROR,
	             "bcm2835-hvs: %u-byte %s offset 0x%04" PRIx64 " %s", size,
	             access, offset, why);
}

/* The index of a kept register, or NUM_KEPT. */
static size_t kept(uint64_t offset)
{
	size_t i = 0;

	while (i < NUM_KEPT && kept_regs[i] != offset)
		i++;
	return i;
}

static bool in_dlist(uint64_t offset)
{
	return offset >= DLIST_START && offset < DLIST_START + DLIST_SIZE;
}

static uint64_t hvs_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Hvs *hvs = ctx;
	size_t i = kept(offset);

	if (size != 4 || offset % 4 != 0) {
		stop(hvs, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	if (in_dlist(offset))
		return hvs->dlist[(offset - DLIST_START) / 4];
	if (i < NUM_KEPT)
		return hvs->regs[i];
	if (offset == REG_DISPSTAT)
		return 0; /* no channel runs */
	stop(hvs, "read of", offset, size,
	     "is not emulated: no display is attached");
	return 0;
}

static void hvs_write(void *ctx, uint64_t offset, uint64_t value, unsigned size)
{
	Bcm2835Hvs *hvs = ctx;
	size_t i = kept(offset);

	if (size != 4 || offset % 4 != 0) {
		stop(hvs, "write to", offset, size, "is not a 32-bit register write");
		return;
	}
	if (in_dlist(offset))
		hvs->dlist[(offset - DLIST_START) / 4] = (uint32_t)value;
	else if (i < NUM_KEPT)
		hvs->regs[i] = (uint32_t)value;
	else if (offset != REG_DISPSTAT) /* no bit of it is set to clear */
		stop(hvs, "write to", offset, size,
		     "is not emulated: no display is attached");
}

static const BusMmioOps hvs_mmio_ops = {
	.read = hvs_read,
	.write = hvs_write,
};

Bcm2835Hvs *bcm2835_hvs_create(Machine *m)
{
	Bcm2835Hvs *hvs = calloc(1, sizeof(*hvs));

	if (hvs == NULL)
		return NULL;
	hvs->machine = m;
	return hvs;
}

void bcm2835_hvs_destroy(Bcm2835Hvs *hvs)
{
	free(hvs);
}

void bcm2835_hvs_reset(Bcm2835Hvs *hvs)
{
	memset(hvs->regs, 0, sizeof(hvs->regs));
	memset(hvs->dlist, 0, sizeof(hvs->dlist));
}

BusStatus bcm2835_hvs_map(Bcm2835Hvs *hvs, Bus *bus, const char *name,
                          uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_HVS_MMIO_SIZE, &hvs_mmio_ops,
	                    hvs);
}
