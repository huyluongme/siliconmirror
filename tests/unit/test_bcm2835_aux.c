/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835 auxiliary peripherals and their mini UART
 * (src/hw/char/bcm2835_aux.c). Expected values come from chapter 2 of
 * "BCM2835 ARM Peripherals" and of "BCM2711 ARM Peripherals".
 *
 * The mini UART runs from the 250 MHz system clock. Most tests set it to
 * the 115313 baud of the Raspberry Pi firmware, AUX_MU_BAUD_REG 270: a
 * frame of a start bit, 8 data bits and a stop bit takes
 * 10 * 8 * 271 / 250 MHz = 86720 ns, and one of 7 data bits 78048 ns.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/char_stream.h"
#include "core/machine.h"
#include "hw/char/bcm2835_aux.h"
#include "test.h"
#include "util/macros.h"

#define BASE 0x3f215000
#define CLOCK_HZ 250000000
#define FRAME_8 86720
#define FRAME_7 78048

enum {
	AUX_IRQ = 0x00,
	AUX_ENABLES = 0x04,
	IO = 0x40,
	IER = 0x44,
	IIR = 0x48,
	LCR = 0x4c,
	MCR = 0x50,
	LSR = 0x54,
	MSR = 0x58,
	SCRATCH = 0x5c,
	CNTL = 0x60,
	STAT = 0x64,
	BAUD = 0x68,
};

enum {
	LSR_DATA_READY = 0x01,
	LSR_OVERRUN = 0x02,
	LSR_TX_EMPTY = 0x20,
	LSR_TX_IDLE = 0x40,
	STAT_SYMBOL = 1 << 0,
	STAT_SPACE = 1 << 1,
	STAT_RX_IDLE = 1 << 2,
	STAT_TX_IDLE = 1 << 3,
	STAT_OVERRUN = 1 << 4,
	STAT_TX_FULL = 1 << 5,
	STAT_RTS = 1 << 6,
	STAT_TX_EMPTY = 1 << 8,
	STAT_TX_DONE = 1 << 9,
};

#define STAT_RESET                                                             \
	(STAT_TX_DONE | STAT_TX_EMPTY | STAT_TX_IDLE | STAT_RX_IDLE | STAT_SPACE)

/*
 * A character stream that collects what is written to it, gives the
 * bytes of input as the host would have sent them, and can hold the line
 * low.
 */
typedef struct Capture {
	CharStream stream;
	char data[128];
	size_t len;
	const char *input;
	size_t input_len, input_pos;
	bool low;
} Capture;

static void capture_write(CharStream *s, const uint8_t *buf, size_t len)
{
	Capture *c = container_of(s, Capture, stream);

	for (size_t i = 0; i < len && c->len < sizeof(c->data) - 1; i++)
		c->data[c->len++] = (char)buf[i];
	c->data[c->len] = '\0';
}

static bool capture_read(CharStream *s, uint8_t *byte)
{
	Capture *c = container_of(s, Capture, stream);

	if (c->low || c->input_pos == c->input_len)
		return false;
	*byte = (uint8_t)c->input[c->input_pos++];
	return true;
}

static bool capture_wait(CharStream *s)
{
	Capture *c = container_of(s, Capture, stream);

	return !c->low && c->input_pos < c->input_len;
}

static bool capture_line_low(CharStream *s)
{
	return container_of(s, Capture, stream)->low;
}

static void capture_destroy(CharStream *s)
{
	(void)s;
}

static const CharStreamOps capture_ops = {
	.write = capture_write,
	.read = capture_read,
	.wait = capture_wait,
	.line_low = capture_line_low,
	.destroy = capture_destroy,
};

typedef struct Setup {
	Machine *m;
	Bcm2835Aux *aux;
	Capture serial;
	IrqLine irq;
} Setup;

/* A machine with the block at BASE, its mini UART enabled. */
static void setup(Setup *s)
{
	memset(s, 0, sizeof(*s));
	s->serial.stream.ops = &capture_ops;
	s->m = machine_create();
	CHECK(s->m != NULL);
	irq_init(&s->irq, NULL, NULL, 0);
	s->aux = bcm2835_aux_create(s->m, CLOCK_HZ, &s->serial.stream, &s->irq);
	CHECK(s->aux != NULL);
	CHECK_EQ(bcm2835_aux_map(s->aux, s->m->bus, "aux", BASE), BUS_OK);
	CHECK_EQ(bus_write(s->m->bus, BASE + AUX_ENABLES, 4, 1), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_aux_destroy(s->aux);
	machine_destroy(s->m);
}

static uint64_t reg_read(Setup *s, uint64_t offset)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, BASE + offset, 4, &value), BUS_OK);
	return value;
}

static void reg_write(Setup *s, uint64_t offset, uint64_t value)
{
	CHECK_EQ(bus_write(s->m->bus, BASE + offset, 4, value), BUS_OK);
}

/* 115313 baud, 8 bits. */
static void configure(Setup *s)
{
	reg_write(s, BAUD, 270);
	reg_write(s, LCR, 3);
}

static void advance(Setup *s, uint64_t t)
{
	event_queue_run_until(&s->m->events, t);
}

/* The host sends bytes: the UART takes them when the machine polls. */
static void type(Setup *s, const char *bytes, size_t len)
{
	s->serial.input = bytes;
	s->serial.input_len = len;
	s->serial.input_pos = 0;
	s->m->inputs[0].poll(s->m->inputs[0].ctx);
}

static void test_reset_values(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(reg_read(&s, AUX_ENABLES), 1);
	CHECK_EQ(reg_read(&s, AUX_IRQ), 0);
	CHECK_EQ(reg_read(&s, IER), 0);
	CHECK_EQ(reg_read(&s, IIR), 0xc1);
	CHECK_EQ(reg_read(&s, LCR), 0);
	CHECK_EQ(reg_read(&s, MCR), 0);
	CHECK_EQ(reg_read(&s, LSR), LSR_TX_IDLE | LSR_TX_EMPTY);
	CHECK_EQ(reg_read(&s, MSR), 0x20); /* CTS asserted */
	CHECK_EQ(reg_read(&s, SCRATCH), 0);
	CHECK_EQ(reg_read(&s, CNTL), 3);
	CHECK_EQ(reg_read(&s, STAT), STAT_RESET);
	CHECK_EQ(reg_read(&s, BAUD), 0);
	CHECK_EQ(reg_read(&s, IO), 0); /* nothing received */

	/* Unsupported bits read as 0; the scratch byte keeps its value. */
	reg_write(&s, IER, UINT32_MAX);
	CHECK_EQ(reg_read(&s, IER), 3);
	reg_write(&s, LCR, UINT32_MAX);
	CHECK_EQ(reg_read(&s, LCR), 0xc1);
	reg_write(&s, MCR, UINT32_MAX);
	CHECK_EQ(reg_read(&s, MCR), 2);
	reg_write(&s, SCRATCH, 0x1a5);
	CHECK_EQ(reg_read(&s, SCRATCH), 0xa5);
	reg_write(&s, BAUD, 0x12345);
	CHECK_EQ(reg_read(&s, BAUD), 0x2345);

	/* Each write changes its own register only. */
	reg_write(&s, MCR, 0);
	reg_write(&s, LCR, 0x83);
	CHECK_EQ(reg_read(&s, MCR), 0);
	reg_write(&s, LCR, 3);
	reg_write(&s, SCRATCH, 0);
	reg_write(&s, MCR, 2);
	CHECK_EQ(reg_read(&s, SCRATCH), 0);
	CHECK_EQ(reg_read(&s, CNTL), 3);
	reg_write(&s, IIR, 6);
	CHECK_EQ(reg_read(&s, LCR), 1);
	reg_write(&s, AUX_ENABLES, UINT32_MAX);
	CHECK_EQ(reg_read(&s, AUX_ENABLES), 7);

	/* Reset while busy: sending, receiving, an overrun, an interrupt. */
	reg_write(&s, AUX_ENABLES, 1);
	reg_write(&s, BAUD, 270);
	reg_write(&s, IER, 3);
	type(&s, "0123456789", 10);
	advance(&s, 9 * FRAME_8 + 1);
	reg_write(&s, IO, 'a');
	reg_write(&s, IO, 'b');
	type(&s, "c", 1);
	CHECK(irq_level(&s.irq));
	bcm2835_aux_reset(s.aux);
	CHECK(!irq_level(&s.irq));
	CHECK_EQ(event_queue_next(&s.m->events), EVENT_NEVER);
	CHECK_EQ(reg_read(&s, AUX_ENABLES), 0);
	s.serial.input_len = s.serial.input_pos; /* nothing more comes */
	reg_write(&s, AUX_ENABLES, 1);
	CHECK_EQ(reg_read(&s, IER), 0);
	CHECK_EQ(reg_read(&s, LCR), 0);
	CHECK_EQ(reg_read(&s, MCR), 0);
	CHECK_EQ(reg_read(&s, SCRATCH), 0);
	CHECK_EQ(reg_read(&s, BAUD), 0);
	CHECK_EQ(reg_read(&s, STAT), STAT_RESET);
	CHECK_EQ(reg_read(&s, LSR), LSR_TX_IDLE | LSR_TX_EMPTY);
	CHECK_EQ(s.serial.len, 0);
	teardown(&s);
}

/* DLAB gives the two halves of the baud rate in place of IO and IER. */
static void test_dlab(void)
{
	Setup s;

	setup(&s);
	reg_write(&s, IER, 1);
	reg_write(&s, BAUD, 0x0101);
	reg_write(&s, LCR, 0x83);
	reg_write(&s, IO, 0x134);
	CHECK_EQ(reg_read(&s, BAUD), 0x0134);
	reg_write(&s, IO, 0x35);
	CHECK_EQ(reg_read(&s, BAUD), 0x0135);
	reg_write(&s, IER, 0x112);
	CHECK_EQ(reg_read(&s, BAUD), 0x1235);
	reg_write(&s, IER, 0x13);
	CHECK_EQ(reg_read(&s, BAUD), 0x1335);
	CHECK_EQ(reg_read(&s, IO), 0x35);
	CHECK_EQ(reg_read(&s, IER), 0x13);
	CHECK_EQ(s.serial.len, 0);
	reg_write(&s, LCR, 3);
	CHECK_EQ(reg_read(&s, IER), 1);
	teardown(&s);
}

/* A byte reaches the host at the end of its frame; the next one follows. */
static void test_transmit(void)
{
	Setup s;

	setup(&s);
	configure(&s);
	reg_write(&s, IO, 0x161);                  /* only the low 8 bits */
	CHECK_EQ(reg_read(&s, LSR), LSR_TX_EMPTY); /* sending: not idle */
	CHECK_EQ(reg_read(&s, STAT), STAT_TX_EMPTY | STAT_SPACE | STAT_RX_IDLE);
	reg_write(&s, IO, 'b');
	CHECK_EQ(reg_read(&s, LSR), LSR_TX_EMPTY);
	CHECK_EQ(reg_read(&s, STAT),
	         1u << 24 | STAT_SPACE | STAT_RX_IDLE); /* busy, 1 in the FIFO */
	advance(&s, FRAME_8 - 1);
	CHECK(strcmp(s.serial.data, "") == 0);
	advance(&s, FRAME_8);
	CHECK(strcmp(s.serial.data, "a") == 0);
	advance(&s, 2 * FRAME_8);
	CHECK(strcmp(s.serial.data, "ab") == 0);
	CHECK_EQ(reg_read(&s, LSR), LSR_TX_IDLE | LSR_TX_EMPTY);
	CHECK_EQ(reg_read(&s, STAT), STAT_RESET);

	/* 7 bits: the top bit is not sent, and the frame is shorter. */
	reg_write(&s, LCR, 0);
	reg_write(&s, IO, 0xe3);
	advance(&s, 2 * FRAME_8 + FRAME_7 - 1);
	CHECK_EQ(s.serial.len, 2);
	advance(&s, 2 * FRAME_8 + FRAME_7);
	CHECK(strcmp(s.serial.data, "abc") == 0);

	/* After reset, the baud register is 0: 31.25 Mbaud. */
	bcm2835_aux_reset(s.aux);
	reg_write(&s, AUX_ENABLES, 1);
	reg_write(&s, IO, 'd');
	advance(&s, s.m->events.now + 287);
	CHECK_EQ(s.serial.len, 3);
	advance(&s, s.m->events.now + 1);
	CHECK(strcmp(s.serial.data, "abcd") == 0);
	teardown(&s);
}

/* Eight bytes wait in the FIFO, one more on the line; others are lost. */
static void test_transmit_fifo(void)
{
	Setup s;

	setup(&s);
	configure(&s);
	for (char c = '0'; c <= '9'; c++)
		reg_write(&s, IO, (uint64_t)c);
	CHECK_EQ(reg_read(&s, LSR), 0);
	CHECK_EQ(reg_read(&s, STAT), 8u << 24 | STAT_TX_FULL | STAT_RX_IDLE);
	advance(&s, 20 * FRAME_8);
	CHECK(strcmp(s.serial.data, "012345678") == 0);

	/* IIR bit 2 empties the transmit FIFO; bit 1 the receive FIFO. */
	for (char c = 'a'; c <= 'e'; c++)
		reg_write(&s, IO, (uint64_t)c);
	reg_write(&s, IIR, 4);
	CHECK_EQ(reg_read(&s, STAT) >> 24, 0);
	advance(&s, 40 * FRAME_8);
	CHECK(strcmp(s.serial.data, "012345678a") == 0);
	teardown(&s);
}

/*
 * The transmitter sends only while the mini UART and the transmitter are
 * enabled; one that is disabled finishes its byte.
 */
static void test_transmit_enable(void)
{
	Setup s;

	setup(&s);
	configure(&s);
	reg_write(&s, CNTL, 1);
	reg_write(&s, IO, 'x');
	advance(&s, 2 * FRAME_8);
	CHECK_EQ(s.serial.len, 0);
	reg_write(&s, CNTL, 3); /* x starts at once */
	advance(&s, 3 * FRAME_8);
	CHECK(strcmp(s.serial.data, "x") == 0);
	reg_write(&s, IO, 'y');
	reg_write(&s, IO, 'z');
	reg_write(&s, AUX_ENABLES, 0); /* y is on the line: it finishes */
	advance(&s, 6 * FRAME_8);
	CHECK(strcmp(s.serial.data, "xy") == 0);
	reg_write(&s, AUX_ENABLES, 1);
	advance(&s, 7 * FRAME_8);
	CHECK(strcmp(s.serial.data, "xyz") == 0);
	teardown(&s);
}

/*
 * Receiving: a byte comes in the time of its frame, into the FIFO; when
 * it is full, bytes are lost and the overrun bits set.
 */
static void test_receive(void)
{
	Setup s;

	setup(&s);
	configure(&s);
	type(&s, "hi", 2);
	CHECK_EQ(reg_read(&s, STAT), STAT_RESET & ~STAT_RX_IDLE);
	advance(&s, FRAME_8 - 1);
	CHECK_EQ(reg_read(&s, LSR) & LSR_DATA_READY, 0);
	advance(&s, 2 * FRAME_8);
	CHECK_EQ(reg_read(&s, LSR), LSR_TX_IDLE | LSR_TX_EMPTY | LSR_DATA_READY);
	CHECK_EQ(reg_read(&s, STAT), 2u << 16 | STAT_RESET | STAT_SYMBOL);
	CHECK_EQ(reg_read(&s, IO), 'h');
	CHECK_EQ(reg_read(&s, IO), 'i');
	CHECK_EQ(reg_read(&s, IO), 0);

	/* Ten bytes: eight fit, two are lost. */
	type(&s, "0123456789", 10);
	advance(&s, 20 * FRAME_8);
	CHECK_EQ(reg_read(&s, STAT) >> 16 & 0xf, 8);
	CHECK(reg_read(&s, STAT) & STAT_OVERRUN);
	CHECK_EQ(reg_read(&s, LSR) & LSR_OVERRUN, LSR_OVERRUN);
	CHECK_EQ(reg_read(&s, LSR) & LSR_OVERRUN, 0);
	CHECK_EQ(reg_read(&s, STAT) & STAT_OVERRUN, 0);
	CHECK_EQ(reg_read(&s, IO), '0');
	reg_write(&s, IO, 'x');
	reg_write(&s, IO, 'y'); /* one in the transmit FIFO */
	reg_write(&s, IIR, 4);
	CHECK_EQ(reg_read(&s, STAT) >> 16 & 0xf, 7);
	CHECK_EQ(reg_read(&s, STAT) >> 24, 0);
	reg_write(&s, IO, 'z');
	reg_write(&s, IIR, 2);
	CHECK_EQ(reg_read(&s, STAT) >> 16 & 0xf, 0);
	CHECK_EQ(reg_read(&s, STAT) >> 24, 1);

	/* 7 bits; and the host's bytes are lost while the receiver is off. */
	reg_write(&s, LCR, 0);
	type(&s, "\xc1", 1);
	advance(&s, s.m->events.now + FRAME_7);
	CHECK_EQ(reg_read(&s, IO), 0x41);
	reg_write(&s, CNTL, 2);
	type(&s, "ab", 2);
	CHECK_EQ(s.serial.input_pos, 2);
	reg_write(&s, CNTL, 3);
	advance(&s, s.m->events.now + 4 * FRAME_8);
	CHECK_EQ(reg_read(&s, STAT) >> 16 & 0xf, 0);
	teardown(&s);
}

/*
 * The interrupt: bit 0 of IER for the receiver, bit 1 for the
 * transmitter, as in a 16550. IIR names the one pending, the receiver
 * first, and AUX_IRQ shows it.
 */
static void test_interrupts(void)
{
	Setup s;

	setup(&s);
	configure(&s);
	type(&s, "q", 1);
	advance(&s, FRAME_8);
	CHECK(!irq_level(&s.irq)); /* a byte, but no interrupt enabled */
	CHECK_EQ(reg_read(&s, IIR), 0xc1);
	reg_write(&s, IER, 2);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(reg_read(&s, IIR), 0xc2); /* the receiver's is off */
	CHECK_EQ(reg_read(&s, IO), 'q');
	CHECK_EQ(reg_read(&s, AUX_IRQ), 1);
	reg_write(&s, IO, 'a'); /* straight onto the line */
	CHECK(irq_level(&s.irq));
	reg_write(&s, IO, 'b');
	CHECK(!irq_level(&s.irq));
	CHECK_EQ(reg_read(&s, IIR), 0xc1);
	CHECK_EQ(reg_read(&s, AUX_IRQ), 0);
	advance(&s, s.m->events.now + FRAME_8);
	CHECK(irq_level(&s.irq));

	reg_write(&s, IER, 1);
	CHECK(!irq_level(&s.irq));
	type(&s, "z", 1);
	advance(&s, s.m->events.now + FRAME_8);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(reg_read(&s, IIR), 0xc4);
	reg_write(&s, IER, 3);
	CHECK_EQ(reg_read(&s, IIR), 0xc4);
	reg_write(&s, AUX_ENABLES, 0);
	CHECK(!irq_level(&s.irq));
	CHECK_EQ(reg_read(&s, AUX_IRQ), 0);
	reg_write(&s, AUX_ENABLES, 1);
	reg_write(&s, IER, 1);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(reg_read(&s, IO), 'z');
	CHECK(!irq_level(&s.irq));
	reg_write(&s, IER, 3);
	CHECK_EQ(reg_read(&s, IIR), 0xc2);
	teardown(&s);
}

/*
 * A line held low brings 0x00 after 0x00 until the FIFO is full and the
 * overrun set; reading makes room for more.
 */
static void test_line_low(void)
{
	Setup s;

	setup(&s);
	configure(&s);
	s.serial.low = true;
	s.m->inputs[0].poll(s.m->inputs[0].ctx);
	CHECK_EQ(reg_read(&s, STAT) & STAT_RX_IDLE, 0);
	advance(&s, 9 * FRAME_8);
	CHECK_EQ(reg_read(&s, STAT) >> 16 & 0xf, 8);
	CHECK(reg_read(&s, STAT) & STAT_OVERRUN);
	CHECK_EQ(event_queue_next(&s.m->events), EVENT_NEVER);
	CHECK(!s.m->inputs[0].wait(s.m->inputs[0].ctx));
	reg_write(&s, IIR, 2); /* room again */
	CHECK(event_queue_next(&s.m->events) != EVENT_NEVER);
	advance(&s, 18 * FRAME_8);
	CHECK_EQ(reg_read(&s, STAT) >> 16 & 0xf, 8);
	CHECK_EQ(event_queue_next(&s.m->events), EVENT_NEVER);
	CHECK_EQ(reg_read(&s, IO), 0);
	CHECK(event_queue_next(&s.m->events) != EVENT_NEVER);
	advance(&s, 19 * FRAME_8);
	CHECK_EQ(reg_read(&s, STAT) >> 16 & 0xf, 8);
	CHECK_EQ(event_queue_next(&s.m->events), EVENT_NEVER);
	CHECK(reg_read(&s, LSR) & LSR_OVERRUN);
	CHECK(event_queue_next(&s.m->events) != EVENT_NEVER);
	reg_write(&s, IIR, 2); /* a byte is on the line */
	advance(&s, 21 * FRAME_8);
	CHECK_EQ(reg_read(&s, STAT) >> 16 & 0xf, 2);
	teardown(&s);
}

/* The machine's input: wait() takes a byte when the receiver listens. */
static void test_wait(void)
{
	Setup s;
	MachineInput *input;

	setup(&s);
	configure(&s);
	input = &s.m->inputs[0];
	s.serial.input = "w";
	s.serial.input_len = 1;
	CHECK(input->wait(input->ctx));
	advance(&s, FRAME_8);
	CHECK_EQ(reg_read(&s, IO), 'w');
	CHECK(!input->wait(input->ctx)); /* the input has ended */

	/* A receiver that is off does not wait, and does not take input. */
	reg_write(&s, CNTL, 2);
	s.serial.input = "v";
	s.serial.input_pos = 0;
	CHECK(!input->wait(input->ctx));
	CHECK_EQ(s.serial.input_pos, 0);
	reg_write(&s, CNTL, 3); /* v starts at once */
	advance(&s, 2 * FRAME_8);
	CHECK_EQ(reg_read(&s, IO), 'v');

	/* A line held low always has something. */
	s.serial.low = true;
	CHECK(input->wait(input->ctx));
	reg_write(&s, AUX_ENABLES, 0);
	CHECK(!input->wait(input->ctx));
	teardown(&s);
}

/* RTS: from MCR, or with receive flow control from the receive FIFO. */
static void test_rts(void)
{
	Setup s;

	setup(&s);
	configure(&s);
	reg_write(&s, MCR, 2);
	CHECK(reg_read(&s, STAT) & STAT_RTS);
	reg_write(&s, CNTL, 3 | 1 << 2); /* de-assert with 3 free entries */
	type(&s, "abcd", 4);
	advance(&s, 4 * FRAME_8);
	CHECK(reg_read(&s, STAT) & STAT_RTS);
	type(&s, "e", 1);
	advance(&s, 5 * FRAME_8);
	CHECK_EQ(reg_read(&s, STAT) & STAT_RTS, 0);
	reg_write(&s, CNTL, 3 | 1 << 2 | 3 << 4); /* with 4 free entries */
	CHECK_EQ(reg_read(&s, STAT) & STAT_RTS, 0);
	CHECK_EQ(reg_read(&s, IO), 'a');
	CHECK_EQ(reg_read(&s, STAT) & STAT_RTS, 0); /* 4 free entries */
	CHECK_EQ(reg_read(&s, IO), 'b');
	CHECK(reg_read(&s, STAT) & STAT_RTS);
	reg_write(&s, CNTL, 3 | 1 << 2 | 1 << 4); /* with 2 free entries */
	CHECK(reg_read(&s, STAT) & STAT_RTS);     /* 5 free */
	type(&s, "jk", 2);
	advance(&s, 7 * FRAME_8);
	CHECK(reg_read(&s, STAT) & STAT_RTS); /* 3 free */
	CHECK_EQ(reg_read(&s, IO), 'c');
	CHECK_EQ(reg_read(&s, IO), 'd');
	reg_write(&s, CNTL, 3 | 1 << 2 | 2 << 4); /* with 1 free entry */
	type(&s, "fgh", 3);
	advance(&s, 10 * FRAME_8);
	CHECK(reg_read(&s, STAT) & STAT_RTS);     /* 2 free */
	reg_write(&s, CNTL, 3 | 1 << 2 | 1 << 4); /* with 2 free entries */
	CHECK_EQ(reg_read(&s, STAT) & STAT_RTS, 0);
	reg_write(&s, CNTL, 3 | 1 << 2 | 2 << 4); /* with 1 free entry */
	type(&s, "i", 1);
	advance(&s, 11 * FRAME_8);
	CHECK_EQ(reg_read(&s, STAT) & STAT_RTS, 0); /* 1 free */
	reg_write(&s, CNTL, 3 | 1 << 2 | 1 << 4);   /* with 2 free entries */
	CHECK_EQ(reg_read(&s, CNTL), 3 | 1 << 2 | 1 << 4);
	teardown(&s);
}

/* What the documents do not describe, or the model lacks, stops it. */
static void test_stops(void)
{
	static const struct {
		bool write;
		uint64_t offset;
		unsigned size;
		bool disabled;
		const char *message;
	} stops[] = {
		{false, IO, 1, false,
	     "1-byte read from offset 0x40 is not described by the datasheet"},
		{false, 0x08, 4, false, "offset 0x08 is not described"},
		{true, 0x3c, 4, false, "offset 0x3c is not described"},
		{false, 0x6c, 4, false, "offset 0x6c is not described"},
		{false, 0x7c, 4, false, "offset 0x7c is not described"},
		{false, 0xd8, 4, false, "offset 0xd8 is not described"},
		{true, 0x42, 4, false, "offset 0x42 is not described"},
		{false, 0x41, 4, false, "offset 0x41 is not described"},
		{false, 0x80, 4, false,
	     "4-byte read from offset 0x80 is not implemented yet (the SPI "
	     "masters)"},
		{true, 0xd4, 4, false, "offset 0xd4 is not implemented yet"},
		{false, IO, 4, true,
	     "offset 0x40 is not possible: AUX_ENABLES disables the mini UART"},
		{true, BAUD, 4, true, "offset 0x68 is not possible: AUX_ENABLES"},
		{true, AUX_IRQ, 4, false,
	     "4-byte write to offset 0x00 is not possible: it is read-only"},
		{true, LSR, 4, false, "offset 0x54 is not possible: it is read-only"},
		{true, MSR, 4, false, "offset 0x58 is not possible: it is read-only"},
		{true, STAT, 4, false, "offset 0x64 is not possible: it is read-only"},
		{true, CNTL, 4, false,
	     "4-byte write to offset 0x60 is not implemented yet (transmit flow "
	     "control with CTS)"},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(stops); i++) {
		Setup s;
		uint64_t value = 0;

		setup(&s);
		reg_write(&s, BAUD, 0x1234);
		if (stops[i].disabled)
			reg_write(&s, AUX_ENABLES, 6);
		if (stops[i].write)
			bus_write(s.m->bus, BASE + stops[i].offset, stops[i].size, 0xff);
		else
			bus_read(s.m->bus, BASE + stops[i].offset, stops[i].size, &value);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		CHECK(strstr(s.m->stop_message, "bcm2835-aux: ") != NULL);
		/* The access did nothing else. */
		CHECK_EQ(value, 0);
		reg_write(&s, AUX_ENABLES, 1);
		CHECK_EQ(reg_read(&s, BAUD), 0x1234);
		CHECK_EQ(reg_read(&s, CNTL), 3);
		CHECK_EQ(reg_read(&s, STAT), STAT_RESET);
		teardown(&s);
	}
}

/* Creating: the clock must not be 0, and the machine needs room. */
static void test_create(void)
{
	Machine *m = machine_create();
	Capture serial = {.stream.ops = &capture_ops};
	static const CharStreamOps output_only = {
		.write = capture_write,
		.destroy = capture_destroy,
	};
	Capture out = {.stream.ops = &output_only};
	Bcm2835Aux *aux;
	int contexts[MACHINE_MAX_INPUTS];

	CHECK(bcm2835_aux_create(m, 0, NULL, NULL) == NULL);
	aux = bcm2835_aux_create(m, CLOCK_HZ, &out.stream, NULL);
	CHECK_EQ(m->num_inputs, 0);
	bcm2835_aux_destroy(aux);
	aux = bcm2835_aux_create(m, CLOCK_HZ, &serial.stream, NULL);
	CHECK(m->num_inputs == 1 && m->inputs[0].ctx == aux);
	bcm2835_aux_destroy(aux);
	CHECK_EQ(m->num_inputs, 0);
	for (unsigned i = 0; i < MACHINE_MAX_INPUTS; i++)
		CHECK(machine_add_input(m, NULL, NULL, &contexts[i]));
	CHECK(bcm2835_aux_create(m, CLOCK_HZ, &serial.stream, NULL) == NULL);
	machine_destroy(m);
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_dlab);
	RUN_TEST(test_transmit);
	RUN_TEST(test_transmit_fifo);
	RUN_TEST(test_transmit_enable);
	RUN_TEST(test_receive);
	RUN_TEST(test_interrupts);
	RUN_TEST(test_line_low);
	RUN_TEST(test_wait);
	RUN_TEST(test_rts);
	RUN_TEST(test_stops);
	RUN_TEST(test_create);
	return test_summary();
}
