/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835's power manager (PM) at 0x7E100000: its watchdog, which
 * resets the board. Broadcom does not document the block. Its devicetree
 * binding (brcm,bcm2835-pm) says what it holds: power domains, reset
 * lines and a watchdog; its registers behave here as Linux's
 * bcm2835_wdt.c and U-Boot's bcm2835_wdt.c and reset.c use them.
 *
 * A write must carry the password 0x5A in bits 31:24, or it does
 * nothing. WDOG holds the watchdog's count, 20 bits of ticks at 65536 a
 * second. The count runs down while the WRCFG field of RSTC (bits 5:4)
 * asks for a full reset, 0b10; when it reaches 0 the whole board resets.
 * RSTS survives that reset: the Raspberry Pi firmware finds there, in
 * bits 0, 2, 4, 6, 8 and 10, the partition to boot from, 63 for a halt.
 *
 * Not modelled, and the machine stops when software uses them: the
 * power domains and reset lines (the other registers), and the other
 * kinds of watchdog reset, WRCFG 0b01 and 0b11.
 */

#ifndef HW_MISC_BCM2835_PM_H
#define HW_MISC_BCM2835_PM_H

#include <stdint.h>

#include "core/bus.h"
#include "core/machine.h"

#define BCM2835_PM_MMIO_SIZE 0x114 /* the binding's "pm" region */
#define BCM2835_PM_HALT_PARTITION 63

typedef struct Bcm2835Pm Bcm2835Pm;

/* The block at power on. It resets m. NULL when memory runs out. */
Bcm2835Pm *bcm2835_pm_create(Machine *m);
void bcm2835_pm_destroy(Bcm2835Pm *pm);

/* A reset of the board: the watchdog stops, RSTS stays. */
void bcm2835_pm_reset(Bcm2835Pm *pm);

/* Map the registers on bus at base. */
BusStatus bcm2835_pm_map(Bcm2835Pm *pm, Bus *bus, const char *name,
                         uint64_t base);

/* The partition that RSTS gives the firmware: 0 for the default. */
unsigned bcm2835_pm_partition(const Bcm2835Pm *pm);

#endif /* HW_MISC_BCM2835_PM_H */
