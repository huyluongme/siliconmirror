/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The ARM interrupt controller of the BCM2835, BCM2836 and BCM2837, at
 * 0x7E00B200 on the VideoCore bus: chapter 7 of Broadcom's "BCM2835 ARM
 * Peripherals". It takes the 64 interrupts of the GPU's peripherals and
 * the 8 of the ARM's own, and gives the ARM one IRQ and one FIQ. On the
 * BCM2836 and BCM2837, these go to the ARM local peripherals, which
 * route them to a core.
 *
 * Accesses other than 32-bit ones, and those to the rest of the block,
 * which the document does not describe, stop the machine as not
 * implemented.
 */

#ifndef HW_INTC_BCM2835_IC_H
#define HW_INTC_BCM2835_IC_H

#include <stdint.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"

/* Size of the register block, from 0x7E00B200 to the ARM timer. */
#define BCM2835_IC_MMIO_SIZE 0x200

#define BCM2835_IC_NUM_GPU 64 /* GPU interrupts, 0 to 63 */
#define BCM2835_IC_NUM_ARM 8  /* ARM interrupts, 0 to 7 */

/* A GPU interrupt, from the table of section 7.5. */
#define BCM2835_IC_USB 9     /* the USB controller (bcm283x.dtsi) */
#define BCM2835_IC_AUX 29    /* the mini UART and the SPI masters */
#define BCM2835_IC_GPIO0 49  /* GPIO bank 0; 50 and 51 for 1 and 2, 52 all */
#define BCM2835_IC_SDHOST 56 /* the SD card controller */
#define BCM2835_IC_UART 57   /* the PL011, UART0 */
#define BCM2835_IC_EMMC 62   /* the Arasan SD controller (bcm2710.dtsi) */

/* ARM interrupts, from the table of section 7.5. */
enum {
	BCM2835_IC_ARM_TIMER,
	BCM2835_IC_ARM_MAILBOX,
	BCM2835_IC_ARM_DOORBELL0,
	BCM2835_IC_ARM_DOORBELL1,
	BCM2835_IC_GPU0_HALTED,
	BCM2835_IC_GPU1_HALTED,
	BCM2835_IC_ILLEGAL_ACCESS1,
	BCM2835_IC_ILLEGAL_ACCESS0,
};

typedef struct Bcm2835Ic Bcm2835Ic;

/*
 * Create the controller in its reset state. It stops m on accesses it
 * does not implement. Returns NULL when out of memory.
 */
Bcm2835Ic *bcm2835_ic_create(Machine *m);
void bcm2835_ic_reset(Bcm2835Ic *ic);
void bcm2835_ic_destroy(Bcm2835Ic *ic);

/* Map the registers at base, the address of 0x7E00B200. */
BusStatus bcm2835_ic_map(Bcm2835Ic *ic, Bus *bus, const char *name,
                         uint64_t base);

/* The input of GPU interrupt n, 0 to 63, or of ARM interrupt n, 0 to 7. */
IrqLine *bcm2835_ic_gpu_input(Bcm2835Ic *ic, unsigned n);
IrqLine *bcm2835_ic_arm_input(Bcm2835Ic *ic, unsigned n);

/*
 * Connect the IRQ and FIQ outputs, or disconnect them with NULL. The
 * lines take the outputs' levels.
 */
void bcm2835_ic_connect(Bcm2835Ic *ic, IrqLine *irq, IrqLine *fiq);

#endif /* HW_INTC_BCM2835_IC_H */
