/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */
/*
 * The BSC, the BCM2835's I2C master, with nothing on its bus: the
 * registers of BCM2835 ARM Peripherals chapter 3, and the transfers of
 * Linux's i2c-bcm2835.c, each ending with ERR and DONE.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/bus.h"
#include "core/event.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/i2c/bcm2835_bsc.h"
#include "test.h"

#define BASE 0x3f805000
#define CORE_HZ 250000000u
#define LINUX_DIV 0x9c4u /* 100 kHz from 250 MHz, as Linux sets it */
#define NAK_NS 100000u   /* 10 SCL clocks of 10 us */

enum { C = 0x0, S = 0x4, DLEN = 0x8, A = 0xc, FIFO = 0x10 };
enum { DIV = 0x14, DEL = 0x18, CLKT = 0x1c };

/* C */
#define READ 0x0001u
#define CLEAR 0x0010u
#define CLEAR2 0x0020u
#define ST 0x0080u
#define INTD 0x0100u
#define INTT 0x0200u
#define INTR 0x0400u
#define I2CEN 0x8000u

/* S */
#define TA 0x001u
#define DONE 0x002u
#define TXW 0x004u
#define RXR 0x008u
#define TXD 0x010u
#define RXD 0x020u
#define TXE 0x040u
#define RXF 0x080u
#define ERR 0x100u
#define CLKT_S 0x200u

typedef struct Setup {
	Machine *m;
	Bcm2835Bsc *bsc;
	IrqLine irq;
} Setup;

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	irq_init(&s->irq, NULL, NULL, 0);
	s->bsc = bcm2835_bsc_create(s->m, "bsc2", CORE_HZ, &s->irq);
	CHECK(s->bsc != NULL);
	CHECK_EQ(bcm2835_bsc_map(s->bsc, s->m->bus, "bsc2", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_bsc_destroy(s->bsc);
	machine_destroy(s->m);
}

static uint64_t rd(Setup *s, uint64_t offset)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, BASE + offset, 4, &value), BUS_OK);
	return value;
}

static void wr(Setup *s, uint64_t offset, uint64_t value)
{
	CHECK_EQ(bus_write(s->m->bus, BASE + offset, 4, value), BUS_OK);
}

static void advance(Setup *s, uint64_t t)
{
	event_queue_run_until(&s->m->events, t);
}

static void check_stop(Setup *s, const char *message)
{
	CHECK_EQ(s->m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s->m->stop_message, "bsc2: ") != NULL);
	if (strstr(s->m->stop_message, message) == NULL)
		fprintf(stderr, "got \"%s\"\n", s->m->stop_message);
	CHECK(strstr(s->m->stop_message, message) != NULL);
	s->m->stop = MACHINE_STOP_NONE;
}

static void check_running(Setup *s)
{
	CHECK_EQ(s->m->stop, MACHINE_STOP_NONE);
}

/* The registers' reset values, and Linux's probe. */
static void test_reset_and_probe(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, C), 0);
	CHECK_EQ(rd(&s, S), TXD | TXE);
	CHECK_EQ(rd(&s, DLEN), 0);
	CHECK_EQ(rd(&s, A), 0);
	CHECK_EQ(rd(&s, DIV), 0x5dc);
	CHECK_EQ(rd(&s, DEL), 0x00300030);
	CHECK_EQ(rd(&s, CLKT), 0x40);
	CHECK(!irq_level(&s.irq));

	/* What the Pi OS kernel wrote at boot. */
	wr(&s, DIV, LINUX_DIV);
	wr(&s, DEL, 0x009c0271);
	wr(&s, CLKT, 0xdac);
	wr(&s, C, 0);
	CHECK_EQ(rd(&s, DIV), LINUX_DIV);
	CHECK_EQ(rd(&s, DEL), 0x009c0271);
	CHECK_EQ(rd(&s, CLKT), 0xdac);
	wr(&s, DEL, 0xffffffff); /* FEDL and REDL fill the register */
	CHECK_EQ(rd(&s, DEL), 0xffffffff);
	wr(&s, DIV, 0xffff);
	wr(&s, CLKT, 0xffff);
	wr(&s, A, 0x7f);
	wr(&s, DLEN, 0xffff);
	CHECK_EQ(rd(&s, DIV), 0xffff);
	CHECK_EQ(rd(&s, CLKT), 0xffff);
	CHECK_EQ(rd(&s, A), 0x7f);
	CHECK_EQ(rd(&s, DLEN), 0xffff);
	/* C keeps its settings; ST and CLEAR read 0. */
	wr(&s, C, INTR | INTT | INTD | READ | CLEAR | CLEAR2);
	CHECK_EQ(rd(&s, C), INTR | INTT | INTD | READ);
	check_running(&s);

	/* A reset in the middle of a transfer: it never ends. */
	wr(&s, FIFO, 1);
	wr(&s, C, I2CEN | ST | INTD);
	CHECK(rd(&s, S) & TA);
	bcm2835_bsc_reset(s.bsc);
	CHECK_EQ(rd(&s, S), TXD | TXE);
	CHECK_EQ(rd(&s, C), 0);
	CHECK_EQ(rd(&s, DIV), 0x5dc);
	CHECK_EQ(rd(&s, DEL), 0x00300030);
	CHECK_EQ(rd(&s, CLKT), 0x40);
	CHECK_EQ(rd(&s, A), 0);
	CHECK_EQ(rd(&s, DLEN), 0);
	advance(&s, 10 * NAK_NS * 33);
	CHECK_EQ(rd(&s, S), TXD | TXE);
	CHECK(!irq_level(&s.irq));
	check_running(&s);
	teardown(&s);

	CHECK(bcm2835_bsc_create(NULL, "bsc0", 0, NULL) == NULL);
	bcm2835_bsc_destroy(NULL);
}

/*
 * Linux's write of two bytes, with INTT and INTD: TXW asks for the
 * bytes, no slave acknowledges the address, ERR and DONE end it.
 */
static void test_write_nak(void)
{
	Setup s;

	setup(&s);
	wr(&s, DIV, LINUX_DIV);
	wr(&s, C, CLEAR);
	wr(&s, S, CLKT_S | ERR | DONE);
	wr(&s, A, 0x50);
	wr(&s, DLEN, 2);
	wr(&s, C, I2CEN | ST | INTT | INTD);
	CHECK_EQ(rd(&s, C), I2CEN | INTT | INTD);
	CHECK_EQ(rd(&s, S), TA | TXW | TXD | TXE);
	CHECK(irq_level(&s.irq));
	wr(&s, FIFO, 0xa0);
	CHECK_EQ(rd(&s, S), TA | TXW | TXD | RXD);
	wr(&s, FIFO, 0x5b);
	CHECK_EQ(rd(&s, S), TA | TXD | RXD); /* enough for DLEN */
	CHECK(!irq_level(&s.irq));
	CHECK_EQ(rd(&s, DLEN), 2);
	advance(&s, NAK_NS - 1);
	CHECK_EQ(rd(&s, S), TA | TXD | RXD);
	advance(&s, NAK_NS);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | RXD);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(rd(&s, DLEN), 2); /* no byte moved */
	/* DONE alone clears; RO bits and 0s do nothing. */
	wr(&s, S, DONE | TA | TXW | RXR | TXD | RXD | TXE | RXF);
	CHECK_EQ(rd(&s, S), ERR | TXD | RXD);
	CHECK(!irq_level(&s.irq));
	wr(&s, S, 0);
	CHECK_EQ(rd(&s, S), ERR | TXD | RXD);
	wr(&s, S, ERR);
	CHECK_EQ(rd(&s, S), TXD | RXD);
	/* The bytes are still in the FIFO, in order. */
	CHECK_EQ(rd(&s, FIFO), 0xa0);
	CHECK_EQ(rd(&s, FIFO), 0x5b);
	CHECK_EQ(rd(&s, S), TXD | TXE);
	check_running(&s);
	teardown(&s);
}

/* TXW: a write, the FIFO under a quarter and under DLEN. */
static void test_txw(void)
{
	Setup s;

	setup(&s);
	wr(&s, DLEN, 10);
	wr(&s, FIFO, 1);
	wr(&s, FIFO, 2);
	wr(&s, FIFO, 3);
	CHECK_EQ(rd(&s, S), TXD | RXD); /* no transfer: no TXW */
	wr(&s, C, I2CEN | ST | INTD);   /* INTT off */
	CHECK_EQ(rd(&s, S), TA | TXW | TXD | RXD);
	CHECK(!irq_level(&s.irq));
	wr(&s, C, I2CEN | INTT); /* INTT on while it runs */
	CHECK(irq_level(&s.irq));
	wr(&s, FIFO, 4);
	CHECK_EQ(rd(&s, S), TA | TXD | RXD);
	CHECK(!irq_level(&s.irq));
	/* A FIFO read takes it under a quarter again. */
	CHECK_EQ(rd(&s, FIFO), 1);
	CHECK_EQ(rd(&s, S), TA | TXW | TXD | RXD);
	CHECK(irq_level(&s.irq));
	advance(&s, NAK_NS);
	/* DONE without INTD: no interrupt; TXW ends with TA. */
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | RXD);
	CHECK(!irq_level(&s.irq));
	wr(&s, C, I2CEN | INTD);
	CHECK(irq_level(&s.irq));
	wr(&s, C, I2CEN | INTR | INTT);
	CHECK(!irq_level(&s.irq));
	check_running(&s);
	teardown(&s);
}

/* RXR: a read, the FIFO three quarters full; a full FIFO. */
static void test_rxr_and_full(void)
{
	Setup s;

	setup(&s);
	for (unsigned i = 0; i < 11; i++)
		wr(&s, FIFO, 0x10 + i);
	wr(&s, DLEN, 1);
	wr(&s, C, I2CEN | ST | READ | INTR);
	CHECK_EQ(rd(&s, S), TA | TXD | RXD);
	CHECK(!irq_level(&s.irq));
	wr(&s, FIFO, 0x1b);
	CHECK_EQ(rd(&s, S), TA | RXR | TXD | RXD);
	CHECK(irq_level(&s.irq));
	for (unsigned i = 12; i < 16; i++)
		wr(&s, FIFO, 0x10 + i);
	CHECK_EQ(rd(&s, S), TA | RXR | RXD | RXF);
	wr(&s, FIFO, 0x99); /* ignored: the FIFO is full */
	advance(&s, NAK_NS);
	CHECK_EQ(rd(&s, S), ERR | DONE | RXD | RXF);
	CHECK(!irq_level(&s.irq)); /* RXR ends with TA; no INTD */
	for (unsigned i = 0; i < 16; i++)
		CHECK_EQ(rd(&s, FIFO), 0x10 + i);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | TXE);
	/* CLEAR empties it; the next byte goes first. */
	wr(&s, FIFO, 0x77);
	wr(&s, FIFO, 0x78);
	CHECK_EQ(rd(&s, FIFO), 0x77);
	wr(&s, C, CLEAR2);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | TXE);
	wr(&s, FIFO, 0x79);
	CHECK_EQ(rd(&s, FIFO), 0x79);
	check_running(&s);
	teardown(&s);
}

/* ST needs I2CEN; how long the address takes; CLEAR with ST. */
static void test_start_and_timing(void)
{
	Setup s;

	setup(&s);
	wr(&s, C, ST | INTD);
	CHECK_EQ(rd(&s, S), TXD | TXE);
	advance(&s, NAK_NS * 400);
	CHECK_EQ(rd(&s, S), TXD | TXE);

	/* CDIV 0 divides by 32768: 10 clocks of 131.072 us. */
	wr(&s, DIV, 0);
	wr(&s, C, I2CEN | ST);
	advance(&s, s.m->events.now + 1310719);
	CHECK_EQ(rd(&s, S), TA | TXD | TXE);
	advance(&s, s.m->events.now + 1);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | TXE);
	wr(&s, S, ERR | DONE);

	/* An odd CDIV counts as the even one below. */
	wr(&s, DIV, LINUX_DIV + 1);
	CHECK_EQ(rd(&s, DIV), LINUX_DIV + 1);
	wr(&s, FIFO, 5);
	wr(&s, C, I2CEN | ST | CLEAR); /* the FIFO cleared first */
	CHECK_EQ(rd(&s, S), TA | TXD | TXE);
	advance(&s, s.m->events.now + NAK_NS - 1);
	CHECK_EQ(rd(&s, S), TA | TXD | TXE);
	advance(&s, s.m->events.now + 1);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | TXE);

	/* A write of zero bytes: still the address, still no ACK. */
	wr(&s, S, ERR | DONE);
	wr(&s, DLEN, 0);
	wr(&s, C, I2CEN | ST | INTT | INTD);
	CHECK_EQ(rd(&s, S), TA | TXD | TXE);
	CHECK(!irq_level(&s.irq));
	advance(&s, s.m->events.now + NAK_NS);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | TXE);
	CHECK(irq_level(&s.irq));
	/* Another transfer after DONE, before it is cleared. */
	wr(&s, C, I2CEN | ST | INTD);
	CHECK_EQ(rd(&s, S), TA | ERR | DONE | TXD | TXE);
	advance(&s, s.m->events.now + NAK_NS);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | TXE);
	check_running(&s);
	teardown(&s);

	/* Without an interrupt line. */
	s.m = machine_create();
	s.bsc = bcm2835_bsc_create(s.m, "bsc2", CORE_HZ, NULL);
	CHECK(s.bsc != NULL);
	CHECK_EQ(bcm2835_bsc_map(s.bsc, s.m->bus, "bsc2", BASE), BUS_OK);
	wr(&s, C, I2CEN | ST | INTD);
	advance(&s, NAK_NS * 33);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | TXE);
	teardown(&s);
}

/*
 * Each bit of C, S and the FIFO's levels alone: a transfer needs ST and
 * I2CEN, RXR a read, its interrupt INTR; TA is read-only.
 */
static void test_bits(void)
{
	Setup s;

	setup(&s);
	wr(&s, C, I2CEN | READ | INTD); /* no ST */
	CHECK_EQ(rd(&s, S), TXD | TXE);
	wr(&s, C, ST | READ | INTD); /* no I2CEN */
	CHECK_EQ(rd(&s, S), TXD | TXE);
	advance(&s, s.m->events.now + NAK_NS * 10);
	CHECK_EQ(rd(&s, S), TXD | TXE);

	/* A write with 12 bytes in the FIFO: no RXR. */
	for (unsigned i = 0; i < 12; i++)
		wr(&s, FIFO, i);
	wr(&s, DLEN, 12);
	wr(&s, C, I2CEN | ST | INTR);
	CHECK_EQ(rd(&s, S), TA | TXD | RXD);
	CHECK(!irq_level(&s.irq));
	/* Writing S's read-only bits leaves them; TA goes on. */
	wr(&s, S, TA | TXW | RXR | TXD | RXD | TXE | RXF);
	CHECK_EQ(rd(&s, S), TA | TXD | RXD);
	advance(&s, s.m->events.now + NAK_NS);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | RXD);
	wr(&s, S, ERR | DONE);

	/* A read with 12 bytes: RXR, but no interrupt without INTR. */
	wr(&s, C, I2CEN | ST | READ | INTD | INTT);
	CHECK_EQ(rd(&s, S), TA | RXR | TXD | RXD);
	CHECK(!irq_level(&s.irq));
	wr(&s, C, I2CEN | READ | INTR);
	CHECK(irq_level(&s.irq));
	advance(&s, s.m->events.now + NAK_NS);
	CHECK(!irq_level(&s.irq));

	/* A reset lowers the interrupt. */
	wr(&s, C, I2CEN | INTD);
	CHECK(irq_level(&s.irq)); /* DONE */
	bcm2835_bsc_reset(s.bsc);
	CHECK(!irq_level(&s.irq));

	/* DIV, DEL and CLKT are each their own. */
	wr(&s, DIV, 0x100);
	CHECK_EQ(rd(&s, DEL), 0x00300030);
	CHECK_EQ(rd(&s, CLKT), 0x40);
	wr(&s, DEL, 0x10);
	CHECK_EQ(rd(&s, DIV), 0x100);
	CHECK_EQ(rd(&s, CLKT), 0x40);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);

	/* A core clock of 1 Hz: CDIV 2 makes SCL 0.5 Hz, 10 clocks 20 s. */
	s.m = machine_create();
	irq_init(&s.irq, NULL, NULL, 0);
	s.bsc = bcm2835_bsc_create(s.m, "bsc2", 1, &s.irq);
	CHECK_EQ(bcm2835_bsc_map(s.bsc, s.m->bus, "bsc2", BASE), BUS_OK);
	wr(&s, DIV, 2);
	wr(&s, C, I2CEN | ST);
	advance(&s, 20000000000ull - 1);
	CHECK_EQ(rd(&s, S), TA | TXD | TXE);
	advance(&s, 20000000000ull);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | TXE);
	teardown(&s);
}

/* What is not modelled stops the machine. */
static void test_stops(void)
{
	static const struct {
		uint64_t offset;
		uint32_t value;
	} reserved[] = {
		{C, 0x0002},   {C, 0x0040},     {C, 0x0800},     {C, 0x10000},
		{S, 0x400},    {S, 0x80000000}, {DLEN, 0x10000}, {A, 0x80},
		{FIFO, 0x100}, {DIV, 0x10000},  {CLKT, 0x10000},
	};
	Setup s;
	uint64_t v;

	setup(&s);
	CHECK_EQ(bus_read(s.m->bus, BASE + S, 2, &v), BUS_OK);
	check_stop(&s, "2-byte read of offset 0x04 is not a 32-bit");
	CHECK_EQ(bus_read(s.m->bus, BASE + 1, 4, &v), BUS_OK);
	check_stop(&s, "read of offset 0x01");
	CHECK_EQ(bus_write(s.m->bus, BASE + C, 1, 0), BUS_OK);
	check_stop(&s, "1-byte write to offset 0x00 is not a 32-bit");
	CHECK_EQ(bus_write(s.m->bus, BASE + 6, 4, 0), BUS_OK);
	check_stop(&s, "write to offset 0x06");
	for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
		wr(&s, reserved[i].offset, reserved[i].value);
		check_stop(&s, "a 1 in a reserved bit");
	}
	wr(&s, C, 0x2);
	check_stop(&s, "4-byte write to offset 0x00 is not emulated: a 1 in a "
	               "reserved bit, which is written as 0");
	/* With bits that count: none is written, nothing starts. */
	wr(&s, C, I2CEN | ST | INTD | 0x2);
	check_stop(&s, "a 1 in a reserved bit");
	CHECK_EQ(rd(&s, S), TXD | TXE);
	advance(&s, s.m->events.now + NAK_NS * 10);
	CHECK_EQ(rd(&s, S), TXD | TXE);
	/* Nothing was written. */
	CHECK_EQ(rd(&s, C), 0);
	CHECK_EQ(rd(&s, S), TXD | TXE);
	CHECK_EQ(rd(&s, DLEN), 0);
	CHECK_EQ(rd(&s, A), 0);
	CHECK_EQ(rd(&s, DIV), 0x5dc);
	CHECK_EQ(rd(&s, CLKT), 0x40);
	CHECK_EQ(rd(&s, FIFO), 0);
	check_stop(&s, "the FIFO is empty");

	wr(&s, A, 0x50);
	wr(&s, DLEN, 3);
	wr(&s, FIFO, 9);
	wr(&s, C, I2CEN | ST);
	wr(&s, C, I2CEN | ST);
	check_stop(&s, "4-byte write to offset 0x00 is not emulated: ST or CLEAR "
	               "while a transfer is active");
	wr(&s, C, I2CEN | CLEAR);
	check_stop(&s, "ST or CLEAR while a transfer is active");
	wr(&s, C, I2CEN | CLEAR2);
	check_stop(&s, "ST or CLEAR while a transfer is active");
	wr(&s, DLEN, 1);
	check_stop(&s, "DLEN or A while a transfer is active");
	wr(&s, A, 0x51);
	check_stop(&s, "DLEN or A while a transfer is active");
	CHECK_EQ(rd(&s, A), 0x50);
	CHECK_EQ(rd(&s, DLEN), 3);
	CHECK_EQ(rd(&s, FIFO), 9); /* the stopped CLEARs cleared nothing */
	/* Other registers may change while it runs. */
	wr(&s, C, I2CEN | INTD);
	wr(&s, DIV, 0x100);
	wr(&s, DEL, 1);
	wr(&s, CLKT, 2);
	wr(&s, FIFO, 3);
	check_running(&s);
	advance(&s, s.m->events.now + NAK_NS);
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | RXD);
	wr(&s, S, DONE | 0x400); /* a reserved bit: DONE stays */
	check_stop(&s, "a 1 in a reserved bit");
	CHECK_EQ(rd(&s, S), ERR | DONE | TXD | RXD);
	wr(&s, DLEN, 1);
	wr(&s, A, 0x51);
	CHECK_EQ(rd(&s, A), 0x51);
	CHECK_EQ(rd(&s, DLEN), 1);
	check_running(&s);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_reset_and_probe);
	RUN_TEST(test_write_nak);
	RUN_TEST(test_txw);
	RUN_TEST(test_rxr_and_full);
	RUN_TEST(test_start_and_timing);
	RUN_TEST(test_bits);
	RUN_TEST(test_stops);
	return test_summary();
}
