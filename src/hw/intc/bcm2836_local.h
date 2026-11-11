/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The ARM local peripherals of the BCM2836 and BCM2837, at 0x40000000:
 * Broadcom's document "BCM2836 ARM-local peripherals" (Quad-A7 control,
 * rev 3.4). The block holds the 64-bit core timer, which is the system
 * counter of the four cores' Generic Timers (section 3.1), routes the
 * interrupts of the cores' timers and performance monitors, of the GPU
 * and of the block itself to the IRQ and FIQ inputs of the cores (3.2),
 * and has the cores' mailboxes (3.3), a local timer (4.11) and the AXI
 * outstanding counters (4.9).
 *
 * Accesses other than 32-bit ones, and the two registers whose contents
 * the document does not give, stop the machine as not implemented.
 */

#ifndef HW_INTC_BCM2836_LOCAL_H
#define HW_INTC_BCM2836_LOCAL_H

#include <stdint.h>

#include "core/bus.h"
#include "core/counter.h"
#include "core/irq.h"
#include "core/machine.h"

/* Size of the register block. */
#define BCM2836_LOCAL_MMIO_SIZE 0x100

#define BCM2836_LOCAL_NUM_CORES 4

/*
 * The interrupt inputs that belong to one core (section 3.2.1): the
 * outputs of its Generic Timer, in the order of the source registers,
 * and of its performance monitors.
 */
typedef enum Bcm2836LocalInput {
	BCM2836_LOCAL_CNTPS,  /* nCNTPSIRQ, the secure physical timer */
	BCM2836_LOCAL_CNTPNS, /* nCNTPNSIRQ, the non-secure physical timer */
	BCM2836_LOCAL_CNTHP,  /* nCNTHPIRQ, the hypervisor timer */
	BCM2836_LOCAL_CNTV,   /* nCNTVIRQ, the virtual timer */
	BCM2836_LOCAL_PMU,    /* nPMUIRQ */
	BCM2836_LOCAL_NUM_INPUTS,
} Bcm2836LocalInput;

typedef struct Bcm2836LocalConfig {
	/* The crystal clock, which the core timer runs from by default. */
	uint64_t crystal_hz;
	/* The APB clock, half the ARM clock, which control bit 8 selects. */
	uint64_t apb_hz;
} Bcm2836LocalConfig;

typedef struct Bcm2836Local Bcm2836Local;

/*
 * Create the block in its reset state. It reads the time from m, stops
 * m on accesses it does not implement, and becomes the idle listener of
 * m, which tells it when the AXI bus of the cores goes quiet. Returns
 * NULL when out of memory.
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

/* An interrupt input of a core, 0 to 3, for the core to drive. */
IrqLine *bcm2836_local_input(Bcm2836Local *local, unsigned core,
                             Bcm2836LocalInput input);

/* The IRQ, or FIQ, input from the GPU's interrupt controller. */
IrqLine *bcm2836_local_gpu_input(Bcm2836Local *local, bool fiq);

/*
 * Connect the IRQ and FIQ outputs for a core to its inputs, or
 * disconnect them with NULL. The lines take the outputs' levels.
 */
void bcm2836_local_connect(Bcm2836Local *local, unsigned core, IrqLine *irq,
                           IrqLine *fiq);

#endif /* HW_INTC_BCM2836_LOCAL_H */
