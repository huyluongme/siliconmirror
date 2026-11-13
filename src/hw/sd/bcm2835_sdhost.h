/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * SDHOST, the BCM2835's own SD card controller, which the Raspberry Pi 3
 * drives its SD card with (the Arasan controller, "EMMC", goes to the
 * WiFi chip).
 *
 * No datasheet describes it. The registers and their bits are those
 * that the drivers use: Raspberry Pi's drivers/mmc/host/bcm2835-sdhost.c
 * in its Linux, written by Raspberry Pi, Linux's bcm2835.c and U-Boot's
 * bcm2835_sdhost.c, which come from it. What they do not show stops the
 * machine.
 *
 * The SD clock is the core clock / (SDCDIV + 2) with SLOW_CARD set, as
 * every driver sets it; without it, in data mode, only the low 3 bits of
 * SDCDIV count (from the driver's comments). Commands, responses and
 * data take their bits' time on the bus: 1 or 4 data lines, as
 * WIDE_EXT_BUS says. Data goes through a FIFO of 16 words, which the
 * Arm reads and writes through SDDATA; the clock stops while it is full
 * on a read or empty on a write. Timeouts count SD clocks: SDTOUT for
 * data and busy, 64 for a response (Ncr, SD Physical Layer
 * specification, 4.12). CRC errors never happen.
 *
 * The DMA request is not modelled yet.
 */

#ifndef HW_SD_BCM2835_SDHOST_H
#define HW_SD_BCM2835_SDHOST_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/sd/sd_card.h"

#define BCM2835_SDHOST_MMIO_SIZE 0x100
#define BCM2835_SDHOST_FIFO_WORDS 16

typedef struct Bcm2835Sdhost Bcm2835Sdhost;

/*
 * Where the card is: card, or NULL for none; connected tells whether the
 * pins between the controller and the card carry the controller's
 * signals now, or is NULL for always.
 */
typedef struct Bcm2835SdhostSlot {
	SdCard *card;
	bool (*connected)(void *ctx);
	void *ctx;
} Bcm2835SdhostSlot;

/*
 * Create the controller in its reset state, with a core clock of
 * core_hz. It takes the time from m and stops m on what it does not
 * emulate. irq is its interrupt, or NULL. Returns NULL when core_hz is 0
 * or memory runs out.
 */
Bcm2835Sdhost *bcm2835_sdhost_create(Machine *m, uint64_t core_hz,
                                     const Bcm2835SdhostSlot *slot,
                                     IrqLine *irq);
void bcm2835_sdhost_destroy(Bcm2835Sdhost *host);

void bcm2835_sdhost_reset(Bcm2835Sdhost *host);

BusStatus bcm2835_sdhost_map(Bcm2835Sdhost *host, Bus *bus, const char *name,
                             uint64_t base);

#endif /* HW_SD_BCM2835_SDHOST_H */
