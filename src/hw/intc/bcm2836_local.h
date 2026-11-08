/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The ARM local peripherals of the BCM2836 and BCM2837, at 0x40000000:
 * Broadcom's document "BCM2836 ARM-local peripherals" (Quad-A7 control,
 * rev 3.4). The block holds the 64-bit core timer, which is the system
 * counter of the four cores' Generic Timers (section 3.1), and routes
 * interrupts and mailboxes to the cores.
 *
 * Implemented so far: the core timer, with the control register, the
 * prescaler and the 64-bit timer registers. Any other access stops the
 * machine, as something not implemented yet.
 */

#ifndef HW_INTC_BCM2836_LOCAL_H
#define HW_INTC_BCM2836_LOCAL_H

#include <stdint.h>

#include "core/bus.h"
#include "core/counter.h"
#include "core/machine.h"

/* Size of the register block. */
#define BCM2836_LOCAL_MMIO_SIZE 0x100

typedef struct Bcm2836LocalConfig {
	/* The crystal clock, which the core timer runs from by default. */
	uint64_t crystal_hz;
	/* The APB clock, half the ARM clock, which control bit 8 selects. */
	uint64_t apb_hz;
} Bcm2836LocalConfig;

typedef struct Bcm2836Local Bcm2836Local;

/*
 * Create the block in its reset state. It reads the time from m and
 * stops m on accesses it does not implement. Returns NULL when out of
 * memory.
 */
Bcm2836Local *bcm2836_local_create(Machine *m,
                                   const Bcm2836LocalConfig *config);

void bcm2836_local_reset(Bcm2836Local *local);
void bcm2836_local_destroy(Bcm2836Local *local);

/* Map the registers at base. */
BusStatus bcm2836_local_map(Bcm2836Local *local, Bus *bus, const char *name,
                            uint64_t base);

/* The core timer, as the system counter to give the cores. */
SystemCounter *bcm2836_local_counter(Bcm2836Local *local);

#endif /* HW_INTC_BCM2836_LOCAL_H */
