/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835's MPHI, its message-based parallel host interface, at
 * 0x7E006000. Broadcom does not document it. On the Raspberry Pi, the
 * USB driver dwc_otg uses it only to raise an interrupt of its own: its
 * fast handler, which takes the USB controller's interrupt, "kicks" its
 * main handler through the MPHI, whose interrupt the board's device tree
 * gives the USB node as "soft", GPU interrupt 32. Its registers behave
 * here as dwc_otg (dwc_otg_hcd_linux.c, dwc_otg_fiq_fsm.c,
 * dwc_otg_hcd_intr.c) uses them:
 *   OUTDDA, 0x28: the bus address of the data to send.
 *   OUTDDB, 0x2c: writing bit 29 sends it, which ends with the interrupt.
 *   CTRL, 0x4c: bit 31 enables the block; bit 16 resets it, and bit 17
 *     says, read, that the reset is done.
 *   INTSTAT, 0x50: bit 16, the interrupt, and bit 29, a send done; a 1
 *     written clears each.
 *
 * smirror's choices, where nothing documents the block: a send ends at
 * once, sets both bits of INTSTAT and raises the interrupt, which stays
 * until bit 16 is cleared; the data is not read, as nothing receives it;
 * a reset is done at once and changes nothing else.
 *
 * Not modelled, and the machine stops when software uses them: other
 * registers and bits, a send while the block is disabled, reads of
 * OUTDDB.
 */

#ifndef HW_MISC_BCM2835_MPHI_H
#define HW_MISC_BCM2835_MPHI_H

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"

#define BCM2835_MPHI_MMIO_SIZE 0x1000 /* the device tree's */

typedef struct Bcm2835Mphi Bcm2835Mphi;

/*
 * The block at power on, disabled, raising irq when a send ends. NULL
 * when memory runs out.
 */
Bcm2835Mphi *bcm2835_mphi_create(Machine *m, IrqLine *irq);
void bcm2835_mphi_destroy(Bcm2835Mphi *mphi);

/* A reset of the board: disabled, no interrupt. */
void bcm2835_mphi_reset(Bcm2835Mphi *mphi);

/* Map the registers on bus at base. */
BusStatus bcm2835_mphi_map(Bcm2835Mphi *mphi, Bus *bus, const char *name,
                           uint64_t base);

#endif /* HW_MISC_BCM2835_MPHI_H */
