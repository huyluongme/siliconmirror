/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for SDHOST (src/hw/sd/bcm2835_sdhost.c), with an SD card in
 * its slot, driven as U-Boot's and Linux's drivers drive it. Times
 * follow the SD Physical Layer specification's bit counts (4.9, 4.12).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "block_mem.h"
#include "core/bus.h"
#include "core/event.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/sd/bcm2835_sdhost.h"
#include "hw/sd/sd_card.h"
#include "test.h"
#include "util/byteorder.h"
#include "util/macros.h"
#include "util/muldiv.h"

#define BASE 0x3f202000
#define CORE_HZ 250000000
#define CARD_SIZE (1024 * 1024)

enum {
	SDCMD = 0x00,
	SDARG = 0x04,
	SDTOUT = 0x08,
	SDCDIV = 0x0c,
	SDRSP0 = 0x10,
	SDRSP1 = 0x14,
	SDRSP2 = 0x18,
	SDRSP3 = 0x1c,
	SDHSTS = 0x20,
	SDVDD = 0x30,
	SDEDM = 0x34,
	SDHCFG = 0x38,
	SDHBCT = 0x3c,
	SDDATA = 0x40,
	SDHBLC = 0x50,
};

#define NEW 0x8000u
#define FAIL 0x4000u
#define BUSYWAIT 0x800u
#define NO_RESPONSE 0x400u
#define LONG 0x200u
#define WRITE 0x80u
#define READ 0x40u

#define DATA_FLAG 0x001u
#define CRC7_ERROR 0x010u
#define CRC16_ERROR 0x020u
#define CMD_TIME_OUT 0x040u
#define REW_TIME_OUT 0x080u
#define BLOCK_IRPT 0x200u
#define BUSY_IRPT 0x400u

#define REL_CMD_LINE 0x001u
#define WIDE_INT_BUS 0x002u
#define WIDE_EXT_BUS 0x004u
#define SLOW_CARD 0x008u
#define DATA_IRPT_EN 0x010u
#define SDIO_IRPT_EN 0x020u
#define BLOCK_IRPT_EN 0x100u
#define BUSY_IRPT_EN 0x400u

enum {
	IDENTMODE = 0x0,
	DATAMODE = 0x1,
	READDATA = 0x2,
	WRITEDATA = 0x3,
	READWAIT = 0x4,
	WRITEWAIT1 = 0x7,
	POWERDOWN = 0x8,
	WRITESTART1 = 0xa,
};

typedef struct Setup {
	Machine *m;
	BlockMem disk;
	SdCard *card;
	bool connected;
	IrqLine irq;
	Bcm2835Sdhost *host;
} Setup;

static bool slot_connected(void *ctx)
{
	return *(bool *)ctx;
}

static void setup_with(Setup *s, bool card)
{
	char error[128];

	memset(s, 0, sizeof(*s));
	s->m = machine_create();
	CHECK(s->m != NULL);
	block_mem_init(&s->disk, CARD_SIZE);
	for (uint32_t i = 0; i < CARD_SIZE; i++)
		s->disk.data[i] = (uint8_t)(i * 13 + (i >> 9));
	if (card) {
		s->card = sd_card_create(s->m, &s->disk.dev, error, sizeof(error));
		CHECK(s->card != NULL);
		sd_card_power(s->card, true);
	}
	s->connected = true;
	irq_init(&s->irq, NULL, NULL, 0);
	s->host = bcm2835_sdhost_create(
		s->m, CORE_HZ,
		&(Bcm2835SdhostSlot){s->card, slot_connected, &s->connected}, &s->irq);
	CHECK(s->host != NULL);
	CHECK_EQ(bcm2835_sdhost_map(s->host, s->m->bus, "sdhost", BASE), BUS_OK);
}

static void setup(Setup *s)
{
	setup_with(s, true);
}

static void teardown(Setup *s)
{
	bcm2835_sdhost_destroy(s->host);
	if (s->card != NULL)
		sd_card_destroy(s->card);
	machine_destroy(s->m);
	block_mem_free(&s->disk);
}

static uint32_t rd(Setup *s, uint64_t offset)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, BASE + offset, 4, &value), BUS_OK);
	return (uint32_t)value;
}

static void wr(Setup *s, uint64_t offset, uint32_t value)
{
	CHECK_EQ(bus_write(s->m->bus, BASE + offset, 4, value), BUS_OK);
}

static uint64_t now(Setup *s)
{
	return s->m->events.now;
}

static void run_until(Setup *s, uint64_t t)
{
	event_queue_run_until(&s->m->events, t);
}

/* The time of clocks at the clock of divisor. */
static uint64_t clocks(uint64_t n, uint32_t divisor)
{
	return mul_div_ceil(n, 1000000000, CORE_HZ / divisor);
}

static unsigned fsm(Setup *s)
{
	return rd(s, SDEDM) & 0xf;
}

static unsigned fifo(Setup *s)
{
	return (rd(s, SDEDM) >> 4) & 0x1f;
}

/* Send a command and wait until NEW clears; returns SDCMD. */
static uint32_t command(Setup *s, uint32_t cmd, uint32_t arg)
{
	wr(s, SDARG, arg);
	wr(s, SDCMD, cmd | NEW);
	for (int i = 0; i < 200000 && (rd(s, SDCMD) & NEW); i++)
		run_until(s, now(s) + 1000);
	CHECK_EQ(rd(s, SDCMD) & NEW, 0);
	return rd(s, SDCMD);
}

/* Power on and the card identified, selected and in 1-bit mode. */
static void identify(Setup *s)
{
	uint32_t c;

	wr(s, SDVDD, 1);
	wr(s, SDCDIV, 0x7ff);
	wr(s, SDTOUT, 0xf00000);
	command(s, 0 | NO_RESPONSE, 0);
	command(s, 8, 0x1aa);
	CHECK_EQ(rd(s, SDRSP0), 0x1aa);
	for (int i = 0; i < 100; i++) {
		command(s, 55, 0);
		command(s, 41, 0x40ff8000);
		if (rd(s, SDRSP0) & (1u << 31))
			break;
		run_until(s, now(s) + 100000);
	}
	CHECK(rd(s, SDRSP0) & (1u << 31));
	command(s, 2 | LONG, 0);
	c = command(s, 3, 0);
	CHECK_EQ(c & FAIL, 0);
	CHECK_EQ(rd(s, SDRSP0) >> 16, 0xaaaa);
	command(s, 7 | BUSYWAIT, 0xaaaa0000);
	wr(s, SDHSTS, BUSY_IRPT);
	wr(s, SDHCFG, SLOW_CARD);
	wr(s, SDCDIV, 3); /* 50 MHz with SLOW_CARD: 250 / 5 */
}

/* Reset values: SDCDIV 507, the power off. */
static void test_reset_values(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, SDCMD), 0);
	CHECK_EQ(rd(&s, SDARG), 0);
	CHECK_EQ(rd(&s, SDTOUT), 0);
	CHECK_EQ(rd(&s, SDCDIV), 507);
	CHECK_EQ(rd(&s, SDHSTS), 0);
	CHECK_EQ(rd(&s, SDVDD), 0);
	CHECK_EQ(rd(&s, SDEDM), POWERDOWN);
	CHECK_EQ(rd(&s, SDHCFG), 0);
	CHECK_EQ(rd(&s, SDHBCT), 512);
	CHECK_EQ(rd(&s, SDHBLC), 0);
	for (unsigned i = 0; i < 4; i++)
		CHECK_EQ(rd(&s, SDRSP0 + 4 * i), 0);
	wr(&s, SDVDD, 3);
	CHECK_EQ(rd(&s, SDVDD), 1);
	CHECK_EQ(fsm(&s), IDENTMODE);
	CHECK(!irq_level(&s.irq));
	teardown(&s);
}

/* A command's time on the bus, at SDCDIV + 2 core clocks a bit. */
static void test_command_timing(void)
{
	Setup s;
	uint64_t start;

	setup(&s);
	wr(&s, SDVDD, 1);
	/* No response: 48 bits, then 8 clocks. */
	start = now(&s);
	wr(&s, SDCMD, NEW | NO_RESPONSE | 0);
	CHECK_EQ(rd(&s, SDCMD), NEW | NO_RESPONSE);
	run_until(&s, start + clocks(56, 509) - 1);
	CHECK(rd(&s, SDCMD) & NEW);
	run_until(&s, start + clocks(56, 509));
	CHECK_EQ(rd(&s, SDCMD), NO_RESPONSE);
	/* A short response: 48 bits, Ncr of 2, 48 bits. */
	wr(&s, SDCDIV, 8);
	wr(&s, SDARG, 0x1a5);
	start = now(&s);
	wr(&s, SDCMD, NEW | 8);
	run_until(&s, start + clocks(98, 10) - 1);
	CHECK(rd(&s, SDCMD) & NEW);
	CHECK_EQ(rd(&s, SDRSP0), 0); /* not yet */
	run_until(&s, start + clocks(98, 10));
	CHECK_EQ(rd(&s, SDCMD), 8);
	CHECK_EQ(rd(&s, SDRSP0), 0x1a5);
	CHECK_EQ(rd(&s, SDARG), 0x1a5);
	/* No response: CMD_TIME_OUT after 64 clocks, and FAIL. */
	wr(&s, SDARG, 0x2aa); /* 1.8 V: the card does not answer */
	start = now(&s);
	wr(&s, SDCMD, NEW | 8);
	run_until(&s, start + clocks(112, 10) - 1);
	CHECK_EQ(rd(&s, SDHSTS), 0);
	run_until(&s, start + clocks(112, 10));
	CHECK_EQ(rd(&s, SDCMD), FAIL | 8);
	CHECK_EQ(rd(&s, SDHSTS), CMD_TIME_OUT);
	/* Clearing takes 1 bits; FAIL clears with the next command. */
	wr(&s, SDHSTS, 0xffffffff);
	CHECK_EQ(rd(&s, SDHSTS), 0);
	CHECK_EQ(command(&s, 8, 0x1aa), 8);
	/* A long response where the card gives a short one: CRC7. */
	CHECK_EQ(command(&s, 8 | LONG, 0x1aa), FAIL | LONG | 8);
	CHECK_EQ(rd(&s, SDHSTS), CRC7_ERROR);
	wr(&s, SDHSTS, CRC7_ERROR);
	teardown(&s);
}

/* Without a card, or without the pins: every command times out. */
static void test_no_card(void)
{
	Setup s;

	setup_with(&s, false);
	wr(&s, SDVDD, 1);
	CHECK_EQ(command(&s, 8, 0x1aa), FAIL | 8);
	CHECK_EQ(rd(&s, SDHSTS), CMD_TIME_OUT);
	CHECK_EQ(command(&s, 0 | NO_RESPONSE, 0), NO_RESPONSE);
	teardown(&s);

	setup(&s);
	wr(&s, SDVDD, 1);
	s.connected = false;
	CHECK_EQ(command(&s, 8, 0x1aa), FAIL | 8);
	s.connected = true;
	CHECK_EQ(command(&s, 8, 0x1aa), 8);
	teardown(&s);
}

/* Identification, and the long responses in SDRSP0 to SDRSP3. */
static void test_identification(void)
{
	Setup s;
	uint64_t start;

	setup(&s);
	wr(&s, SDVDD, 1);
	wr(&s, SDHCFG, SLOW_CARD); /* one clock for both modes */
	command(&s, 0 | NO_RESPONSE, 0);
	CHECK_EQ(fsm(&s), IDENTMODE);
	command(&s, 8, 0x1aa);
	command(&s, 55, 0);
	command(&s, 41, 0x40ff8000);
	run_until(&s, now(&s) + 1000000);
	command(&s, 55, 0);
	command(&s, 41, 0x40ff8000);
	CHECK_EQ(rd(&s, SDRSP0), 0x80ff8000);
	/* The CID: bits 127:96 in SDRSP3, 31:0 in SDRSP0. */
	CHECK_EQ(command(&s, 2 | LONG, 0), LONG | 2);
	CHECK_EQ(rd(&s, SDRSP3) >> 24, 0x00); /* MID */
	CHECK_EQ(rd(&s, SDRSP3) & 0xffffff, 'S' << 16 | 'M' << 8 | 'S');
	CHECK_EQ(rd(&s, SDRSP2), 'M' << 24 | 'S' << 16 | 'D' << 8 | '1');
	CHECK_EQ(rd(&s, SDRSP1), 0x105ca1ab); /* PRV, then PSN */
	CHECK_EQ(rd(&s, SDRSP0) & 1, 1);      /* the end bit */
	CHECK_EQ(fsm(&s), IDENTMODE);
	command(&s, 3, 0);
	CHECK_EQ(fsm(&s), DATAMODE); /* identification is over */
	/* CMD7 with BUSYWAIT: NEW stays until the busy ends. */
	wr(&s, SDARG, 0xaaaa0000);
	start = now(&s);
	wr(&s, SDCMD, NEW | BUSYWAIT | 7);
	run_until(&s, start + clocks(98, 509));
	CHECK(rd(&s, SDCMD) & NEW);
	CHECK_EQ(rd(&s, SDHSTS), 0);
	run_until(&s, start + clocks(98, 509) + clocks(8, 509));
	CHECK_EQ(rd(&s, SDCMD), BUSYWAIT | 7);
	CHECK_EQ(rd(&s, SDHSTS), BUSY_IRPT);
	CHECK(!irq_level(&s.irq));
	wr(&s, SDHCFG, BUSY_IRPT_EN);
	CHECK(irq_level(&s.irq));
	wr(&s, SDHSTS, BUSY_IRPT);
	CHECK(!irq_level(&s.irq));
	/* CMD0 goes back to identification mode. */
	command(&s, 0 | NO_RESPONSE, 0);
	CHECK_EQ(fsm(&s), IDENTMODE);
	teardown(&s);
}

/* Read the words of a block from the FIFO as they come. */
static void read_block(Setup *s, uint32_t *words, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		for (int k = 0; k < 10000 && fifo(s) == 0; k++)
			run_until(s, now(s) + 100);
		words[i] = rd(s, SDDATA);
	}
}

/* A single block, PIO, with the FIFO and its flags. */
static void test_read(void)
{
	Setup s;
	uint32_t words[128];
	uint64_t start;

	setup(&s);
	identify(&s);
	wr(&s, SDHBCT, 512);
	wr(&s, SDHBLC, 1);
	wr(&s, SDEDM, 4 << 14 | 4 << 9);
	start = now(&s);
	wr(&s, SDARG, 1024);
	wr(&s, SDCMD, NEW | READ | 17);
	run_until(&s, start + clocks(98, 5));
	CHECK_EQ(rd(&s, SDCMD), READ | 17);
	CHECK_EQ(fsm(&s), READDATA);
	/* Each word: 32 clocks on 1 line, after Nac and the start bit. */
	run_until(&s, start + clocks(98 + 3 + 32, 5) - 1);
	CHECK_EQ(fifo(&s), 0);
	CHECK_EQ(rd(&s, SDHSTS) & DATA_FLAG, 0);
	run_until(&s, start + clocks(98 + 3 + 32, 5));
	CHECK_EQ(fifo(&s), 1);
	CHECK_EQ(rd(&s, SDHSTS) & DATA_FLAG, 0); /* threshold 4 */
	run_until(&s, start + clocks(98 + 3 + 4 * 32, 5));
	CHECK_EQ(fifo(&s), 4);
	CHECK_EQ(rd(&s, SDHSTS) & DATA_FLAG, DATA_FLAG);
	wr(&s, SDHCFG, SLOW_CARD | DATA_IRPT_EN);
	CHECK(irq_level(&s.irq));
	wr(&s, SDHCFG, SLOW_CARD);
	/* The FIFO fills; the clock waits. */
	run_until(&s, start + clocks(98 + 3 + 40 * 32, 5));
	CHECK_EQ(fifo(&s), 16);
	CHECK_EQ(fsm(&s), READWAIT);
	CHECK_EQ(rd(&s, SDEDM), 4u << 14 | 4u << 9 | 16 << 4 | READWAIT);
	read_block(&s, words, 128);
	CHECK(memcmp(words, s.disk.data + 1024, 512) == 0);
	CHECK_EQ(fifo(&s), 0);
	CHECK_EQ(fsm(&s), READDATA); /* the CRC and end bit to come */
	run_until(&s, now(&s) + clocks(17, 5));
	CHECK_EQ(fsm(&s), DATAMODE);
	CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT);
	wr(&s, SDHCFG, SLOW_CARD | BLOCK_IRPT_EN);
	CHECK(irq_level(&s.irq));
	wr(&s, SDHSTS, BLOCK_IRPT);
	CHECK(!irq_level(&s.irq));
	teardown(&s);
}

/* Several blocks, then CMD12; a read the card refuses times out. */
static void test_multiple_read(void)
{
	Setup s;
	uint32_t words[128];
	uint64_t start;

	setup(&s);
	identify(&s);
	wr(&s, SDHBCT, 512);
	wr(&s, SDHBLC, 3);
	command(&s, READ | 18, 2048);
	for (unsigned b = 0; b < 3; b++) {
		read_block(&s, words, 128);
		CHECK(memcmp(words, s.disk.data + 2048 + 512 * b, 512) == 0);
	}
	run_until(&s, now(&s) + 100000);
	CHECK_EQ(fsm(&s), DATAMODE);
	CHECK_EQ(command(&s, BUSYWAIT | 12, 0), BUSYWAIT | 12);
	CHECK_EQ(fifo(&s), 0);

	/* Out of range: a response, then no data until SDTOUT clocks. */
	wr(&s, SDTOUT, 1000);
	wr(&s, SDHSTS, 0x7f8);
	wr(&s, SDHBLC, 1);
	start = now(&s);
	wr(&s, SDARG, CARD_SIZE);
	wr(&s, SDCMD, NEW | READ | 17);
	run_until(&s, start + clocks(98, 5));
	CHECK_EQ(rd(&s, SDCMD), READ | 17);
	CHECK(rd(&s, SDRSP0) & (1u << 31));
	CHECK_EQ(fsm(&s), READDATA);
	run_until(&s, start + clocks(98, 5) + clocks(1000, 5) - 1);
	CHECK_EQ(rd(&s, SDHSTS), 0);
	run_until(&s, start + clocks(98, 5) + clocks(1000, 5));
	CHECK_EQ(rd(&s, SDHSTS), REW_TIME_OUT);
	CHECK_EQ(fsm(&s), DATAMODE);
	teardown(&s);
}

/* Writes: the Arm fills the FIFO; the card programs each block. */
static void test_write(void)
{
	Setup s;
	uint32_t words[128];
	unsigned written = 0;

	setup(&s);
	identify(&s);
	for (unsigned i = 0; i < 128; i++)
		words[i] = 0xa5a50000u + i;
	wr(&s, SDHBCT, 512);
	wr(&s, SDHBLC, 2);
	wr(&s, SDEDM, 4 << 14 | 4 << 9);
	command(&s, WRITE | 25, 4096);
	CHECK_EQ(fsm(&s), WRITESTART1);
	CHECK_EQ(rd(&s, SDHSTS) & DATA_FLAG, DATA_FLAG); /* room for 4 */
	for (unsigned b = 0; b < 2; b++) {
		for (unsigned i = 0; i < 128; i++) {
			for (int k = 0; k < 10000 && fifo(&s) == 16; k++)
				run_until(&s, now(&s) + 100);
			wr(&s, SDDATA, words[i] + 0x100 * b);
			written++;
		}
		CHECK(fsm(&s) == WRITEDATA || fsm(&s) == WRITEWAIT1);
		for (int k = 0; k < 10000 && !(rd(&s, SDHSTS) & BLOCK_IRPT); k++)
			run_until(&s, now(&s) + 100);
		CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT | (b == 0 ? DATA_FLAG : 0));
		wr(&s, SDHSTS, BLOCK_IRPT);
	}
	CHECK_EQ(written, 256);
	CHECK_EQ(fsm(&s), DATAMODE);
	CHECK_EQ(command(&s, BUSYWAIT | 12, 0), BUSYWAIT | 12);
	for (unsigned b = 0; b < 2; b++)
		for (unsigned i = 0; i < 128; i++)
			CHECK_EQ(load_le32(s.disk.data + 4096 + 512 * b + 4 * i),
			         words[i] + 0x100 * b);

	/* A single block, read back through the FIFO. */
	wr(&s, SDHBLC, 1);
	command(&s, WRITE | 24, 8192);
	for (unsigned i = 0; i < 128; i++) {
		for (int k = 0; k < 10000 && fifo(&s) == 16; k++)
			run_until(&s, now(&s) + 100);
		wr(&s, SDDATA, ~words[i]);
	}
	for (int k = 0; k < 10000 && !(rd(&s, SDHSTS) & BLOCK_IRPT); k++)
		run_until(&s, now(&s) + 100);
	wr(&s, SDHSTS, BLOCK_IRPT);
	CHECK_EQ(fsm(&s), DATAMODE);
	CHECK_EQ(command(&s, READ | 17, 8192), READ | 17);
	{
		uint32_t back[128];

		read_block(&s, back, 128);
		for (unsigned i = 0; i < 128; i++)
			CHECK_EQ(back[i], ~words[i]);
	}
	teardown(&s);
}

/* 4 data lines, and the card's width that must match. */
static void test_bus_width(void)
{
	Setup s;
	uint32_t words[128];
	uint64_t start;

	setup(&s);
	identify(&s);
	wr(&s, SDHBCT, 512);
	wr(&s, SDHBLC, 1);
	/* The host on 4 lines, the card on 1: the CRC fails. */
	wr(&s, SDHCFG, SLOW_CARD | WIDE_EXT_BUS);
	command(&s, READ | 17, 0);
	read_block(&s, words, 128);
	run_until(&s, now(&s) + 100000);
	CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT | CRC16_ERROR);
	wr(&s, SDHSTS, 0x7f8);
	/* ACMD6 sets the card to 4 lines; a word takes 8 clocks. */
	command(&s, 55, 0xaaaa0000);
	command(&s, 6, 2);
	start = now(&s);
	wr(&s, SDARG, 512);
	wr(&s, SDCMD, NEW | READ | 17);
	run_until(&s, start + clocks(98 + 3 + 8, 5) - 1);
	CHECK_EQ(fifo(&s), 0);
	run_until(&s, start + clocks(98 + 3 + 8, 5));
	CHECK_EQ(fifo(&s), 1);
	read_block(&s, words, 128);
	CHECK(memcmp(words, s.disk.data + 512, 512) == 0);
	run_until(&s, now(&s) + 100000);
	CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT);
	teardown(&s);
}

/* Without SLOW_CARD, data mode uses the low 3 bits of SDCDIV. */
static void test_clock(void)
{
	Setup s;
	uint64_t start;

	setup(&s);
	identify(&s);
	wr(&s, SDCDIV, 0x7f9); /* low bits 1: 250 / 3 MHz */
	wr(&s, SDHCFG, 0);
	start = now(&s);
	command(&s, 13, 0xaaaa0000);
	CHECK_EQ(now(&s) - start <= clocks(98, 3) + 1000, 1);
	wr(&s, SDHCFG, SLOW_CARD);
	start = now(&s);
	wr(&s, SDARG, 0xaaaa0000);
	wr(&s, SDCMD, NEW | 13);
	run_until(&s, start + clocks(98, 0x7fb) - 1);
	CHECK(rd(&s, SDCMD) & NEW);
	run_until(&s, start + clocks(98, 0x7fb));
	CHECK_EQ(rd(&s, SDCMD) & NEW, 0);
	teardown(&s);
}

/* FORCE_DATA_MODE ends a transfer; SDVDD off and a reset too. */
static void test_abort(void)
{
	Setup s;

	setup(&s);
	identify(&s);
	wr(&s, SDHBCT, 512);
	wr(&s, SDHBLC, 1);
	command(&s, READ | 17, 0);
	run_until(&s, now(&s) + 20000);
	CHECK_EQ(fsm(&s), READWAIT); /* the FIFO is full */
	CHECK(fifo(&s) > 0);
	wr(&s, SDEDM, rd(&s, SDEDM) | 1u << 19);
	CHECK_EQ(fsm(&s), DATAMODE);
	CHECK_EQ(fifo(&s), 0);
	run_until(&s, now(&s) + 100000);
	CHECK_EQ(fifo(&s), 0);

	command(&s, BUSYWAIT | 12, 0);
	command(&s, READ | 17, 0);
	wr(&s, SDVDD, 0);
	CHECK_EQ(fsm(&s), POWERDOWN);
	wr(&s, SDVDD, 1);
	run_until(&s, now(&s) + 100000);
	CHECK_EQ(fsm(&s), DATAMODE);

	wr(&s, SDARG, 0xaaaa0000);
	wr(&s, SDCMD, NEW | 13);
	bcm2835_sdhost_reset(s.host);
	run_until(&s, now(&s) + 1000000);
	CHECK_EQ(rd(&s, SDCMD), 0);
	CHECK_EQ(rd(&s, SDRSP0), 0);
	CHECK_EQ(rd(&s, SDEDM), POWERDOWN);
	teardown(&s);
}

/* What the drivers never do stops the machine. */
static void test_stops(void)
{
	static const struct {
		bool write;
		uint64_t offset;
		unsigned size;
		uint32_t value;
		const char *message;
	} stops[] = {
		{false, SDCMD, 2, 0, "2-byte read of offset 0x00 is not a 32-bit"},
		{true, SDARG, 1, 0, "1-byte write to offset 0x04 is not a 32-bit"},
		{false, 0x24, 4, 0, "offset 0x24 is not a register the drivers use"},
		{true, 0x44, 4, 0, "offset 0x44 is not a register the drivers use"},
		{false, 0x22, 4, 0, "offset 0x22 is not a 32-bit register read"},
		{false, SDDATA, 4, 0,
	     "offset 0x40 is not emulated: the FIFO is "
	     "empty"},
		{true, SDCMD, 4, 0x1000, "offset 0x00 sets bits that the drivers"},
		{true, SDCMD, 4, NEW | 8, "SDVDD has the power off"},
		{true, SDEDM, 4, 1u << 20, "offset 0x34 sets bits that the drivers"},
		{true, SDHCFG, 4, 0x80, "offset 0x38 sets bits that the drivers"},
		{true, SDHBCT, 4, 513, "not whole words of up to 512 bytes"},
		{true, SDHBCT, 4, 6, "not whole words of up to 512 bytes"},
		{true, SDRSP1, 4, 0, "offset 0x14 is not a register the drivers"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(stops); i++) {
		Setup s;
		uint64_t value = 0;

		setup(&s);
		if (stops[i].write)
			bus_write(s.m->bus, BASE + stops[i].offset, stops[i].size,
			          stops[i].value);
		else
			bus_read(s.m->bus, BASE + stops[i].offset, stops[i].size, &value);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, "bcm2835-sdhost: ") != NULL);
		if (strstr(s.m->stop_message, stops[i].message) == NULL)
			fprintf(stderr, "%s\n", s.m->stop_message);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		teardown(&s);
	}

	/* A command while one runs; the FIFO full; SDHBCT 0. */
	Setup s;

	setup(&s);
	wr(&s, SDVDD, 1);
	wr(&s, SDCMD, NEW | 8);
	bus_write(s.m->bus, BASE + SDCMD, 4, NEW | 8);
	CHECK(strstr(s.m->stop_message, "the last command has not finished"));
	s.m->stop = MACHINE_STOP_NONE;
	for (unsigned i = 0; i < 16; i++)
		wr(&s, SDDATA, i);
	bus_write(s.m->bus, BASE + SDDATA, 4, 16);
	CHECK(strstr(s.m->stop_message, "the FIFO is full"));
	s.m->stop = MACHINE_STOP_NONE;
	run_until(&s, now(&s) + 10000000);
	wr(&s, SDHBCT, 0);
	wr(&s, SDARG, 0x1aa);
	wr(&s, SDCMD, NEW | READ | 8);
	run_until(&s, now(&s) + 10000000);
	CHECK(strstr(s.m->stop_message, "4-byte write to offset 0x00 is not "
	                                "emulated: a transfer with SDHBCT 0"));
	teardown(&s);

	/* A command while data moves, but CMD12. */
	setup(&s);
	identify(&s);
	wr(&s, SDHBCT, 512);
	wr(&s, SDHBLC, 2);
	command(&s, READ | 18, 0);
	wr(&s, SDCMD, NEW | 13);
	CHECK(strstr(s.m->stop_message, "a command other than CMD12 while data "
	                                "moves"));
	CHECK(fsm(&s) == READDATA || fsm(&s) == READWAIT);
	s.m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(command(&s, BUSYWAIT | 12, 0), BUSYWAIT | 12);
	CHECK_EQ(fsm(&s), DATAMODE);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* Clocks at 50 MHz: SLOW_CARD and SDCDIV 3, as identify() leaves them. */
#define CLK(n) ((uint64_t)(n) * 20)

static uint64_t start_command(Setup *s, uint32_t cmd, uint32_t arg)
{
	wr(s, SDARG, arg);
	wr(s, SDCMD, cmd | NEW);
	return now(s);
}

/* Long responses take 136 bits; a length mismatch fails as CRC7. */
static void test_response_lengths(void)
{
	Setup s;
	uint64_t t;

	setup(&s);
	wr(&s, SDVDD, 1);
	wr(&s, SDHCFG, SLOW_CARD);
	wr(&s, SDCDIV, 3);
	command(&s, 0 | NO_RESPONSE, 0);
	command(&s, 8, 0x1aa);
	command(&s, 55, 0);
	command(&s, 41, 0x40ff8000);
	run_until(&s, now(&s) + 1000000);
	command(&s, 55, 0);
	command(&s, 41, 0x40ff8000);
	/* CMD2: 48 bits, Ncr, 136 bits. */
	t = start_command(&s, 2 | LONG, 0);
	run_until(&s, t + CLK(48 + 2 + 136) - 1);
	CHECK(rd(&s, SDCMD) & NEW);
	run_until(&s, t + CLK(48 + 2 + 136));
	CHECK_EQ(rd(&s, SDCMD), LONG | 2);
	command(&s, 3, 0);
	/* CMD9 answers 136 bits where 48 are expected, and the reverse. */
	t = start_command(&s, 9, 0xaaaa0000);
	run_until(&s, t + CLK(48 + 2 + 48) - 1);
	CHECK(rd(&s, SDCMD) & NEW);
	run_until(&s, t + CLK(48 + 2 + 48));
	CHECK_EQ(rd(&s, SDCMD), FAIL | 9);
	CHECK_EQ(rd(&s, SDHSTS), CRC7_ERROR);
	wr(&s, SDHSTS, CRC7_ERROR);
	t = start_command(&s, 13 | LONG, 0xaaaa0000);
	run_until(&s, t + CLK(48 + 2 + 136) - 1);
	CHECK(rd(&s, SDCMD) & NEW);
	run_until(&s, t + CLK(48 + 2 + 136));
	CHECK_EQ(rd(&s, SDCMD), FAIL | LONG | 13);
	teardown(&s);
}

/*
 * A block read word by word, each read as soon as it comes: the times,
 * DATA_FLAG for the rest of the block, the block's end 17 clocks after
 * its last word, and the interrupt line at each of those moments.
 */
static void test_read_exact(void)
{
	Setup s;
	uint64_t t, done;
	uint32_t word;

	setup(&s);
	identify(&s);
	wr(&s, SDHBCT, 512);
	wr(&s, SDHBLC, 1);
	wr(&s, SDEDM, 4 << 14 | 4 << 9);
	wr(&s, SDHCFG, SLOW_CARD | DATA_IRPT_EN);
	t = start_command(&s, READ | 17, 0);
	done = t + CLK(98);
	for (unsigned i = 0; i < 128; i++) {
		uint64_t at = done + CLK(3 + 32 * (i + 1));

		run_until(&s, at - 1);
		CHECK_EQ(fifo(&s), 0);
		CHECK(!irq_level(&s.irq));
		run_until(&s, at);
		CHECK_EQ(fifo(&s), 1);
		/* One word is below the threshold, but the last one is due. */
		CHECK_EQ(rd(&s, SDHSTS), i == 127 ? DATA_FLAG : 0);
		CHECK_EQ(irq_level(&s.irq), i == 127);
		word = rd(&s, SDDATA);
		CHECK_EQ(word, load_le32(s.disk.data + 4 * i));
		CHECK_EQ(rd(&s, SDHSTS), 0);
		CHECK(!irq_level(&s.irq));
	}
	CHECK_EQ(fsm(&s), READDATA);
	wr(&s, SDHCFG, SLOW_CARD | BLOCK_IRPT_EN);
	t = done + CLK(3 + 32 * 128 + 17);
	run_until(&s, t - 1);
	CHECK_EQ(rd(&s, SDHSTS), 0);
	CHECK(!irq_level(&s.irq));
	run_until(&s, t);
	CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(fsm(&s), DATAMODE);
	teardown(&s);
}

/*
 * Blocks written with the FIFO kept full: each word leaves 32 clocks
 * after the last, the first Nac and a start bit after the response; a
 * block's CRC, its CRC status and the card's busy time take 17 + 8 + 8
 * clocks, in WRITEWAIT1.
 */
static void test_write_exact(void)
{
	Setup s;
	uint64_t t, word_at;
	unsigned next = 0;

	setup(&s);
	identify(&s);
	wr(&s, SDHBCT, 512);
	wr(&s, SDHBLC, 2);
	wr(&s, SDEDM, 4 << 14 | 4 << 9);
	for (; next < 16; next++)
		wr(&s, SDDATA, 0x1000 + next);
	CHECK_EQ(rd(&s, SDHSTS), DATA_FLAG); /* words to read, so far */
	t = start_command(&s, WRITE | 25, 4096);
	word_at = t + CLK(98 + 3 + 32);
	for (unsigned b = 0; b < 2; b++) {
		for (unsigned i = 0; i < 128; i++) {
			run_until(&s, word_at - 1);
			CHECK_EQ(fifo(&s),
			         next - (128 * b + i) < 16 ? next - (128 * b + i) : 16);
			run_until(&s, word_at);
			CHECK_EQ(fifo(&s),
			         (next - (128 * b + i) < 16 ? next - (128 * b + i) : 16) -
			             1);
			/* Refill in the middle of the next word. */
			run_until(&s, word_at + 100);
			if (next < 256) {
				wr(&s, SDDATA, 0x1000 + next);
				next++;
			}
			word_at += CLK(32);
		}
		/* The block's end: CRC, CRC status, busy. */
		word_at -= CLK(32);
		CHECK_EQ(fsm(&s), WRITEWAIT1);
		run_until(&s, word_at + CLK(17 + 8 + 8) - 1);
		CHECK_EQ(fsm(&s), WRITEWAIT1);
		CHECK_EQ(rd(&s, SDHSTS) & BLOCK_IRPT, 0);
		run_until(&s, word_at + CLK(17 + 8 + 8));
		CHECK_EQ(rd(&s, SDHSTS) & BLOCK_IRPT, BLOCK_IRPT);
		wr(&s, SDHSTS, BLOCK_IRPT);
		word_at += CLK(17 + 8 + 8 + 3 + 32);
	}
	CHECK_EQ(fsm(&s), DATAMODE);
	for (unsigned i = 0; i < 256; i++)
		CHECK_EQ(load_le32(s.disk.data + 4096 + 4 * i), 0x1000 + i);
	CHECK_EQ(command(&s, BUSYWAIT | 12, 0), BUSYWAIT | 12);
	wr(&s, SDHSTS, BUSY_IRPT);

	/* The FIFO runs dry: the first word waits Nac, a later one not. */
	wr(&s, SDHBLC, 1);
	t = start_command(&s, WRITE | 24, 8192);
	run_until(&s, t + CLK(98) + 1000);
	CHECK_EQ(fsm(&s), WRITESTART1);
	CHECK_EQ(rd(&s, SDHSTS), DATA_FLAG); /* room */
	wr(&s, SDDATA, 0xabcd0000);
	t = now(&s);
	run_until(&s, t + CLK(3 + 32) - 1);
	CHECK_EQ(fifo(&s), 1);
	run_until(&s, t + CLK(3 + 32));
	CHECK_EQ(fifo(&s), 0);
	CHECK_EQ(fsm(&s), WRITEDATA); /* stalled in the block */
	run_until(&s, t + CLK(3 + 32) + 5000);
	CHECK_EQ(fsm(&s), WRITEDATA);
	wr(&s, SDDATA, 0xabcd0001);
	t = now(&s);
	run_until(&s, t + CLK(32) - 1);
	CHECK_EQ(fifo(&s), 1);
	run_until(&s, t + CLK(32));
	CHECK_EQ(fifo(&s), 0);
	for (unsigned i = 2; i < 128; i++) {
		for (int k = 0; k < 10000 && fifo(&s) == 16; k++)
			run_until(&s, now(&s) + 100);
		wr(&s, SDDATA, 0xabcd0000 + i);
	}
	for (int k = 0; k < 10000 && !(rd(&s, SDHSTS) & BLOCK_IRPT); k++)
		run_until(&s, now(&s) + 100);
	CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT);
	for (unsigned i = 0; i < 128; i++)
		CHECK_EQ(load_le32(s.disk.data + 8192 + 4 * i), 0xabcd0000 + i);
	teardown(&s);
}

/* Writes that the card does not take: CRC16_ERROR at the block's end. */
static void test_write_errors(void)
{
	for (int c = 0; c < 2; c++) {
		Setup s;

		setup(&s);
		identify(&s);
		wr(&s, SDHBCT, 512);
		wr(&s, SDHBLC, 1);
		if (c == 0)
			wr(&s, SDHCFG, SLOW_CARD | WIDE_EXT_BUS); /* the card: 1 line */
		command(&s, WRITE | 24, 0);
		if (c == 1)
			s.connected = false; /* the card leaves */
		for (unsigned i = 0; i < 128; i++) {
			for (int k = 0; k < 10000 && fifo(&s) == 16; k++)
				run_until(&s, now(&s) + 100);
			wr(&s, SDDATA, i);
		}
		for (int k = 0; k < 10000 && !(rd(&s, SDHSTS) & BLOCK_IRPT); k++)
			run_until(&s, now(&s) + 100);
		CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT | CRC16_ERROR);
		teardown(&s);
	}
}

/* Data mode without SLOW_CARD: (SDCDIV & 7) + 2, to the nanosecond. */
static void test_data_clock_exact(void)
{
	Setup s;
	uint64_t t;

	setup(&s);
	identify(&s);
	wr(&s, SDCDIV, 0x7f9);                       /* 250 / 3 MHz */
	wr(&s, SDHCFG, REL_CMD_LINE | WIDE_INT_BUS); /* neither slows it */
	t = start_command(&s, 13, 0xaaaa0000);
	run_until(&s, t + clocks(98, 3) - 1);
	CHECK(rd(&s, SDCMD) & NEW);
	run_until(&s, t + clocks(98, 3));
	CHECK_EQ(rd(&s, SDCMD), 13);
	/* Neither are they 4 data lines: a word takes 32 clocks. */
	wr(&s, SDCDIV, 3);
	wr(&s, SDHCFG, SLOW_CARD | REL_CMD_LINE | WIDE_INT_BUS);
	wr(&s, SDHBLC, 1);
	t = start_command(&s, READ | 17, 0);
	run_until(&s, t + CLK(98 + 3 + 32) - 1);
	CHECK_EQ(fifo(&s), 0);
	run_until(&s, t + CLK(98 + 3 + 32));
	CHECK_EQ(fifo(&s), 1);
	teardown(&s);
}

/* Each source of the interrupt with each enable bit, and no other. */
static void test_irq_matrix(void)
{
	static const uint32_t bits[] = {REL_CMD_LINE,  WIDE_INT_BUS, WIDE_EXT_BUS,
	                                SLOW_CARD,     DATA_IRPT_EN, SDIO_IRPT_EN,
	                                BLOCK_IRPT_EN, BUSY_IRPT_EN};
	Setup s;

	setup(&s);
	identify(&s);
	for (int source = 0; source < 3; source++) {
		uint32_t enable = source == 0   ? DATA_IRPT_EN
		                  : source == 1 ? BUSY_IRPT_EN
		                                : BLOCK_IRPT_EN;

		wr(&s, SDHCFG, SLOW_CARD);
		if (source == 0) {
			wr(&s, SDDATA, 1); /* a word in the FIFO, no transfer */
			CHECK_EQ(rd(&s, SDHSTS), DATA_FLAG);
		} else if (source == 1) {
			command(&s, BUSYWAIT | 13, 0xaaaa0000);
			run_until(&s, now(&s) + 10000);
			CHECK_EQ(rd(&s, SDHSTS), BUSY_IRPT);
		} else {
			uint32_t words[128];

			wr(&s, SDHBLC, 1);
			command(&s, READ | 17, 0);
			read_block(&s, words, 128);
			run_until(&s, now(&s) + 10000);
			CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT);
		}
		for (size_t i = 0; i < ARRAY_SIZE(bits); i++) {
			wr(&s, SDHCFG, bits[i] | (bits[i] == SLOW_CARD ? 0 : SLOW_CARD));
			CHECK_EQ(irq_level(&s.irq), bits[i] == enable);
		}
		wr(&s, SDHCFG, SLOW_CARD);
		wr(&s, SDHSTS, 0x7f8);
		wr(&s, SDEDM, 1u << 19); /* empties the FIFO */
		CHECK_EQ(rd(&s, SDHSTS), 0);
	}
	teardown(&s);
}

/* SDEDM's thresholds: where they are, and how DATA_FLAG uses them. */
static void test_thresholds(void)
{
	Setup s;
	uint64_t t;

	setup(&s);
	identify(&s);
	/* Read back as written; the FSM and FIFO fields are ignored. */
	wr(&s, SDEDM, 0x1fu << 14 | 1u << 9 | 0x1ff);
	CHECK_EQ(rd(&s, SDEDM), 0x1fu << 14 | 1u << 9 | DATAMODE);
	wr(&s, SDEDM, 1u << 14 | 0x1fu << 9);
	CHECK_EQ(rd(&s, SDEDM), 1u << 14 | 0x1fu << 9 | DATAMODE);
	CHECK_EQ(rd(&s, SDHCFG), SLOW_CARD); /* SDEDM is not SDHCFG */

	/* A read threshold of 3, then 0 (taken as 1). */
	for (unsigned th = 3;; th = 0) {
		unsigned need = th ? th : 1;

		wr(&s, SDEDM, th << 14);
		wr(&s, SDHBLC, 1);
		t = start_command(&s, READ | 17, 0) + CLK(98 + 3);
		for (unsigned w = 1; w <= 4; w++) {
			run_until(&s, t + CLK(32 * w));
			CHECK_EQ(fifo(&s), w);
			CHECK_EQ(rd(&s, SDHSTS) & DATA_FLAG, w >= need ? DATA_FLAG : 0);
		}
		wr(&s, SDEDM, 1u << 19);
		command(&s, BUSYWAIT | 12, 0);
		wr(&s, SDHSTS, 0x7f8);
		if (th == 0)
			break;
	}

	/* A write threshold of 4: room for 4 words, or not. */
	wr(&s, SDEDM, 4u << 9);
	wr(&s, SDHBLC, 1);
	command(&s, WRITE | 24, 0);
	for (unsigned i = 0; i < 13; i++) {
		CHECK_EQ(rd(&s, SDHSTS) & DATA_FLAG, DATA_FLAG);
		wr(&s, SDDATA, i);
	}
	CHECK_EQ(rd(&s, SDHSTS) & DATA_FLAG, 0); /* room for 3 */
	wr(&s, SDEDM, 1u << 19);
	teardown(&s);

	/* Words left in the FIFO after the transfer: DATA_FLAG, any threshold. */
	setup(&s);
	identify(&s);
	wr(&s, SDEDM, 0x1fu << 14);
	wr(&s, SDHBLC, 1);
	wr(&s, SDHBCT, 16);
	command(&s, 55, 0xaaaa0000);
	command(&s, READ | 51, 0); /* the SCR: 8 bytes in a block of 16 */
	run_until(&s, now(&s) + 100000);
	CHECK_EQ(fsm(&s), DATAMODE);
	CHECK_EQ(fifo(&s), 4);
	CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT | CRC16_ERROR | DATA_FLAG);
	CHECK_EQ(rd(&s, SDDATA), 0x02802502u); /* 02 25 80 02 */
	CHECK_EQ(rd(&s, SDDATA), 0);
	CHECK_EQ(rd(&s, SDDATA), 0); /* past the SCR: zeros */
	CHECK_EQ(rd(&s, SDDATA), 0);
	teardown(&s);
}

/* Register writes that must change only themselves. */
static void test_register_writes(void)
{
	Setup s;
	uint64_t value;

	setup(&s);
	/* SDCMD without NEW: a value, not a command; also without power. */
	wr(&s, SDCMD, 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	run_until(&s, now(&s) + 1000000);
	CHECK_EQ(rd(&s, SDCMD), 0);
	wr(&s, SDVDD, 1);
	CHECK_EQ(command(&s, 8, 0x2aa), FAIL | 8);
	wr(&s, SDCMD, 8);
	CHECK_EQ(rd(&s, SDCMD), 8); /* FAIL cleared */
	run_until(&s, now(&s) + 1000000);
	CHECK_EQ(rd(&s, SDHSTS), CMD_TIME_OUT);
	/* Writing 0 clears nothing; a bit clears only itself. */
	wr(&s, SDHSTS, 0);
	CHECK_EQ(rd(&s, SDHSTS), CMD_TIME_OUT);
	wr(&s, SDHSTS, CRC7_ERROR);
	CHECK_EQ(rd(&s, SDHSTS), CMD_TIME_OUT);
	CHECK_EQ(rd(&s, SDVDD), 1); /* SDHSTS is not SDVDD */
	wr(&s, SDTOUT, 0x1234);
	CHECK_EQ(rd(&s, SDCDIV), 507); /* SDTOUT is not SDCDIV */
	wr(&s, SDHBLC, 7);
	CHECK_EQ(fifo(&s), 0); /* SDHBLC is not SDDATA */
	CHECK_EQ(rd(&s, SDHBLC), 7);
	/* SDEDM without FORCE_DATA_MODE keeps identification mode. */
	wr(&s, SDEDM, 4u << 14);
	CHECK_EQ(fsm(&s), IDENTMODE);
	wr(&s, SDEDM, 1u << 19);
	CHECK_EQ(fsm(&s), DATAMODE);
	/* The register window: 0x100 bytes. */
	CHECK_EQ(bus_read(s.m->bus, BASE + 0x100, 4, &value), BUS_ERR_UNMAPPED);
	CHECK_EQ(bus_read(s.m->bus, BASE + 0xfc, 4, &value), BUS_OK);
	CHECK(strstr(s.m->stop_message, "offset 0xfc is not a register"));
	teardown(&s);

	/* During a read: SDCMD without NEW, and SDVDD 1 again, stop nothing. */
	setup(&s);
	identify(&s);
	wr(&s, SDHBLC, 1);
	command(&s, READ | 17, 0);
	wr(&s, SDCMD, READ | 17);
	wr(&s, SDVDD, 1);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	{
		uint32_t words[128];

		read_block(&s, words, 128);
		CHECK(memcmp(words, s.disk.data, 512) == 0);
	}
	teardown(&s);

	CHECK(bcm2835_sdhost_create(NULL, 0, &(Bcm2835SdhostSlot){0}, NULL) ==
	      NULL);
}

/* Commands and transfers at the edges. */
static void test_command_edges(void)
{
	Setup s;
	uint32_t words[128];

	/* CMD15 sends the card away: identification mode. */
	setup(&s);
	identify(&s);
	command(&s, 15 | NO_RESPONSE, 0xaaaa0000);
	CHECK_EQ(fsm(&s), IDENTMODE);
	teardown(&s);

	/* SDHBLC 0: one block. */
	setup(&s);
	identify(&s);
	wr(&s, SDHBLC, 0);
	command(&s, READ | 18, 0);
	read_block(&s, words, 128);
	run_until(&s, now(&s) + 100000);
	CHECK_EQ(fsm(&s), DATAMODE);
	CHECK_EQ(rd(&s, SDHSTS), BLOCK_IRPT);
	CHECK_EQ(command(&s, BUSYWAIT | 12, 0), BUSYWAIT | 12);
	/* A read that fails has no data. */
	s.connected = false;
	CHECK_EQ(command(&s, READ | 17, 0), FAIL | READ | 17);
	CHECK_EQ(fsm(&s), DATAMODE);
	run_until(&s, now(&s) + 1000000);
	CHECK_EQ(fsm(&s), DATAMODE);
	CHECK_EQ(fifo(&s), 0);
	/* The card leaves between the command and its data: a timeout. */
	s.connected = true;
	wr(&s, SDHSTS, 0x7f8);
	wr(&s, SDTOUT, 100);
	start_command(&s, READ | 17, 0);
	s.connected = false;
	run_until(&s, now(&s) + CLK(98 + 100));
	CHECK_EQ(rd(&s, SDHSTS), REW_TIME_OUT);
	CHECK_EQ(fifo(&s), 0);
	teardown(&s);

	/* A reset in the middle of a transfer, with the interrupt up. */
	setup(&s);
	identify(&s);
	wr(&s, SDHBLC, 5);
	wr(&s, SDHCFG, SLOW_CARD | DATA_IRPT_EN);
	command(&s, READ | 18, 0);
	run_until(&s, now(&s) + 20000);
	CHECK(irq_level(&s.irq));
	bcm2835_sdhost_reset(s.host);
	CHECK(!irq_level(&s.irq));
	CHECK_EQ(rd(&s, SDHBLC), 0);
	CHECK_EQ(fifo(&s), 0);
	wr(&s, SDVDD, 1);
	CHECK_EQ(fsm(&s), IDENTMODE);
	run_until(&s, now(&s) + 10000000);
	CHECK_EQ(fifo(&s), 0);
	CHECK_EQ(rd(&s, SDHSTS), 0);
	teardown(&s);

	/* Destroyed in the middle of each kind of wait. */
	for (int when = 0; when < 4; when++) {
		setup(&s);
		identify(&s);
		wr(&s, SDHBLC, 2);
		if (when == 0) {
			start_command(&s, READ | 18, 0); /* the response */
		} else if (when == 1) {
			command(&s, READ | 18, 0); /* a word */
		} else if (when == 2) {
			command(&s, BUSYWAIT | 13, 0xaaaa0000);
			start_command(&s, BUSYWAIT | 13, 0xaaaa0000);
			run_until(&s, now(&s) + CLK(98) + 1); /* the busy time */
		} else {
			wr(&s, SDTOUT, 1000000);
			wr(&s, SDARG, 1u << 20); /* out of range: no data */
			command(&s, READ | 17, 1u << 20);
		}
		bcm2835_sdhost_destroy(s.host);
		run_until(&s, now(&s) + 100000000);
		if (s.card != NULL)
			sd_card_destroy(s.card);
		machine_destroy(s.m);
		block_mem_free(&s.disk);
	}
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_command_timing);
	RUN_TEST(test_no_card);
	RUN_TEST(test_identification);
	RUN_TEST(test_read);
	RUN_TEST(test_multiple_read);
	RUN_TEST(test_write);
	RUN_TEST(test_bus_width);
	RUN_TEST(test_clock);
	RUN_TEST(test_abort);
	RUN_TEST(test_stops);
	RUN_TEST(test_response_lengths);
	RUN_TEST(test_read_exact);
	RUN_TEST(test_write_exact);
	RUN_TEST(test_write_errors);
	RUN_TEST(test_data_clock_exact);
	RUN_TEST(test_irq_matrix);
	RUN_TEST(test_thresholds);
	RUN_TEST(test_register_writes);
	RUN_TEST(test_command_edges);
	return test_summary();
}
