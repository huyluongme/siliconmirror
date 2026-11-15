/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The BCM2835's random number generator; see bcm2835_rng.h. */

#include "hw/misc/bcm2835_rng.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

enum { REG_CTRL = 0x0, REG_STATUS = 0x4, REG_DATA = 0x8 };

#define RBGEN 0x1u        /* CTRL: the generator runs */
#define FIFO_WORDS 4u     /* Linux's RNG_FIFO_WORDS */
#define COUNT_SHIFT 24    /* STATUS: the words in the FIFO */
#define WORD_NS 34000u    /* a word every 34 us: RNG_US_PER_WORD */
#define BITS_PER_WORD 32u /* the warm-up's bits, at the words' rate */
#define SEED 0x534d4952u  /* "SMIR": the same numbers at each reset */
#define GOLDEN 0x9e3779b97f4a7c15ull /* SplitMix64's increment */

struct Bcm2835Rng {
	Machine *machine;
	bool running;
	uint32_t warmup;  /* bits to throw away when it starts */
	unsigned fifo;    /* words in the FIFO */
	uint64_t next_at; /* when the next word is made, if it runs */
	uint64_t state;   /* SplitMix64's */
};

static void stop(Bcm2835Rng *rng, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(rng->machine, MACHINE_STOP_ERROR,
	             "bcm2835-rng: %u-byte %s offset 0x%02" PRIx64 " %s", size,
	             access, offset, why);
}

/*
 * SplitMix64 (Steele, Lea and Flood, "Fast splittable pseudorandom number
 * generators", 2014, in Vigna's version): its next number's high half.
 */
static uint32_t next_number(Bcm2835Rng *rng)
{
	uint64_t z = (rng->state += GOLDEN);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return (uint32_t)((z ^ (z >> 31)) >> 32);
}

/* Fill the FIFO with the words made until now. */
static void update(Bcm2835Rng *rng)
{
	uint64_t now = machine_now(rng->machine);

	while (rng->running && rng->fifo < FIFO_WORDS && rng->next_at <= now) {
		rng->fifo++;
		rng->next_at += WORD_NS;
	}
}

static uint64_t rng_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Rng *rng = ctx;

	if (size != 4 || offset % 4 != 0) {
		stop(rng, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	update(rng);
	switch (offset) {
	case REG_CTRL:
		return rng->running;
	case REG_STATUS:
		return (uint64_t)rng->fifo << COUNT_SHIFT;
	case REG_DATA:
		if (rng->fifo == 0) {
			stop(rng, "read of", offset, size,
			     "is not emulated: the FIFO is empty");
			return 0;
		}
		/* A full FIFO held the generator: it starts a word now. */
		if (rng->fifo-- == FIFO_WORDS)
			rng->next_at = machine_now(rng->machine) + WORD_NS;
		return next_number(rng);
	default:
		stop(rng, "read of", offset, size,
		     "is not emulated: no driver uses it");
		return 0;
	}
}

static void rng_write(void *ctx, uint64_t offset, uint64_t value, unsigned size)
{
	Bcm2835Rng *rng = ctx;
	uint32_t v = (uint32_t)value;

	if (size != 4 || offset % 4 != 0) {
		stop(rng, "write to", offset, size, "is not a 32-bit register write");
		return;
	}
	update(rng);
	switch (offset) {
	case REG_CTRL:
		if (v & ~RBGEN) {
			stop(rng, "write to", offset, size,
			     "is not emulated: bits other than RBGEN");
			return;
		}
		if ((v & RBGEN) && !rng->running) {
			rng->next_at = machine_now(rng->machine) +
			               (uint64_t)rng->warmup * WORD_NS / BITS_PER_WORD +
			               WORD_NS;
		}
		rng->running = v & RBGEN;
		return;
	case REG_STATUS:
		if (rng->running) {
			stop(rng, "write to", offset, size,
			     "is not emulated: a warm-up while the generator runs");
			return;
		}
		rng->warmup = v;
		return;
	default:
		stop(rng, "write to", offset, size,
		     "is not emulated: no driver writes it");
		return;
	}
}

static const BusMmioOps rng_mmio_ops = {
	.read = rng_read,
	.write = rng_write,
};

Bcm2835Rng *bcm2835_rng_create(Machine *m)
{
	Bcm2835Rng *rng = calloc(1, sizeof(*rng));

	if (rng == NULL)
		return NULL;
	rng->machine = m;
	bcm2835_rng_reset(rng);
	return rng;
}

void bcm2835_rng_destroy(Bcm2835Rng *rng)
{
	free(rng);
}

void bcm2835_rng_reset(Bcm2835Rng *rng)
{
	rng->running = false;
	rng->warmup = 0;
	rng->fifo = 0;
	rng->state = SEED;
}

BusStatus bcm2835_rng_map(Bcm2835Rng *rng, Bus *bus, const char *name,
                          uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_RNG_MMIO_SIZE, &rng_mmio_ops,
	                    rng);
}
