/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835's clock manager (CPRMAN) at 0x7E101000: the PLLs that
 * multiply the 19.2 MHz crystal, their output channels, and the clock
 * generators that divide those outputs for the peripherals.
 *
 * "BCM2835 ARM Peripherals" documents only the general purpose clocks
 * (section 6.3): a control register with the password 0x5A in bits
 * 31:24, MASH in bits 10:9, BUSY in bit 7, KILL in bit 5, ENAB in bit 4
 * and the source in bits 3:0, and a divisor register with DIVI in bits
 * 23:12 and DIVF in bits 11:0. The other registers, and the clocks they
 * belong to, come from Linux's clk-bcm2835.c, which drives them on the
 * real chip; the other generators are taken to work as the general
 * purpose ones do.
 *
 * What the ARM sees at boot is what the firmware leaves, and smirror
 * has no VideoCore: the reset state is that of the firmware. Where a
 * real Raspberry Pi 3 Model B shows it, the rates are those of the
 * board; the other values are this model's choice, documented in
 * bcm2835_cprman.c.
 *
 * Not modelled, and the machine stops when software uses them: the
 * frequency counter (CM_OSCCOUNT, CM_TCNTCTL, CM_TCNTCNT), and a change
 * of the rate of a clock that a modelled device runs from (the UART,
 * EMMC and VPU clocks), as those devices keep the rates they were made
 * with.
 */

#ifndef HW_MISC_BCM2835_CPRMAN_H
#define HW_MISC_BCM2835_CPRMAN_H

#include <stdint.h>

#include "core/bus.h"
#include "core/machine.h"

#define BCM2835_CPRMAN_MMIO_SIZE 0x2000 /* the device tree's "reg" */
#define BCM2835_CPRMAN_OSC_HZ 19200000

/* The clocks that modelled devices run from. */
typedef enum Bcm2835CprmanClock {
	BCM2835_CPRMAN_UART, /* UART0, the PL011 */
	BCM2835_CPRMAN_EMMC, /* the Arasan SD host */
	BCM2835_CPRMAN_VPU,  /* the core clock: mini UART, SDHOST */
	BCM2835_CPRMAN_NUM_CLOCKS
} Bcm2835CprmanClock;

typedef struct Bcm2835Cprman Bcm2835Cprman;

/* The block as the firmware leaves it. NULL when memory runs out. */
Bcm2835Cprman *bcm2835_cprman_create(Machine *m);
void bcm2835_cprman_destroy(Bcm2835Cprman *c);

/* A reset of the board: the firmware sets the clocks up again. */
void bcm2835_cprman_reset(Bcm2835Cprman *c);

/* Map the registers on bus at base. */
BusStatus bcm2835_cprman_map(Bcm2835Cprman *c, Bus *bus, const char *name,
                             uint64_t base);

/* The rate of a clock in Hz: 0 while it is stopped. */
uint64_t bcm2835_cprman_rate(const Bcm2835Cprman *c, Bcm2835CprmanClock clock);

#endif /* HW_MISC_BCM2835_CPRMAN_H */
