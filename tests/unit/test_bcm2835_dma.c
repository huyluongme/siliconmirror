/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835's DMA controller (src/hw/dma/bcm2835_dma.c),
 * from chapter 4 of the BCM2835 ARM Peripherals datasheet, with a FIFO
 * peripheral that drives DREQ 13.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/dma/bcm2835_dma.h"
#include "test.h"
#include "util/byteorder.h"
#include "util/macros.h"

#define BASE 0x3f007000
#define BASE15 0x3fe05000
#define FIFO_AT 0x3f202000 /* bus address 0x7e202000 */
#define FIFO_BUS 0x7e202040u
#define RAM_SIZE 0x10000

enum {
	CS = 0x00,
	CONBLK_AD = 0x04,
	TI = 0x08,
	SOURCE_AD = 0x0c,
	DEST_AD = 0x10,
	TXFR_LEN = 0x14,
	STRIDE = 0x18,
	NEXTCONBK = 0x1c,
	DEBUG = 0x20,
	INT_STATUS = 0xfe0,
	ENABLE = 0xff0,
};

enum {
	ACTIVE = 1u << 0,
	END = 1u << 1,
	INT = 1u << 2,
	DREQ = 1u << 3,
	PAUSED = 1u << 4,
	DREQ_STOPS = 1u << 5,
	ABORT = 1u << 30,
	INTEN = 1u << 0,
	TDMODE = 1u << 1,
	DEST_INC = 1u << 4,
	DEST_WIDTH = 1u << 5,
	DEST_DREQ = 1u << 6,
	DEST_IGNORE = 1u << 7,
	SRC_INC = 1u << 8,
	SRC_WIDTH = 1u << 9,
	SRC_DREQ = 1u << 10,
	SRC_IGNORE = 1u << 11,
};

#define RESET (1u << 31)
#define PERMAP(n) ((uint32_t)(n) << 16)
#define VERSION (2u << 25)
#define LITE (1u << 28)

/* A peripheral with a FIFO of words at offset 0x40, and its DREQ. */
typedef struct Fifo {
	uint32_t words[64];
	unsigned head, count, reads, writes;
	uint64_t offset;    /* of the last access */
	bool sink;          /* DREQ for room, not for data */
	bool stop_on_read;  /* a read stops the machine, as SDHOST's can */
	bool stop_on_write; /* so does a write */
	Machine *m;
	IrqLine *dreq;
} Fifo;

static void fifo_dreq(Fifo *f)
{
	irq_set(f->dreq, f->sink ? f->count < 4 : f->count != 0);
}

static uint64_t fifo_read(void *ctx, uint64_t offset, unsigned size)
{
	Fifo *f = ctx;
	uint32_t w;

	(void)size;
	f->offset = offset;
	f->reads++;
	if (f->stop_on_read) {
		machine_stop(f->m, MACHINE_STOP_ERROR, "fifo: read");
		return 0x99;
	}
	if (f->count == 0)
		return 0xdeadbeef;
	w = f->words[f->head];
	f->head = (f->head + 1) % 64;
	f->count--;
	fifo_dreq(f);
	return w;
}

static void fifo_write(void *ctx, uint64_t offset, uint64_t value,
                       unsigned size)
{
	Fifo *f = ctx;

	(void)size;
	f->offset = offset;
	f->writes++;
	if (f->stop_on_write) {
		machine_stop(f->m, MACHINE_STOP_ERROR, "fifo: write");
		return;
	}
	f->words[(f->head + f->count) % 64] = (uint32_t)value;
	f->count++;
	fifo_dreq(f);
}

static const BusMmioOps fifo_ops = {.read = fifo_read, .write = fifo_write};

typedef struct Setup {
	Machine *m;
	Bcm2835Dma *dma;
	IrqLine irqs[BCM2835_DMA_NUM_IRQS];
	Fifo fifo;  /* at bus address 0x7e202000, with DREQ 13 */
	Fifo first; /* at 0x7e000000, the first of the peripherals */
	Fifo local; /* at ARM 0x40000000, which no bus address reaches */
} Setup;

static void setup(Setup *s)
{
	IrqLine *lines[BCM2835_DMA_NUM_IRQS];

	memset(s, 0, sizeof(*s));
	s->m = machine_create();
	CHECK(s->m != NULL);
	CHECK_EQ(bus_map_ram(s->m->bus, "ram", 0, RAM_SIZE), BUS_OK);
	for (unsigned i = 0; i < BCM2835_DMA_NUM_IRQS; i++) {
		irq_init(&s->irqs[i], NULL, NULL, i);
		lines[i] = &s->irqs[i];
	}
	s->dma = bcm2835_dma_create(s->m, lines);
	CHECK(s->dma != NULL);
	CHECK_EQ(bcm2835_dma_map(s->dma, s->m->bus, "dma", BASE, BASE15), BUS_OK);
	CHECK_EQ(
		bus_map_mmio(s->m->bus, "fifo", FIFO_AT, 0x100, &fifo_ops, &s->fifo),
		BUS_OK);
	CHECK_EQ(bus_map_mmio(s->m->bus, "first", 0x3f000000, 0x100, &fifo_ops,
	                      &s->first),
	         BUS_OK);
	CHECK_EQ(bus_map_mmio(s->m->bus, "local", 0x40000000, 0x100, &fifo_ops,
	                      &s->local),
	         BUS_OK);
	s->fifo.m = s->first.m = s->local.m = s->m;
	s->fifo.dreq = bcm2835_dma_dreq(s->dma, BCM2835_DMA_DREQ_SDHOST);
}

static void teardown(Setup *s)
{
	bcm2835_dma_destroy(s->dma);
	machine_destroy(s->m);
}

static uint64_t ch(unsigned n)
{
	return n == 15 ? BASE15 : BASE + 0x100u * n;
}

static uint32_t rd(Setup *s, uint64_t address)
{
	uint64_t v = 0;

	CHECK_EQ(bus_read(s->m->bus, address, 4, &v), BUS_OK);
	return (uint32_t)v;
}

static void wr(Setup *s, uint64_t address, uint32_t v)
{
	CHECK_EQ(bus_write(s->m->bus, address, 4, v), BUS_OK);
	if (s->m->stop != MACHINE_STOP_NONE) {
		fprintf(stderr, "stopped: %s\n", s->m->stop_message);
		test_failures++;
		s->m->stop = MACHINE_STOP_NONE;
	}
}

static void put_cb(Setup *s, uint32_t at, uint32_t ti, uint32_t src,
                   uint32_t dst, uint32_t len, uint32_t stride, uint32_t next)
{
	uint32_t w[8] = {ti, src, dst, len, stride, next, 0, 0};
	uint8_t bytes[32];

	for (unsigned i = 0; i < 8; i++)
		store_le32(bytes + 4 * i, w[i]);
	CHECK_EQ(bus_write_block(s->m->bus, at, bytes, sizeof(bytes)), BUS_OK);
}

static void run(Setup *s, uint64_t t)
{
	event_queue_run_until(&s->m->events, t);
}

static void start(Setup *s, unsigned n, uint32_t cb)
{
	wr(s, ch(n) + CONBLK_AD, cb);
	wr(s, ch(n) + CS, ACTIVE);
}

/* Every channel at reset; DEBUG tells the LITE engines, 7 to 14. */
static void test_reset_values(void)
{
	Setup s;

	setup(&s);
	for (unsigned n = 0; n < BCM2835_DMA_NUM_CHANNELS; n++) {
		CHECK_EQ(rd(&s, ch(n) + CS), 0);
		CHECK_EQ(rd(&s, ch(n) + CONBLK_AD), 0);
		CHECK_EQ(rd(&s, ch(n) + TI), 0);
		CHECK_EQ(rd(&s, ch(n) + TXFR_LEN), 0);
		CHECK_EQ(rd(&s, ch(n) + NEXTCONBK), 0);
		CHECK_EQ(rd(&s, ch(n) + DEBUG),
		         VERSION | (n >= 7 && n <= 14 ? LITE : 0));
		if (n < 7 || n == 15)
			CHECK_EQ(rd(&s, ch(n) + STRIDE), 0);
	}
	CHECK_EQ(rd(&s, BASE + ENABLE), 0x7fff);
	CHECK_EQ(rd(&s, BASE + INT_STATUS), 0);
	teardown(&s);
}

/*
 * A copy in RAM, through two aliases of the bus: the control block loads
 * in 8 words' time, the data moves, and the block ends after its words.
 */
static void test_copy(void)
{
	Setup s;
	uint8_t src[101], dst[101];

	setup(&s);
	for (unsigned i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)(i * 7 + 1);
	CHECK_EQ(bus_write_block(s.m->bus, 0x2000, src, sizeof(src)), BUS_OK);
	put_cb(&s, 0x1000, SRC_INC | DEST_INC | INTEN, 0xc0002000, 0x40003001, 101,
	       0x12345678, 0);
	put_cb(&s, 0, SRC_INC | DEST_INC, 0x2000, 0x3800, 4, 0, 0); /* unused */
	wr(&s, ch(0) + CONBLK_AD, 0x80001000);
	CHECK_EQ(rd(&s, ch(0) + NEXTCONBK), 0); /* CONBLK_AD alone */
	wr(&s, ch(0) + CS, ACTIVE);
	CHECK_EQ(rd(&s, ch(0) + CS), ACTIVE); /* not loaded: no DREQ yet */
	run(&s, 31);
	CHECK_EQ(rd(&s, ch(0) + TI), 0);
	run(&s, 32);
	CHECK_EQ(rd(&s, ch(0) + TI), SRC_INC | DEST_INC | INTEN);
	CHECK_EQ(rd(&s, ch(0) + STRIDE), 0x12345678);
	CHECK_EQ(rd(&s, ch(0) + TXFR_LEN), 0); /* the data moved at once */
	CHECK_EQ(rd(&s, ch(0) + SOURCE_AD), 0xc0002000 + 101);
	CHECK_EQ(rd(&s, ch(0) + DEST_AD), 0x40003001 + 101);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ | ACTIVE); /* PERMAP 0: DREQ reads 1 */
	CHECK_EQ(bus_read_block(s.m->bus, 0x3001, dst, sizeof(dst)), BUS_OK);
	CHECK(memcmp(src, dst, sizeof(src)) == 0);
	run(&s, 32 + 26 * 4 - 1);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ | ACTIVE);
	CHECK_EQ(irq_level(&s.irqs[0]), false);
	run(&s, 32 + 26 * 4);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ | INT | END);
	CHECK_EQ(rd(&s, ch(0) + CONBLK_AD), 0);
	CHECK_EQ(irq_level(&s.irqs[0]), true);
	CHECK_EQ(rd(&s, BASE + INT_STATUS), 1);
	wr(&s, ch(0) + CS, INT);
	CHECK_EQ(irq_level(&s.irqs[0]), false);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ | END);
	wr(&s, ch(0) + CS, END);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ);

	/* ACTIVE with CONBLK_AD 0 does nothing; the kept bits read back. */
	wr(&s, ch(0) + CS, 0x30ff0000u | ACTIVE);
	run(&s, 5000);
	CHECK_EQ(rd(&s, ch(0) + CS), 0x30ff0000u | DREQ | ACTIVE);
	CHECK_EQ(rd(&s, ch(0) + CONBLK_AD), 0);
	CHECK_EQ(rd(&s, ch(0) + TI), SRC_INC | DEST_INC | INTEN);
	CHECK_EQ(rd(&s, 0x3800), 0); /* nothing loaded from address 0 */
	teardown(&s);
}

/* 128-bit widths, between two places in RAM that increment. */
static void test_wide(void)
{
	Setup s;

	setup(&s);
	for (uint32_t i = 0; i < 8; i++)
		CHECK_EQ(bus_write(s.m->bus, 0x2000 + 4 * i, 4, 0x100 + i), BUS_OK);
	put_cb(&s, 0x1000, SRC_WIDTH | DEST_WIDTH | SRC_INC | DEST_INC, 0x2000,
	       0x3000, 32, 0, 0);
	start(&s, 0, 0x1000);
	run(&s, 32 + 8 * 4);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ | END);
	CHECK_EQ(rd(&s, 0x301c), 0x107);
	teardown(&s);
}

/* A chain: the second block runs after the first; INT stays set. */
static void test_chain(void)
{
	Setup s;

	setup(&s);
	put_cb(&s, 0x1000, SRC_INC | DEST_INC | INTEN, 0x2000, 0x3000, 8, 0,
	       0x1020 | 0x1f);
	put_cb(&s, 0x1020, SRC_INC | DEST_INC, 0x2100, 0x3100, 4, 0, 0);
	CHECK_EQ(bus_write(s.m->bus, 0x2000, 8, 0x1122334455667788u), BUS_OK);
	CHECK_EQ(bus_write(s.m->bus, 0x2100, 4, 0xa5a5a5a5u), BUS_OK);
	start(&s, 2, 0x1000);
	run(&s, 32 + 8);
	CHECK_EQ(rd(&s, ch(2) + CS), DREQ | INT | END | ACTIVE);
	CHECK_EQ(rd(&s, ch(2) + CONBLK_AD), 0x1020);
	CHECK_EQ(irq_level(&s.irqs[2]), true);
	run(&s, 32 + 8 + 32 - 1);
	CHECK_EQ(rd(&s, ch(2) + TI), SRC_INC | DEST_INC | INTEN);
	run(&s, 32 + 8 + 32 + 4);
	CHECK_EQ(rd(&s, ch(2) + TI), SRC_INC | DEST_INC);
	CHECK_EQ(rd(&s, ch(2) + CS), DREQ | INT | END);
	CHECK_EQ(rd(&s, ch(2) + CONBLK_AD), 0);
	CHECK_EQ(rd(&s, 0x3000), 0x55667788u);
	CHECK_EQ(rd(&s, 0x3004), 0x11223344u);
	CHECK_EQ(rd(&s, 0x3100), 0xa5a5a5a5u);

	/* NEXTCONBK can be written; its low 5 bits read 0. */
	wr(&s, ch(2) + NEXTCONBK, 0x12345678);
	CHECK_EQ(rd(&s, ch(2) + NEXTCONBK), 0x12345660);
	wr(&s, ch(2) + DEBUG, 0xffffffff); /* no error flag to clear */
	CHECK_EQ(rd(&s, ch(2) + DEBUG), VERSION);
	wr(&s, ch(15) + DEBUG, 0xffffffff);
	CHECK_EQ(rd(&s, ch(15) + DEBUG), VERSION);
	teardown(&s);
}

/* A LITE engine: half the speed, 16 bits of length, no 2D or ignore. */
static void test_lite(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(bus_write(s.m->bus, 0x2000, 8, 0x0102030405060708u), BUS_OK);
	CHECK_EQ(bus_write(s.m->bus, 0x2008, 8, 0x090a0b0c0d0e0f10u), BUS_OK);
	CHECK_EQ(bus_write(s.m->bus, 0x3008, 4, 0xffffffffu), BUS_OK);
	put_cb(&s, 0x1000,
	       SRC_INC | DEST_INC | INTEN | TDMODE | SRC_IGNORE | DEST_IGNORE,
	       0x2000, 0x3000, 0xffff0009, 0, 0);
	start(&s, 7, 0x1000);
	run(&s, 63);
	CHECK_EQ(rd(&s, ch(7) + TI), 0);
	run(&s, 64);
	CHECK_EQ(rd(&s, ch(7) + TI), SRC_INC | DEST_INC | INTEN);
	CHECK_EQ(rd(&s, ch(7) + SOURCE_AD), 0x2009); /* 16 bits of length */
	run(&s, 64 + 3 * 8 - 1);
	CHECK_EQ(rd(&s, ch(7) + CS) & END, 0);
	run(&s, 64 + 3 * 8);
	CHECK_EQ(rd(&s, ch(7) + CS), DREQ | INT | END);
	CHECK_EQ(rd(&s, 0x3000), 0x05060708u);
	CHECK_EQ(rd(&s, 0x3004), 0x01020304u);
	CHECK_EQ(rd(&s, 0x3008), 0xffffff10u);
	CHECK_EQ(irq_level(&s.irqs[7]), true);
	teardown(&s);
}

/*
 * DREQ pacing: a read from the FIFO waits for its DREQ, then moves a
 * word each cycle while the FIFO has data; clearing ACTIVE pauses it. A
 * transfer that PERMAP names but no DREQ bit paces does not wait.
 */
static void test_paced_read(void)
{
	Setup s;

	setup(&s);
	put_cb(&s, 0x1000, PERMAP(13) | SRC_DREQ | DEST_INC | INTEN, FIFO_BUS,
	       0x3000, 12, 0, 0);
	start(&s, 4, 0x1000);
	run(&s, 32);
	CHECK_EQ(rd(&s, ch(4) + CS), DREQ_STOPS | ACTIVE); /* DREQ low */
	wr(&s, ch(4) + CS, 0);
	CHECK_EQ(rd(&s, ch(4) + CS), PAUSED); /* paused, not waiting */
	wr(&s, ch(4) + CS, ACTIVE);
	run(&s, 1000);
	CHECK_EQ(s.fifo.reads, 0);
	s.fifo.words[0] = 0x11111111;
	s.fifo.words[1] = 0x22222222;
	s.fifo.count = 2;
	fifo_dreq(&s.fifo);
	CHECK_EQ(rd(&s, ch(4) + CS), DREQ | ACTIVE);
	run(&s, 1000);
	CHECK_EQ(s.fifo.reads, 1);
	CHECK_EQ(s.fifo.offset, 0x40);
	run(&s, 1002);
	wr(&s, ch(4) + CS, 0); /* paused, between two words */
	CHECK_EQ(rd(&s, ch(4) + CS), PAUSED | DREQ);
	run(&s, 1100);
	CHECK_EQ(s.fifo.reads, 1);
	wr(&s, ch(4) + CS, ACTIVE);
	run(&s, 1100);
	CHECK_EQ(s.fifo.reads, 2);
	CHECK_EQ(rd(&s, ch(4) + TXFR_LEN), 4);
	run(&s, 1104);
	CHECK_EQ(rd(&s, ch(4) + CS), DREQ_STOPS | ACTIVE);

	/* Channel 2, unpaced with PERMAP 13, moves at once. */
	put_cb(&s, 0x1040, PERMAP(13) | SRC_INC | DEST_INC, 0x2000, 0x4000, 400, 0,
	       0);
	start(&s, 2, 0x1040);
	run(&s, 1104 + 32);
	CHECK_EQ(rd(&s, ch(2) + TXFR_LEN), 0);
	CHECK_EQ(rd(&s, ch(2) + CS), ACTIVE); /* DREQ 13 is low */

	/* The last word; DREQ's edges leave channel 2 alone. */
	run(&s, 1200);
	s.fifo.words[2] = 0x33333333;
	s.fifo.count = 1;
	fifo_dreq(&s.fifo);
	run(&s, 1204);
	CHECK_EQ(rd(&s, ch(4) + CS), INT | END);
	CHECK_EQ(rd(&s, 0x3000), 0x11111111);
	CHECK_EQ(rd(&s, 0x3004), 0x22222222);
	CHECK_EQ(rd(&s, 0x3008), 0x33333333);
	CHECK_EQ(s.fifo.reads, 3);
	run(&s, 1136 + 400 - 1);
	CHECK_EQ(rd(&s, ch(2) + CS), ACTIVE);
	run(&s, 1136 + 400);
	CHECK_EQ(rd(&s, ch(2) + CS), END);

	/* WAITS, next to PERMAP, changes nothing. */
	put_cb(&s, 0x1080, 1u << 21 | PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS,
	       0x3100, 4, 0, 0);
	s.fifo.count = 1;
	fifo_dreq(&s.fifo);
	start(&s, 4, 0x1080);
	run(&s, 2000);
	CHECK_EQ(rd(&s, ch(4) + CS), INT | END); /* INT stays until cleared */
	teardown(&s);
}

/* A paced write to the FIFO: a word while it has room. */
static void test_paced_write(void)
{
	Setup s;

	setup(&s);
	s.fifo.sink = true;
	fifo_dreq(&s.fifo);
	for (uint32_t i = 0; i < 6; i++)
		CHECK_EQ(bus_write(s.m->bus, 0x2000 + 4 * i, 4, 0xa0 + i), BUS_OK);
	put_cb(&s, 0x1000, PERMAP(13) | DEST_DREQ | SRC_INC, 0x2000, FIFO_BUS, 24,
	       0, 0);
	start(&s, 0, 0x1000);
	run(&s, 32 + 4 * 4);
	CHECK_EQ(s.fifo.writes, 4);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ_STOPS | ACTIVE);
	s.fifo.count = 0;
	fifo_dreq(&s.fifo);
	run(&s, 32 + 4 * 4 + 2 * 4);
	CHECK_EQ(s.fifo.writes, 6);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ | END);
	CHECK_EQ(rd(&s, ch(0) + SOURCE_AD), 0x2000 + 24);
	CHECK_EQ(rd(&s, ch(0) + DEST_AD), FIFO_BUS);
	CHECK_EQ(s.fifo.words[1], 0xa5);
	CHECK_EQ(irq_level(&s.irqs[0]), false); /* no INTEN */
	teardown(&s);
}

/* Unpaced words from a peripheral, the address staying the same. */
static void test_peripheral_words(void)
{
	Setup s;

	setup(&s);
	s.fifo.words[0] = 7;
	s.fifo.words[1] = 8;
	s.fifo.count = 2;
	put_cb(&s, 0x1000, DEST_INC, FIFO_BUS, 0x3000, 8, 0, 0);
	start(&s, 0, 0x1000);
	run(&s, 32 + 8);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ | END);
	CHECK_EQ(rd(&s, 0x3000), 7);
	CHECK_EQ(rd(&s, 0x3004), 8);
	CHECK_EQ(rd(&s, ch(0) + SOURCE_AD), FIFO_BUS);

	/* A word repeated from RAM that does not increment. */
	CHECK_EQ(bus_write(s.m->bus, 0x2000, 4, 0x5a5a5a5a), BUS_OK);
	put_cb(&s, 0x1000, DEST_INC, 0x2000, 0x3100, 12, 0, 0);
	start(&s, 0, 0x1000);
	run(&s, 1000);
	CHECK_EQ(rd(&s, 0x3108), 0x5a5a5a5a);

	/* The first peripheral's address, 0x7e000000. */
	s.first.words[0] = 0xc0ffee;
	s.first.count = 1;
	put_cb(&s, 0x1000, DEST_INC, 0x7e000040, 0x3200, 4, 0, 0);
	start(&s, 0, 0x1000);
	run(&s, 2000);
	CHECK_EQ(rd(&s, 0x3200), 0xc0ffee);
	CHECK_EQ(s.first.offset, 0x40);
	teardown(&s);
}

/* A peripheral that stops the machine: the transfer goes no further. */
static void test_device_stops(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(bus_write(s.m->bus, 0x3000, 4, 0x5a5a5a5a), BUS_OK);
	s.fifo.stop_on_read = true;
	put_cb(&s, 0x1000, DEST_INC, FIFO_BUS, 0x3000, 8, 0, 0);
	start(&s, 0, 0x1000);
	run(&s, 1000);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK_EQ(s.fifo.reads, 1);
	CHECK_EQ(rd(&s, 0x3000), 0x5a5a5a5a);
	CHECK_EQ(rd(&s, ch(0) + DEST_AD), 0x3000);
	CHECK_EQ(rd(&s, ch(0) + TXFR_LEN), 8);
	teardown(&s);

	setup(&s);
	s.fifo.stop_on_write = true;
	put_cb(&s, 0x1000, SRC_INC, 0x2000, FIFO_BUS, 8, 0, 0);
	start(&s, 0, 0x1000);
	run(&s, 1000);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK_EQ(s.fifo.writes, 1);
	CHECK_EQ(rd(&s, ch(0) + SOURCE_AD), 0x2000);
	CHECK_EQ(rd(&s, ch(0) + TXFR_LEN), 8);
	teardown(&s);
}

/* A long copy goes in pieces of 4 KB: up to where the RAM ends. */
static void test_long_copy(void)
{
	Setup s;

	setup(&s);
	for (uint32_t i = 0; i < 0x3000; i += 4)
		CHECK_EQ(bus_write(s.m->bus, 0x4000 + i, 4, i), BUS_OK);
	put_cb(&s, 0x1000, SRC_INC | DEST_INC, 0x4000, 0xe000, 0x3000, 0, 0);
	start(&s, 0, 0x1000);
	run(&s, 1000);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message,
	             "a write at bus address 0x00010000 is not emulated") != NULL);
	CHECK_EQ(rd(&s, 0xfffc), 0x1ffc);
	CHECK_EQ(rd(&s, ch(0) + TXFR_LEN), 0x1000);
	teardown(&s);
}

/* ABORT goes on with NEXTCONBK; RESET clears everything. */
static void test_abort_and_reset(void)
{
	Setup s;

	setup(&s);
	put_cb(&s, 0x1000, PERMAP(13) | SRC_DREQ | DEST_INC | INTEN, FIFO_BUS,
	       0x3000, 8, 0, 0x1020);
	put_cb(&s, 0x1020, SRC_INC | DEST_INC | INTEN, 0x2000, 0x3100, 4, 0, 0);
	start(&s, 0, 0x1000);
	run(&s, 100);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ_STOPS | ACTIVE);
	wr(&s, ch(0) + CS, ABORT | ACTIVE);
	CHECK_EQ(rd(&s, ch(0) + CS), ACTIVE); /* no longer stopped by DREQ */
	CHECK_EQ(rd(&s, ch(0) + CONBLK_AD), 0x1020);
	run(&s, 100 + 32 + 4);
	CHECK_EQ(rd(&s, ch(0) + CS), DREQ | INT | END); /* the second block's */
	CHECK_EQ(rd(&s, ch(0) + TI), SRC_INC | DEST_INC | INTEN);
	CHECK_EQ(irq_level(&s.irqs[0]), true);

	/* RESET clears INT and its line. */
	wr(&s, ch(0) + CS, RESET | ACTIVE | INT);
	CHECK_EQ(irq_level(&s.irqs[0]), false);
	CHECK_EQ(rd(&s, ch(0) + CS), 0);
	CHECK_EQ(rd(&s, ch(0) + CONBLK_AD), 0);
	CHECK_EQ(rd(&s, ch(0) + TI), 0);
	CHECK_EQ(rd(&s, ch(0) + SOURCE_AD), 0);
	CHECK_EQ(rd(&s, ch(0) + DEST_AD), 0);
	CHECK_EQ(rd(&s, ch(0) + TXFR_LEN), 0);
	CHECK_EQ(rd(&s, ch(0) + STRIDE), 0);
	CHECK_EQ(rd(&s, ch(0) + NEXTCONBK), 0);

	/* ABORT between two words: the next block loads in its own time. */
	put_cb(&s, 0x1040, PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 8, 0,
	       0x1060);
	put_cb(&s, 0x1060, SRC_INC | DEST_INC, 0x2000, 0x3100, 4, 0, 0);
	s.fifo.count = 2;
	fifo_dreq(&s.fifo);
	start(&s, 0, 0x1040); /* now 136: loads at 168, a word, then 172 */
	run(&s, 170);
	CHECK_EQ(s.fifo.reads, 1);
	wr(&s, ch(0) + CS, ABORT | ACTIVE);
	run(&s, 170 + 32 - 1);
	CHECK_EQ(rd(&s, ch(0) + TI), PERMAP(13) | SRC_DREQ | DEST_INC);
	CHECK_EQ(s.fifo.reads, 1);
	run(&s, 170 + 32);
	CHECK_EQ(rd(&s, ch(0) + TI), SRC_INC | DEST_INC);

	/* ABORT of the last block: ACTIVE clears. */
	put_cb(&s, 0x1080, PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 8, 0,
	       0);
	s.fifo.count = 0;
	fifo_dreq(&s.fifo);
	run(&s, 300); /* the copy of 0x1060 ends */
	wr(&s, ch(0) + CS, INT | END);
	start(&s, 0, 0x1080);
	run(&s, 1000);
	wr(&s, ch(0) + CS, ABORT | ACTIVE);
	CHECK_EQ(rd(&s, ch(0) + CS), 0); /* DREQ 13 is low */
	CHECK_EQ(rd(&s, ch(0) + CONBLK_AD), 0);
	wr(&s, ch(0) + CS, ABORT | ACTIVE); /* nothing loaded: nothing */
	CHECK_EQ(rd(&s, ch(0) + CS), ACTIVE);

	/* RESET before the control block loads: it never does. */
	wr(&s, ch(0) + CONBLK_AD, 0x1020);
	wr(&s, ch(0) + CS, RESET);
	run(&s, 2000);
	CHECK_EQ(rd(&s, ch(0) + TI), 0);

	/* RESET of a waiting channel: DREQ moves nothing after it. */
	put_cb(&s, 0x10a0, PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 8,
	       0x55, 0x10c0);
	start(&s, 0, 0x10a0);
	run(&s, 3000);
	CHECK_EQ(rd(&s, ch(0) + STRIDE), 0x55);
	wr(&s, ch(0) + CS, RESET);
	CHECK_EQ(rd(&s, ch(0) + TXFR_LEN), 0);
	CHECK_EQ(rd(&s, ch(0) + STRIDE), 0);
	CHECK_EQ(rd(&s, ch(0) + NEXTCONBK), 0);
	s.fifo.count = 2;
	fifo_dreq(&s.fifo);
	run(&s, 4000);
	CHECK_EQ(s.fifo.reads, 1);

	/* A board reset does the same for every channel, and ENABLE. */
	wr(&s, BASE + ENABLE, 0x1);
	start(&s, 0, 0x1080);
	run(&s, 5000);
	bcm2835_dma_reset(s.dma);
	CHECK_EQ(rd(&s, ch(0) + CS), 0);
	CHECK_EQ(rd(&s, ch(0) + CONBLK_AD), 0);
	CHECK_EQ(rd(&s, BASE + ENABLE), 0x7fff);
	teardown(&s);
}

/* The lines: one each for 0 to 10, one for 11 to 14; 15 has none. */
static void test_interrupts(void)
{
	static const unsigned channels[] = {1, 10, 11, 12, 14, 15};
	Setup s;

	setup(&s);
	for (unsigned i = 0; i < ARRAY_SIZE(channels); i++) {
		put_cb(&s, 0x1000 + 0x20 * i, SRC_INC | DEST_INC | INTEN, 0x2000,
		       0x3000, 4, 0, 0);
		start(&s, channels[i], 0x1000 + 0x20 * i);
	}
	run(&s, 1000);
	CHECK_EQ(rd(&s, BASE + INT_STATUS),
	         1u << 1 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 14 | 1u << 15);
	CHECK_EQ(irq_level(&s.irqs[1]), true);
	CHECK_EQ(irq_level(&s.irqs[10]), true);
	CHECK_EQ(irq_level(&s.irqs[11]), true);
	for (unsigned i = 0; i < BCM2835_DMA_NUM_IRQS; i++)
		if (i != 1 && i != 10 && i != 11)
			CHECK_EQ(irq_level(&s.irqs[i]), false);
	wr(&s, ch(11) + CS, INT);
	wr(&s, ch(12) + CS, INT);
	CHECK_EQ(irq_level(&s.irqs[11]), true); /* 14 still */
	wr(&s, ch(14) + CS, INT);
	CHECK_EQ(irq_level(&s.irqs[11]), false);
	wr(&s, ch(15) + CS, INT);
	CHECK_EQ(rd(&s, BASE + INT_STATUS), 1u << 1 | 1u << 10);
	teardown(&s);
}

/* What the model lacks, or the datasheet does not describe, stops it. */
static void test_stops(void)
{
	static const struct {
		uint32_t ti, src, dst, len; /* a control block at 0x1000 */
		unsigned channel;
		uint32_t cb;
		const char *message;
	} blocks[] = {
		{SRC_INC | DEST_INC, 0x2000, 0x3000, 4, 0, 0x1004,
	     "channel 0: the control block at 0x00001004 is not described: it is "
	     "not 256-bit aligned"},
		{SRC_INC | DEST_INC, 0x2000, 0x3000, 4, 0, 0x3f000000,
	     "channel 0: a control block at bus address 0x3f000000 is not "
	     "emulated"},
		{SRC_INC | DEST_INC, 0x2000, 0x3000, 4, 0, 0x7e202000,
	     "a control block at bus address 0x7e202000"},
		{SRC_INC | DEST_INC, 0x2000, 0x3000, 4, 0, 0x00020000,
	     "a control block at bus address 0x00020000"},
		{TDMODE, 0x2000, 0x3000, 4, 3, 0x1000,
	     "channel 3: the control block at 0x00001000 is not implemented "
	     "yet: 2D mode"},
		{SRC_IGNORE, 0x2000, 0x3000, 4, 0, 0x1000, "the ignore modes"},
		{DEST_IGNORE, 0x2000, 0x3000, 4, 0, 0x1000, "the ignore modes"},
		{SRC_INC | DEST_INC, 0x2000, 0x3000, 0, 0, 0x1000,
	     "is not described: a transfer of 0 bytes"},
		{SRC_INC | DEST_INC, 0x2000, 0x3000, 0x40000000, 0, 0x1000,
	     "a transfer of 0 bytes"}, /* bits 31:30 are not length */
		{SRC_WIDTH | DEST_INC, 0x2000, 0x3000, 16, 0, 0x1000,
	     "a 128-bit width on a side"},
		{DEST_WIDTH | SRC_INC, 0x2000, 0x3000, 16, 0, 0x1000,
	     "a 128-bit width on a side"},
		{DEST_WIDTH | SRC_INC | DEST_INC, FIFO_BUS, 0x3000, 16, 0, 0x1000,
	     "a 128-bit width on a side"},
		{PERMAP(5) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 4, 0, 0x1000,
	     "channel 0: DREQ 5 is not implemented yet: no modelled peripheral "
	     "drives it"},
		{PERMAP(5) | DEST_DREQ | SRC_INC, 0x2000, FIFO_BUS, 4, 0, 0x1000,
	     "DREQ 5"},
		{PERMAP(29) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 4, 0, 0x1000,
	     "DREQ 29 is not"},
		{PERMAP(1) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 4, 0, 0x1000,
	     "DREQ 1 is not"},
		{PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 6, 0, 0x1000,
	     "a paced transfer of bytes that are not whole 32-bit words"},
		{PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3002, 4, 0, 0x1000,
	     "not whole 32-bit words"},
		{PERMAP(13) | SRC_DREQ | SRC_INC, 0x2002, FIFO_BUS, 4, 0, 0x1000,
	     "not whole 32-bit words"},
		{DEST_INC, FIFO_BUS, 0x3000, 6, 0, 0x1000,
	     "a transfer of 6 bytes from 0x7e202040 to 0x00003000 is not "
	     "implemented yet: only 32-bit words go to or from a peripheral"},
		{DEST_INC, FIFO_BUS, 0x3001, 4, 0, 0x1000, "only 32-bit words"},
		{SRC_INC, 0x2001, FIFO_BUS, 4, 0, 0x1000, "only 32-bit words"},
		{SRC_INC | DEST_INC, 0x7e300000, 0x3000, 4, 0, 0x1000,
	     "channel 0: a read at bus address 0x7e300000 is not emulated"},
		{SRC_INC | DEST_INC, 0x2000, 0x7e300000, 4, 0, 0x1000,
	     "a write at bus address 0x7e300000"},
		{SRC_INC | DEST_INC, 0x3f000000, 0x3000, 4, 0, 0x1000,
	     "a read at bus address 0x3f000000"},
		{SRC_INC | DEST_INC, 0x7f000000, 0x3000, 4, 0, 0x1000,
	     "a read at bus address 0x7f000000"},
		{SRC_INC | DEST_INC, 0x2000, 0xff000000, 4, 0, 0x1000,
	     "a write at bus address 0xff000000"},
		{SRC_INC | DEST_INC, 0x0000fffc, 0x3000, 8, 0, 0x1000,
	     "a read at bus address 0x0000fffc"},
		{SRC_INC | DEST_INC, 0x2000, 0x3effffff, 2, 0, 0x1000,
	     "a write at bus address 0x3effffff"},
		{SRC_INC | DEST_INC, 0xbefffffe, 0x3000, 4, 0, 0x1000,
	     "a read at bus address 0xbefffffe"},
		{DEST_INC, 0x00020000, 0x3000, 4, 0, 0x1000,
	     "a read at bus address 0x00020000"},
		{SRC_INC, 0x2000, 0x00020000, 4, 0, 0x1000,
	     "a write at bus address 0x00020000"},
	};
	static const struct {
		bool write;
		uint64_t address;
		unsigned size;
		uint32_t value;
		const char *message;
	} regs[] = {
		{false, BASE + CS, 2, 0,
	     "bcm2835-dma: 2-byte read from offset 0x000 is not a 32-bit"},
		{false, BASE + ENABLE, 2, 0,
	     "2-byte read from offset 0xff0 is not a 32-bit"},
		{true, BASE + CONBLK_AD, 2, 0x1000,
	     "2-byte write to offset 0x004 is not a 32-bit"},
		{true, BASE + 0x102, 4, 0, "offset 0x102 is not a 32-bit"},
		{false, BASE + 0x024, 4, 0,
	     "4-byte read from offset 0x024 is not described by the datasheet"},
		{false, BASE + 0x718, 4, 0, "offset 0x718 is not described"},
		{true, BASE + 0xe18, 4, 0, "offset 0xe18 is not described"},
		{false, BASE + 0xf00, 4, 0, "offset 0xf00 is not described"},
		{false, BASE + 0xfe4, 4, 0, "offset 0xfe4 is not described"},
		{true, BASE + INT_STATUS, 4, 0,
	     "write to offset 0xfe0 is not described: INT_STATUS"},
		{true, BASE + 0xffc, 4, 0, "offset 0xffc is not described"},
		{true, BASE + TI, 4, 0,
	     "4-byte write to offset 0x008 is not emulated: the control block "
	     "loads it"},
		{true, BASE + 0x30c, 4, 0, "offset 0x30c is not emulated"},
		{true, BASE + 0x410, 4, 0, "offset 0x410 is not emulated"},
		{true, BASE + 0x514, 4, 0, "offset 0x514 is not emulated"},
		{true, BASE + 0x618, 4, 0, "offset 0x618 is not emulated"},
		{false, BASE15 + 0x24, 4, 0,
	     "read from channel 15 offset 0x024 is not described"},
		{true, BASE15 + 0x24, 4, 0, "write to channel 15 offset 0x024"},
		{false, BASE15 + 2, 4, 0, "read from channel 15 offset 0x002 is not"},
		{true, BASE15 + CONBLK_AD, 1, 0x20,
	     "1-byte write to channel 15 offset 0x004"},
		{true, BASE15 + TI, 4, 0, "the control block loads it"},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(blocks); i++) {
		Setup s;
		unsigned n = blocks[i].channel;

		setup(&s);
		CHECK_EQ(bus_write(s.m->bus, 0x2000, 4, 0x12345678), BUS_OK);
		CHECK_EQ(bus_write(s.m->bus, 0x3000, 4, 0x5a5a5a5a), BUS_OK);
		put_cb(&s, 0x1000, blocks[i].ti, blocks[i].src, blocks[i].dst,
		       blocks[i].len, 0, 0);
		wr(&s, ch(n) + CONBLK_AD, blocks[i].cb);
		bus_write(s.m->bus, ch(n) + CS, 4, ACTIVE);
		run(&s, 10000);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		if (strstr(s.m->stop_message, blocks[i].message) == NULL) {
			fprintf(stderr, "block %u: %s\n", i, s.m->stop_message);
			test_failures++;
		}
		CHECK_EQ(rd(&s, 0x3000), 0x5a5a5a5a); /* nothing was written */
		CHECK_EQ(rd(&s, ch(n) + CS) & END, 0);
		CHECK_EQ(rd(&s, ch(n) + TXFR_LEN),
		         blocks[i].cb == 0x1000 ? blocks[i].len & 0x3fffffff : 0);
		CHECK_EQ(s.first.reads + s.local.reads, 0);
		teardown(&s);
	}
	for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
		Setup s;
		uint64_t value = 0;

		setup(&s);
		if (regs[i].write)
			bus_write(s.m->bus, regs[i].address, regs[i].size, regs[i].value);
		else
			bus_read(s.m->bus, regs[i].address, regs[i].size, &value);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		if (strstr(s.m->stop_message, regs[i].message) == NULL) {
			fprintf(stderr, "reg %u: %s\n", i, s.m->stop_message);
			test_failures++;
		}
		CHECK_EQ(value, 0);
		CHECK_EQ(rd(&s, BASE + CONBLK_AD), 0);
		CHECK_EQ(rd(&s, BASE15 + CONBLK_AD), 0);
		teardown(&s);
	}
}

/* ENABLE: a channel it turns off does not run, nor is turned off running. */
static void test_enable(void)
{
	Setup s;

	setup(&s);
	put_cb(&s, 0x1000, PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 4, 0,
	       0);
	wr(&s, BASE + ENABLE, 0xfffffffe);
	CHECK_EQ(rd(&s, BASE + ENABLE), 0x7ffe);
	wr(&s, ch(0) + CONBLK_AD, 0x1000);
	bus_write(s.m->bus, ch(0) + CS, 4, ACTIVE);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message,
	             "4-byte write to offset 0x000 is not described: channel 0 "
	             "runs while ENABLE turns it off") != NULL);
	s.m->stop = MACHINE_STOP_NONE;
	run(&s, 1000);
	CHECK_EQ(rd(&s, ch(0) + TI), 0); /* nothing loaded */
	teardown(&s);

	setup(&s);
	put_cb(&s, 0x1000, PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 4, 0,
	       0);
	wr(&s, BASE + ENABLE, 0x7ffe);
	wr(&s, ch(0) + CS, ACTIVE); /* no control block: nothing runs */
	bus_write(s.m->bus, ch(0) + CONBLK_AD, 4, 0x1000);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message, "offset 0x004 is not described: channel "
	                                "0 runs while ENABLE") != NULL);
	teardown(&s);

	setup(&s);
	put_cb(&s, 0x1000, PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 4, 0,
	       0);
	wr(&s, BASE + ENABLE, 0x3fff);
	wr(&s, ch(14) + CONBLK_AD, 0x1000);
	bus_write(s.m->bus, ch(14) + CS, 4, ACTIVE);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message, "channel 14 runs while ENABLE") != NULL);
	teardown(&s);

	/* Channels that run keep their bits; turning one off stops. */
	setup(&s);
	put_cb(&s, 0x1000, PERMAP(13) | SRC_DREQ | DEST_INC, FIFO_BUS, 0x3000, 4, 0,
	       0);
	start(&s, 14, 0x1000);
	start(&s, 0, 0x1000);
	run(&s, 1000);
	wr(&s, BASE + ENABLE, 0x7fff);
	bus_write(s.m->bus, BASE + ENABLE, 4, 0x3fff);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message, "offset 0xff0 is not described: it turns "
	                                "off a channel that runs") != NULL);
	CHECK_EQ(rd(&s, BASE + ENABLE), 0x7fff);
	s.m->stop = MACHINE_STOP_NONE;
	bus_write(s.m->bus, BASE + ENABLE, 4, 0x7ffe);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK_EQ(rd(&s, BASE + ENABLE), 0x7fff);
	s.m->stop = MACHINE_STOP_NONE;
	bus_write(s.m->bus, ch(14) + CONBLK_AD, 4, 0x1020);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message, "4-byte write to offset 0xe04 is not "
	                                "described: a control block is "
	                                "loaded") != NULL);
	CHECK_EQ(rd(&s, ch(14) + CONBLK_AD), 0x1000);
	teardown(&s);
}

/* Destroying the controller cancels what its channels have pending. */
static void test_destroy_busy(void)
{
	Setup s;

	setup(&s);
	put_cb(&s, 0x1000, SRC_INC | DEST_INC, 0x2000, 0x3000, 4, 0, 0);
	start(&s, 0, 0x1000);
	start(&s, 15, 0x1000);
	CHECK(event_queue_next(&s.m->events) != EVENT_NEVER);
	bcm2835_dma_destroy(s.dma);
	CHECK_EQ(event_queue_next(&s.m->events), EVENT_NEVER);
	machine_destroy(s.m);
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_copy);
	RUN_TEST(test_wide);
	RUN_TEST(test_chain);
	RUN_TEST(test_lite);
	RUN_TEST(test_paced_read);
	RUN_TEST(test_paced_write);
	RUN_TEST(test_peripheral_words);
	RUN_TEST(test_device_stops);
	RUN_TEST(test_long_copy);
	RUN_TEST(test_abort_and_reset);
	RUN_TEST(test_interrupts);
	RUN_TEST(test_stops);
	RUN_TEST(test_enable);
	RUN_TEST(test_destroy_busy);
	return test_summary();
}
