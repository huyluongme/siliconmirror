/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The BCM2835's MPHI, as dwc_otg uses it; see bcm2835_mphi.h. */

#include "hw/misc/bcm2835_mphi.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

enum {
	REG_OUTDDA = 0x28,
	REG_OUTDDB = 0x2c,
	REG_CTRL = 0x4c,
	REG_INTSTAT = 0x50,
};

#define OUTDDB_SEND (1u << 29)
#define CTRL_ENABLE (1u << 31)
#define CTRL_RESET (1u << 16)
#define CTRL_RESET_DONE (1u << 17)
#define INTSTAT_IRQ (1u << 16)
#define INTSTAT_SENT (1u << 29)

struct Bcm2835Mphi {
	Machine *machine;
	IrqLine *irq;
	uint32_t outdda;
	uint32_t ctrl; /* ENABLE and RESET, as written */
	uint32_t intstat;
};

static void stop(Bcm2835Mphi *mphi, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(mphi->machine, MACHINE_STOP_ERROR,
	             "bcm2835-mphi: %u-byte %s offset 0x%03" PRIx64 " %s", size,
	             access, offset, why);
}

static void update_irq(Bcm2835Mphi *mphi)
{
	irq_set(mphi->irq, mphi->intstat & INTSTAT_IRQ);
}

static uint64_t mphi_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Mphi *mphi = ctx;

	if (size != 4 || offset % 4 != 0) {
		stop(mphi, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	switch (offset) {
	case REG_OUTDDA:
		return mphi->outdda;
	case REG_CTRL:
		/* A reset is done as soon as it is asked. */
		return mphi->ctrl | (mphi->ctrl & CTRL_RESET ? CTRL_RESET_DONE : 0);
	case REG_INTSTAT:
		return mphi->intstat;
	default:
		stop(mphi, "read of", offset, size,
		     "is not emulated: dwc_otg does not read it");
		return 0;
	}
}

static void mphi_write(void *ctx, uint64_t offset, uint64_t value,
                       unsigned size)
{
	Bcm2835Mphi *mphi = ctx;
	uint32_t v = (uint32_t)value;

	if (size != 4 || offset % 4 != 0) {
		stop(mphi, "write to", offset, size, "is not a 32-bit register write");
		return;
	}
	switch (offset) {
	case REG_OUTDDA:
		mphi->outdda = v;
		return;
	case REG_OUTDDB:
		if (v != OUTDDB_SEND) {
			stop(mphi, "write to", offset, size,
			     "is not emulated: only a send, bit 29");
			return;
		}
		if (!(mphi->ctrl & CTRL_ENABLE)) {
			stop(mphi, "write to", offset, size,
			     "is not emulated: a send while the block is disabled");
			return;
		}
		mphi->intstat |= INTSTAT_IRQ | INTSTAT_SENT;
		update_irq(mphi);
		return;
	case REG_CTRL:
		if (v & ~(CTRL_ENABLE | CTRL_RESET)) {
			stop(mphi, "write to", offset, size,
			     "is not emulated: bits other than enable and reset");
			return;
		}
		mphi->ctrl = v;
		return;
	case REG_INTSTAT:
		if (v & ~(INTSTAT_IRQ | INTSTAT_SENT)) {
			stop(mphi, "write to", offset, size,
			     "is not emulated: bits other than 16 and 29");
			return;
		}
		mphi->intstat &= ~v;
		update_irq(mphi);
		return;
	default:
		stop(mphi, "write to", offset, size,
		     "is not emulated: dwc_otg does not write it");
		return;
	}
}

static const BusMmioOps mphi_mmio_ops = {
	.read = mphi_read,
	.write = mphi_write,
};

Bcm2835Mphi *bcm2835_mphi_create(Machine *m, IrqLine *irq)
{
	Bcm2835Mphi *mphi = calloc(1, sizeof(*mphi));

	if (mphi == NULL)
		return NULL;
	mphi->machine = m;
	mphi->irq = irq;
	return mphi;
}

void bcm2835_mphi_destroy(Bcm2835Mphi *mphi)
{
	free(mphi);
}

void bcm2835_mphi_reset(Bcm2835Mphi *mphi)
{
	mphi->outdda = 0;
	mphi->ctrl = 0;
	mphi->intstat = 0;
	update_irq(mphi);
}

BusStatus bcm2835_mphi_map(Bcm2835Mphi *mphi, Bus *bus, const char *name,
                           uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_MPHI_MMIO_SIZE, &mphi_mmio_ops,
	                    mphi);
}
