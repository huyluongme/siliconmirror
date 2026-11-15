/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The doorbells between the Arm and the VideoCore, at 0x7E00B840, which
 * VCHIQ rings: the board's device tree gives its vchiq node these
 * registers and ARM interrupt 2, the Arm's doorbell 0. Broadcom does not
 * document them; they behave here as Linux's vchiq_arm uses them:
 *   BELL0, 0x00: read, bit 2 says whether the VideoCore rang the Arm's
 *     doorbell since the last read; the read clears it, and with it the
 *     interrupt.
 *   BELL2, 0x08: written, rings the VideoCore's doorbell.
 *
 * Not modelled, and the machine stops when software uses them: the other
 * registers, writes of BELL0 and reads of BELL2.
 */

#ifndef HW_MISC_BCM2835_DOORBELL_H
#define HW_MISC_BCM2835_DOORBELL_H

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"

#define BCM2835_DOORBELL_MMIO_SIZE 0x3c /* the device tree's */

typedef struct Bcm2835Doorbell Bcm2835Doorbell;

/* The VideoCore's end: ring() is called when the Arm rings it. */
typedef struct Bcm2835DoorbellVc {
	void (*ring)(void *ctx);
	void *ctx;
} Bcm2835DoorbellVc;

/*
 * The doorbells, with irq the Arm's doorbell 0 interrupt. NULL when
 * memory runs out.
 */
Bcm2835Doorbell *bcm2835_doorbell_create(Machine *m, IrqLine *irq,
                                         const Bcm2835DoorbellVc *vc);
void bcm2835_doorbell_destroy(Bcm2835Doorbell *db);

/* A reset of the board: the Arm's doorbell is not rung. */
void bcm2835_doorbell_reset(Bcm2835Doorbell *db);

/* Map the registers on bus at base. */
BusStatus bcm2835_doorbell_map(Bcm2835Doorbell *db, Bus *bus, const char *name,
                               uint64_t base);

/* The VideoCore rings the Arm's doorbell 0. */
void bcm2835_doorbell_ring(Bcm2835Doorbell *db);

#endif /* HW_MISC_BCM2835_DOORBELL_H */
