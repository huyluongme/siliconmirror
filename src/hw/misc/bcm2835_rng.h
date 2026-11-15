/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835's random number generator (RNG) at 0x7E104000. Broadcom
 * does not document it; its registers behave here as Linux's
 * bcm2835-rng.c uses them:
 *   CTRL, 0x0: bit 0, RBGEN, runs the generator.
 *   STATUS, 0x4: written before the generator runs, the count of its
 *     first numbers to throw away, the warm-up; read, bits 31:24 count
 *     the words in its FIFO, of 4 words (RNG_FIFO_WORDS).
 *   DATA, 0x8: the FIFO's next word.
 *
 * smirror's choices, where nothing documents the block: the generator
 * makes a word every 34 us, the rate the driver is tuned for
 * (RNG_US_PER_WORD), and waits while its FIFO is full; the warm-up counts
 * bits, made at that rate, 32 a word; STATUS reads 0 in its other bits.
 * The numbers come from SplitMix64, from the same seed at each reset, so
 * that a run repeats: a real board's differ each time.
 *
 * Not modelled, and the machine stops when software uses them: the
 * other bits of CTRL, the register at 0xc, a read of DATA with the FIFO
 * empty, a new warm-up while the generator runs. Its interrupt is not
 * raised; Linux masks it only on other chips, through a register past
 * the 16 bytes of the device tree's node.
 */

#ifndef HW_MISC_BCM2835_RNG_H
#define HW_MISC_BCM2835_RNG_H

#include "core/bus.h"
#include "core/machine.h"

#define BCM2835_RNG_MMIO_SIZE 0x10 /* the device tree's */

typedef struct Bcm2835Rng Bcm2835Rng;

/* The generator at power on, stopped. NULL when memory runs out. */
Bcm2835Rng *bcm2835_rng_create(Machine *m);
void bcm2835_rng_destroy(Bcm2835Rng *rng);

/* A reset of the board: stopped, its FIFO empty, its seed again. */
void bcm2835_rng_reset(Bcm2835Rng *rng);

/* Map the registers on bus at base. */
BusStatus bcm2835_rng_map(Bcm2835Rng *rng, Bus *bus, const char *name,
                          uint64_t base);

#endif /* HW_MISC_BCM2835_RNG_H */
