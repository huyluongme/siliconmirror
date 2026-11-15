/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835's DMA controller, from chapter 4 of "BCM2835 ARM
 * Peripherals": 16 channels, 0 to 14 at 0x7E007000 and 15 at 0x7EE05000,
 * each of which runs a linked list of control blocks from memory.
 * Channels 7 to 14 are LITE engines (section 4.5): no 2D mode, no
 * ignore modes, a 16-bit length and half the bandwidth.
 *
 * The channels see the VideoCore's bus addresses: the peripherals at
 * 0x7E000000, and the RAM in each of its four aliases (0x00000000,
 * 0x40000000, 0x80000000, 0xC0000000). Transfers can be paced by the
 * DREQ signals of the peripherals (section 4.2.1.3), which the board
 * connects to the inputs that bcm2835_dma_dreq() returns.
 *
 * The interrupts are those of the device tree (bcm2835-common.dtsi):
 * channels 0 to 10 each have their own line, and channels 11 to 14
 * share one.
 */

#ifndef HW_DMA_BCM2835_DMA_H
#define HW_DMA_BCM2835_DMA_H

#include <stdint.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"

#define BCM2835_DMA_MMIO_SIZE 0x1000  /* channels 0 to 14, and globals */
#define BCM2835_DMA15_MMIO_SIZE 0x100 /* channel 15 */
#define BCM2835_DMA_NUM_CHANNELS 16
#define BCM2835_DMA_NUM_DREQS 32
#define BCM2835_DMA_NUM_IRQS 12 /* 0 to 10; 11 for channels 11-14 */
#define BCM2835_DMA_DREQ_SDHOST 13

typedef struct Bcm2835Dma Bcm2835Dma;

/*
 * The controller at reset, moving data on m's bus. irqs are its
 * interrupt outputs, each of which may be NULL. NULL when memory runs
 * out.
 */
Bcm2835Dma *bcm2835_dma_create(Machine *m,
                               IrqLine *const irqs[BCM2835_DMA_NUM_IRQS]);
void bcm2835_dma_destroy(Bcm2835Dma *dma);
void bcm2835_dma_reset(Bcm2835Dma *dma);

/* Map channels 0 to 14 and the global registers, and channel 15. */
BusStatus bcm2835_dma_map(Bcm2835Dma *dma, Bus *bus, const char *name,
                          uint64_t base, uint64_t base15);

/*
 * The input of DREQ n, 1 to 31, for the peripheral that drives it. A
 * transfer paced by a DREQ that nothing drives stops the machine.
 */
IrqLine *bcm2835_dma_dreq(Bcm2835Dma *dma, unsigned n);

#endif /* HW_DMA_BCM2835_DMA_H */
