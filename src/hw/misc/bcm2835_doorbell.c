/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The doorbells between the Arm and the VideoCore; see the header. */

#include "hw/misc/bcm2835_doorbell.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

enum { REG_BELL0 = 0x00, REG_BELL2 = 0x08 };

#define BELL0_RUNG (1u << 2) /* Linux's ARM_DS_ACTIVE */

struct Bcm2835Doorbell {
	Machine *machine;
	IrqLine *irq;
	Bcm2835DoorbellVc vc;
	bool rung; /* the Arm's doorbell 0 */
};

static void stop(Bcm2835Doorbell *db, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(db->machine, MACHINE_STOP_ERROR,
	             "bcm2835-doorbell: %u-byte %s offset 0x%02" PRIx64 " %s", size,
	             access, offset, why);
}

static void set_rung(Bcm2835Doorbell *db, bool rung)
{
	db->rung = rung;
	irq_set(db->irq, rung);
}

static uint64_t doorbell_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Doorbell *db = ctx;
	bool rung = db->rung;

	if (size != 4 || offset != REG_BELL0) {
		stop(db, "read of", offset, size,
		     "is not emulated: only BELL0 is read, in 32 bits");
		return 0;
	}
	set_rung(db, false);
	return rung ? BELL0_RUNG : 0;
}

static void doorbell_write(void *ctx, uint64_t offset, uint64_t value,
                           unsigned size)
{
	Bcm2835Doorbell *db = ctx;

	(void)value;
	if (size != 4 || offset != REG_BELL2) {
		stop(db, "write to", offset, size,
		     "is not emulated: only BELL2 is written, in 32 bits");
		return;
	}
	db->vc.ring(db->vc.ctx);
}

static const BusMmioOps doorbell_mmio_ops = {
	.read = doorbell_read,
	.write = doorbell_write,
};

Bcm2835Doorbell *bcm2835_doorbell_create(Machine *m, IrqLine *irq,
                                         const Bcm2835DoorbellVc *vc)
{
	Bcm2835Doorbell *db = calloc(1, sizeof(*db));

	if (db == NULL)
		return NULL;
	db->machine = m;
	db->irq = irq;
	db->vc = *vc;
	return db;
}

void bcm2835_doorbell_destroy(Bcm2835Doorbell *db)
{
	free(db);
}

void bcm2835_doorbell_reset(Bcm2835Doorbell *db)
{
	set_rung(db, false);
}

BusStatus bcm2835_doorbell_map(Bcm2835Doorbell *db, Bus *bus, const char *name,
                               uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_DOORBELL_MMIO_SIZE,
	                    &doorbell_mmio_ops, db);
}

void bcm2835_doorbell_ring(Bcm2835Doorbell *db)
{
	set_rung(db, true);
}
