/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835's DMA controller; see bcm2835_dma.h. Section numbers refer
 * to chapter 4 of "BCM2835 ARM Peripherals".
 *
 * A channel runs while ACTIVE is set and CONBLK_AD is not 0: it loads the
 * control block there, moves TXFR_LEN bytes from SOURCE_AD to DEST_AD,
 * incrementing each address that SRC_INC or DEST_INC asks for, sets END,
 * and INT if INTEN asks for it, then goes on with NEXTCONBK; after a
 * NEXTCONBK of 0, it clears ACTIVE (section 4.2.1.1). With SRC_DREQ or
 * DEST_DREQ, it moves one 32-bit word each time it finds the DREQ of
 * PERMAP high (section 4.2.1.3).
 *
 * What the datasheet does not give, this model chooses:
 *  - Time: a channel moves a 32-bit word in a cycle of the 250 MHz core
 *    clock, a LITE channel in two ("about half the bandwidth"), and
 *    loading a control block takes the time of its 8 words. The data of
 *    a transfer that no DREQ paces moves at once, and the transfer ends
 *    after the time of its words. Bursts, wait cycles, priorities and
 *    write responses change nothing else.
 *  - The widths: a transfer moves the bytes it is asked for, as if each
 *    side were read and written byte by byte, whatever SRC_WIDTH and
 *    DEST_WIDTH say. A side that does not increment, or a peripheral, is
 *    read or written 32 bits at a time.
 *  - CS.DREQ reads 0 until a control block has been loaded; PAUSED is
 *    set while a control block waits with ACTIVE clear.
 *  - RESET clears every register of the channel and stops its transfer,
 *    whatever the other bits of the write. ABORT drops the transfer in
 *    progress without END or INT, and goes on with NEXTCONBK.
 *  - A channel never sees a bus error: an address where nothing is
 *    modelled stops the machine, as does the VideoCore's part of the
 *    RAM, above 0x3F000000 in each alias.
 *
 * Not modelled, and the machine stops when software uses them: 2D mode,
 * the ignore modes, a transfer of 0 bytes, a 128-bit width on a side that
 * does not increment, a control block that is not 256-bit aligned, a
 * DREQ that no modelled peripheral drives, writes of the registers that
 * the control block loads, and of INT_STATUS, a channel that ENABLE
 * turns off, and other sizes than 32 bits.
 */

#include "hw/dma/bcm2835_dma.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "util/bits.h"
#include "util/byteorder.h"

enum {
	REG_CS = 0x00,
	REG_CONBLK_AD = 0x04,
	REG_TI = 0x08,
	REG_SOURCE_AD = 0x0c,
	REG_DEST_AD = 0x10,
	REG_TXFR_LEN = 0x14,
	REG_STRIDE = 0x18,
	REG_NEXTCONBK = 0x1c,
	REG_DEBUG = 0x20,
	REG_INT_STATUS = 0xfe0,
	REG_ENABLE = 0xff0,
};

#define CS_RESET (1u << 31)
#define CS_ABORT (1u << 30)
#define CS_KEPT 0x30ff0000u /* DISDEBUG, WAIT_FOR_..., the priorities */
#define CS_DREQ_STOPS_DMA (1u << 5)
#define CS_PAUSED (1u << 4)
#define CS_DREQ (1u << 3)
#define CS_INT (1u << 2)
#define CS_END (1u << 1)
#define CS_ACTIVE (1u << 0)

#define TI_INTEN (1u << 0)
#define TI_TDMODE (1u << 1)
#define TI_DEST_INC (1u << 4)
#define TI_DEST_WIDTH (1u << 5)
#define TI_DEST_DREQ (1u << 6)
#define TI_DEST_IGNORE (1u << 7)
#define TI_SRC_INC (1u << 8)
#define TI_SRC_WIDTH (1u << 9)
#define TI_SRC_DREQ (1u << 10)
#define TI_SRC_IGNORE (1u << 11)
#define TI_MASK 0x07fffffbu /* bits 26:0, but the reserved bit 2 */
#define TI_LITE_REMOVED (TI_TDMODE | TI_DEST_IGNORE | TI_SRC_IGNORE)

#define LEN_MASK 0x3fffffffu
#define LEN_LITE_MASK 0x0000ffffu
#define CB_ALIGN 0x1fu
#define CB_WORDS 8

#define DEBUG_LITE (1u << 28)
#define DEBUG_VERSION (2u << 25)

#define ENABLE_MASK 0x7fffu /* channels 0 to 14 */

#define BUS_PERIPHERALS 0x7e000000u
#define ARM_PERIPHERALS 0x3f000000u
#define PERIPHERALS_SIZE 0x01000000u
#define ALIAS_MASK 0x3fffffffu
#define VC_RAM 0x3f000000u /* the VideoCore's part of the RAM, and up */

#define NS_PER_WORD 4 /* a cycle of the 250 MHz core clock */
#define COPY_CHUNK 4096

#define CHANNEL_STRIDE 0x100
#define FIRST_LITE 7
#define LAST_LITE 14
#define SHARED_IRQ 11 /* channels 11 to 14 */

typedef struct Channel {
	struct Bcm2835Dma *dma;
	unsigned n;
	uint32_t cs, conblk, ti, src, dst, len, stride, next;
	bool loaded;  /* a control block is loaded and its transfer not done */
	bool cb_seen; /* a control block was loaded since the reset */
	bool waiting; /* stopped by its DREQ */
	Event step;
} Channel;

struct Bcm2835Dma {
	Machine *machine;
	IrqLine *irqs[BCM2835_DMA_NUM_IRQS];
	IrqLine dreqs[BCM2835_DMA_NUM_DREQS];
	bool driven[BCM2835_DMA_NUM_DREQS];
	uint32_t enable;
	Channel ch[BCM2835_DMA_NUM_CHANNELS];
};

static bool lite(const Channel *c)
{
	return c->n >= FIRST_LITE && c->n <= LAST_LITE;
}

static uint64_t ns_per_word(const Channel *c)
{
	return lite(c) ? 2 * NS_PER_WORD : NS_PER_WORD;
}

static unsigned permap(const Channel *c)
{
	return field32(c->ti, 20, 16);
}

static bool paced(const Channel *c)
{
	return (c->ti & (TI_SRC_DREQ | TI_DEST_DREQ)) && permap(c) != 0;
}

static bool dreq_level(const Channel *c)
{
	return permap(c) == 0 || irq_level(&c->dma->dreqs[permap(c)]);
}

static void update_irqs(Bcm2835Dma *dma)
{
	bool shared = false;

	for (unsigned n = 0; n < BCM2835_DMA_NUM_CHANNELS; n++) {
		bool level = dma->ch[n].cs & CS_INT;

		if (n < SHARED_IRQ)
			irq_set(dma->irqs[n], level);
		else if (n <= LAST_LITE)
			shared |= level;
	}
	irq_set(dma->irqs[SHARED_IRQ], shared);
}

/*
 * Where a bus address goes: the ARM's physical address, and whether a
 * peripheral is there. False for the VideoCore's part of the RAM.
 */
static bool translate(uint32_t address, uint64_t *arm, bool *mmio)
{
	uint32_t ram = address & ALIAS_MASK;

	if (address - BUS_PERIPHERALS < PERIPHERALS_SIZE) {
		*arm = ARM_PERIPHERALS + (address - BUS_PERIPHERALS);
		*mmio = true;
		return true;
	}
	*arm = ram;
	*mmio = false;
	return ram < VC_RAM;
}

static void fail(Channel *c, const char *what, uint32_t address)
{
	machine_stop(c->dma->machine, MACHINE_STOP_ERROR,
	             "bcm2835-dma: channel %u: %s at bus address 0x%08" PRIx32
	             " is not emulated",
	             c->n, what, address);
}

static bool stopped(const Channel *c)
{
	return c->dma->machine->stop != MACHINE_STOP_NONE;
}

/* One 32-bit word from src to dst, either a peripheral or the RAM. */
static bool move_word(Channel *c)
{
	Bus *bus = c->dma->machine->bus;
	uint64_t from, to, word = 0;
	bool mmio;

	if (!translate(c->src, &from, &mmio) ||
	    bus_read(bus, from, 4, &word) != BUS_OK) {
		fail(c, "a read", c->src);
		return false;
	}
	if (stopped(c))
		return false;
	if (!translate(c->dst, &to, &mmio) ||
	    bus_write(bus, to, 4, word) != BUS_OK) {
		fail(c, "a write", c->dst);
		return false;
	}
	if (stopped(c))
		return false;
	if (c->ti & TI_SRC_INC)
		c->src += 4;
	if (c->ti & TI_DEST_INC)
		c->dst += 4;
	c->len -= 4;
	return true;
}

/*
 * Whether a bus address is in the RAM, and where. A range that runs past
 * it fails on the bus: the peripherals follow the RAM there.
 */
static bool in_ram(uint32_t address, uint64_t *arm)
{
	bool mmio;

	return translate(address, arm, &mmio) && !mmio;
}

/* n bytes of RAM, both sides incrementing, through a buffer. */
static bool copy_ram(Channel *c, uint32_t n)
{
	Bus *bus = c->dma->machine->bus;
	uint8_t buf[COPY_CHUNK];

	while (n != 0) {
		uint32_t chunk = n < COPY_CHUNK ? n : COPY_CHUNK;
		uint64_t from, to;

		if (!in_ram(c->src, &from) ||
		    bus_read_block(bus, from, buf, chunk) != BUS_OK) {
			fail(c, "a read", c->src);
			return false;
		}
		if (!in_ram(c->dst, &to) ||
		    bus_write_block(bus, to, buf, chunk) != BUS_OK) {
			fail(c, "a write", c->dst);
			return false;
		}
		c->src += chunk;
		c->dst += chunk;
		c->len -= chunk;
		n -= chunk;
	}
	return true;
}

static bool both_ram(const Channel *c)
{
	uint64_t arm;

	return in_ram(c->src, &arm) && in_ram(c->dst, &arm);
}

/* The whole of an unpaced transfer. */
static bool move_all(Channel *c)
{
	uint32_t inc = TI_SRC_INC | TI_DEST_INC;

	if ((c->ti & inc) == inc && both_ram(c))
		return copy_ram(c, c->len);
	if (c->len % 4 != 0 || c->src % 4 != 0 || c->dst % 4 != 0) {
		machine_stop(c->dma->machine, MACHINE_STOP_ERROR,
		             "bcm2835-dma: channel %u: a transfer of %" PRIu32
		             " bytes from 0x%08" PRIx32 " to 0x%08" PRIx32
		             " is not implemented yet: only 32-bit words go to or"
		             " from a peripheral or an address that does not"
		             " increment",
		             c->n, c->len, c->src, c->dst);
		return false;
	}
	while (c->len != 0)
		if (!move_word(c))
			return false;
	return true;
}

static void schedule(Channel *c, uint64_t delay)
{
	Machine *m = c->dma->machine;

	event_schedule(&m->events, &c->step, machine_now(m) + delay);
}

static void unsupported(Channel *c, const char *what)
{
	machine_stop(c->dma->machine, MACHINE_STOP_ERROR,
	             "bcm2835-dma: channel %u: the control block at 0x%08" PRIx32
	             " %s",
	             c->n, c->conblk, what);
}

/* Load the control block at CONBLK_AD into the registers. */
static bool load(Channel *c)
{
	uint32_t w[CB_WORDS], width = TI_SRC_WIDTH | TI_DEST_WIDTH;
	uint8_t bytes[4 * CB_WORDS];
	uint64_t arm;
	bool mmio;

	if (c->conblk & CB_ALIGN) {
		unsupported(c, "is not described: it is not 256-bit aligned");
		return false;
	}
	if (!translate(c->conblk, &arm, &mmio) || mmio ||
	    bus_read_block(c->dma->machine->bus, arm, bytes, sizeof(bytes)) !=
	        BUS_OK) {
		fail(c, "a control block", c->conblk);
		return false;
	}
	for (unsigned i = 0; i < CB_WORDS; i++)
		w[i] = load_le32(bytes + 4 * i);
	c->ti = w[0] & TI_MASK & (lite(c) ? ~TI_LITE_REMOVED : ~0u);
	c->src = w[1];
	c->dst = w[2];
	c->len = w[3] & (lite(c) ? LEN_LITE_MASK : LEN_MASK);
	c->stride = w[4]; /* a LITE channel has no register to show it */
	c->next = w[5] & ~CB_ALIGN;
	c->loaded = true;
	c->cb_seen = true;
	if (c->ti & TI_TDMODE) {
		unsupported(c, "is not implemented yet: 2D mode");
		return false;
	}
	if (c->ti & (TI_SRC_IGNORE | TI_DEST_IGNORE)) {
		unsupported(c, "is not implemented yet: the ignore modes");
		return false;
	}
	if (c->len == 0) {
		unsupported(c, "is not described: a transfer of 0 bytes");
		return false;
	}
	if ((c->ti & width) &&
	    ((c->ti & (TI_SRC_INC | TI_DEST_INC)) != (TI_SRC_INC | TI_DEST_INC) ||
	     !both_ram(c))) {
		unsupported(c, "is not implemented yet: a 128-bit width on a side "
		               "that does not increment, or a peripheral");
		return false;
	}
	if (paced(c) && (c->len % 4 != 0 || c->src % 4 != 0 || c->dst % 4 != 0)) {
		unsupported(c, "is not implemented yet: a paced transfer of bytes "
		               "that are not whole 32-bit words");
		return false;
	}
	if (paced(c) && !c->dma->driven[permap(c)]) {
		machine_stop(c->dma->machine, MACHINE_STOP_ERROR,
		             "bcm2835-dma: channel %u: DREQ %u is not implemented "
		             "yet: no modelled peripheral drives it",
		             c->n, permap(c));
		return false;
	}
	return true;
}

/* The end of a control block: END, INT, and the next one. */
static void finish(Channel *c)
{
	c->cs |= CS_END;
	if (c->ti & TI_INTEN) {
		c->cs |= CS_INT;
		update_irqs(c->dma);
	}
	c->loaded = false;
	c->conblk = c->next;
	if (c->next == 0)
		c->cs &= ~CS_ACTIVE;
}

/* The time a channel takes to load a control block. */
static uint64_t fetch_ns(const Channel *c)
{
	return CB_WORDS * ns_per_word(c);
}

/*
 * The next part of a channel's work, when its time has come: load a
 * control block and move its data, or end the transfer.
 */
static void step(void *ctx)
{
	Channel *c = ctx;
	uint64_t words;

	if (!c->loaded && !load(c))
		return;
	if (c->len == 0) {
		finish(c);
		if (c->cs & CS_ACTIVE)
			schedule(c, fetch_ns(c));
		return;
	}
	if (paced(c)) {
		c->waiting = !dreq_level(c);
		if (!c->waiting && move_word(c))
			schedule(c, ns_per_word(c));
		return;
	}
	words = (c->len + 3u) / 4u;
	if (move_all(c))
		schedule(c, words * ns_per_word(c));
}

/* A DREQ changed: the channels it stopped look at it again. */
static void dreq_changed(void *ctx, unsigned n, bool level)
{
	Bcm2835Dma *dma = ctx;

	(void)level;
	for (unsigned i = 0; i < BCM2835_DMA_NUM_CHANNELS; i++) {
		Channel *c = &dma->ch[i];

		if (c->waiting && permap(c) == n) {
			c->waiting = false;
			schedule(c, 0);
		}
	}
}

static bool enabled(const Bcm2835Dma *dma, unsigned n)
{
	return n > LAST_LITE || (dma->enable >> n & 1);
}

static void reset_channel(Channel *c)
{
	event_cancel(&c->dma->machine->events, &c->step);
	c->cs = c->conblk = c->ti = c->src = c->dst = 0;
	c->len = c->stride = c->next = 0;
	c->loaded = c->cb_seen = c->waiting = false;
	update_irqs(c->dma);
}

/* Start or go on, if the channel has something to do. */
static void kick(Channel *c, uint64_t offset)
{
	if (!(c->cs & CS_ACTIVE) || (!c->loaded && c->conblk == 0))
		return;
	if (!enabled(c->dma, c->n)) {
		machine_stop(c->dma->machine, MACHINE_STOP_ERROR,
		             "bcm2835-dma: 4-byte write to offset 0x%03" PRIx64
		             " is not described: channel %u runs while ENABLE "
		             "turns it off",
		             offset, c->n);
		return;
	}
	if (!event_pending(&c->step))
		schedule(c, c->loaded ? 0 : fetch_ns(c));
}

static uint32_t read_cs(const Channel *c)
{
	uint32_t cs = c->cs;

	if (c->loaded && !(cs & CS_ACTIVE))
		cs |= CS_PAUSED;
	if (c->waiting)
		cs |= CS_DREQ_STOPS_DMA;
	if (c->cb_seen && dreq_level(c))
		cs |= CS_DREQ;
	return cs;
}

static void write_cs(Channel *c, uint64_t offset, uint32_t v)
{
	if (v & CS_RESET) {
		reset_channel(c);
		return;
	}
	c->cs = (c->cs & ~(CS_KEPT | CS_ACTIVE)) | (v & (CS_KEPT | CS_ACTIVE));
	c->cs &= ~(v & (CS_INT | CS_END));
	update_irqs(c->dma);
	if ((v & CS_ABORT) && c->loaded) {
		event_cancel(&c->dma->machine->events, &c->step);
		c->loaded = false;
		c->waiting = false;
		c->conblk = c->next;
		if (c->next == 0)
			c->cs &= ~CS_ACTIVE;
	}
	if (!(c->cs & CS_ACTIVE)) {
		event_cancel(&c->dma->machine->events, &c->step);
		c->waiting = false;
	}
	kick(c, offset);
}

static void stop_access(Bcm2835Dma *dma, const char *access, uint64_t offset,
                        unsigned size, const char *why)
{
	machine_stop(dma->machine, MACHINE_STOP_ERROR,
	             "bcm2835-dma: %u-byte %s offset 0x%03" PRIx64 " %s", size,
	             access, offset, why);
}

/* The channel whose registers are at offset, and the register. */
static Channel *channel_at(Bcm2835Dma *dma, uint64_t offset, unsigned *reg)
{
	unsigned n = (unsigned)(offset / CHANNEL_STRIDE);
	Channel *c;

	*reg = (unsigned)(offset % CHANNEL_STRIDE);
	if (n > LAST_LITE)
		return NULL;
	c = &dma->ch[n];
	if (*reg > REG_DEBUG || (lite(c) && *reg == REG_STRIDE))
		return NULL;
	return c;
}

static uint64_t channel_read(const Channel *c, unsigned reg)
{
	switch (reg) {
	case REG_CS:
		return read_cs(c);
	case REG_CONBLK_AD:
		return c->conblk;
	case REG_TI:
		return c->ti;
	case REG_SOURCE_AD:
		return c->src;
	case REG_DEST_AD:
		return c->dst;
	case REG_TXFR_LEN:
		return c->len;
	case REG_STRIDE:
		return c->stride;
	case REG_NEXTCONBK:
		return c->next;
	default: /* REG_DEBUG */
		return DEBUG_VERSION | (lite(c) ? DEBUG_LITE : 0);
	}
}

static void channel_write(Channel *c, uint64_t offset, unsigned reg, uint32_t v)
{
	switch (reg) {
	case REG_CS:
		write_cs(c, offset, v);
		return;
	case REG_CONBLK_AD:
		if (c->loaded) {
			stop_access(c->dma, "write to", offset, 4,
			            "is not described: a control block is loaded");
			return;
		}
		c->conblk = v;
		kick(c, offset);
		return;
	case REG_NEXTCONBK:
		c->next = v & ~CB_ALIGN;
		return;
	case REG_DEBUG:
		return; /* no error flag is ever set */
	default:
		stop_access(c->dma, "write to", offset, 4,
		            "is not emulated: the control block loads it");
		return;
	}
}

static bool accessible(Bcm2835Dma *dma, const char *access, uint64_t offset,
                       unsigned size)
{
	if (size != 4 || offset % 4 != 0) {
		stop_access(dma, access, offset, size,
		            "is not a 32-bit register access");
		return false;
	}
	return true;
}

static uint64_t dma_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Dma *dma = ctx;
	uint32_t status = 0;
	unsigned reg;
	Channel *c;

	if (!accessible(dma, "read from", offset, size))
		return 0;
	if (offset == REG_INT_STATUS) {
		for (unsigned n = 0; n < BCM2835_DMA_NUM_CHANNELS; n++)
			if (dma->ch[n].cs & CS_INT)
				status |= 1u << n;
		return status;
	}
	if (offset == REG_ENABLE)
		return dma->enable;
	c = channel_at(dma, offset, &reg);
	if (c == NULL) {
		stop_access(dma, "read from", offset, size,
		            "is not described by the datasheet");
		return 0;
	}
	return channel_read(c, reg);
}

static void dma_write(void *ctx, uint64_t offset, uint64_t value, unsigned size)
{
	Bcm2835Dma *dma = ctx;
	uint32_t v = (uint32_t)value;
	unsigned reg;
	Channel *c;

	if (!accessible(dma, "write to", offset, size))
		return;
	if (offset == REG_ENABLE) {
		for (unsigned n = 0; n <= LAST_LITE; n++) {
			if (!(v >> n & 1) && (dma->ch[n].loaded)) {
				stop_access(dma, "write to", offset, size,
				            "is not described: it turns off a channel "
				            "that runs");
				return;
			}
		}
		dma->enable = v & ENABLE_MASK;
		return;
	}
	c = channel_at(dma, offset, &reg);
	if (c == NULL || offset == REG_INT_STATUS) {
		stop_access(dma, "write to", offset, size,
		            offset == REG_INT_STATUS
		                ? "is not described: INT_STATUS"
		                : "is not described by the datasheet");
		return;
	}
	channel_write(c, offset, reg, v);
}

/* Channel 15, alone at its own base. */
static uint64_t dma15_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Dma *dma = ctx;

	if (!accessible(dma, "read from channel 15", offset, size))
		return 0;
	if (offset > REG_DEBUG) {
		stop_access(dma, "read from channel 15", offset, size,
		            "is not described by the datasheet");
		return 0;
	}
	return channel_read(&dma->ch[15], (unsigned)offset);
}

static void dma15_write(void *ctx, uint64_t offset, uint64_t value,
                        unsigned size)
{
	Bcm2835Dma *dma = ctx;

	if (!accessible(dma, "write to channel 15", offset, size))
		return;
	if (offset > REG_DEBUG) {
		stop_access(dma, "write to channel 15", offset, size,
		            "is not described by the datasheet");
		return;
	}
	channel_write(&dma->ch[15], offset, (unsigned)offset, (uint32_t)value);
}

static const BusMmioOps dma_mmio_ops = {
	.read = dma_read,
	.write = dma_write,
};

static const BusMmioOps dma15_mmio_ops = {
	.read = dma15_read,
	.write = dma15_write,
};

Bcm2835Dma *bcm2835_dma_create(Machine *m,
                               IrqLine *const irqs[BCM2835_DMA_NUM_IRQS])
{
	Bcm2835Dma *dma = calloc(1, sizeof(*dma));

	if (dma == NULL)
		return NULL;
	dma->machine = m;
	for (unsigned i = 0; i < BCM2835_DMA_NUM_IRQS; i++)
		dma->irqs[i] = irqs[i];
	for (unsigned i = 0; i < BCM2835_DMA_NUM_DREQS; i++)
		irq_init(&dma->dreqs[i], dreq_changed, dma, i);
	for (unsigned n = 0; n < BCM2835_DMA_NUM_CHANNELS; n++) {
		dma->ch[n].dma = dma;
		dma->ch[n].n = n;
		event_init(&dma->ch[n].step, step, &dma->ch[n]);
	}
	bcm2835_dma_reset(dma);
	return dma;
}

void bcm2835_dma_destroy(Bcm2835Dma *dma)
{
	for (unsigned n = 0; n < BCM2835_DMA_NUM_CHANNELS; n++)
		event_cancel(&dma->machine->events, &dma->ch[n].step);
	free(dma);
}

void bcm2835_dma_reset(Bcm2835Dma *dma)
{
	for (unsigned n = 0; n < BCM2835_DMA_NUM_CHANNELS; n++)
		reset_channel(&dma->ch[n]);
	dma->enable = ENABLE_MASK;
}

BusStatus bcm2835_dma_map(Bcm2835Dma *dma, Bus *bus, const char *name,
                          uint64_t base, uint64_t base15)
{
	BusStatus status = bus_map_mmio(bus, name, base, BCM2835_DMA_MMIO_SIZE,
	                                &dma_mmio_ops, dma);

	if (status != BUS_OK)
		return status;
	return bus_map_mmio(bus, name, base15, BCM2835_DMA15_MMIO_SIZE,
	                    &dma15_mmio_ops, dma);
}

IrqLine *bcm2835_dma_dreq(Bcm2835Dma *dma, unsigned n)
{
	dma->driven[n] = true;
	return &dma->dreqs[n];
}
