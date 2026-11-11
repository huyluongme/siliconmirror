/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the PL011 UART (src/hw/char/pl011.c). Expected values
 * come from the PL011 Technical Reference Manual, ARM DDI 0183G.
 *
 * The UART runs from a 48 MHz UARTCLK, as on the Raspberry Pi. Most tests
 * set it to 115200 baud: UARTIBRD 26 and UARTFBRD 3, a divisor of
 * 26 + 3/64, so that an 8N1 frame of 10 bits takes 86823 ns.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/char_stream.h"
#include "core/machine.h"
#include "hw/char/pl011.h"
#include "test.h"
#include "util/macros.h"

#define BASE 0x3f201000
#define CLOCK_HZ 48000000
#define FRAME_8N1 86823 /* ns: 10 * 16 * (26 + 3/64) / 48 MHz, rounded up */

enum {
	DR = 0x000,
	RSR_ECR = 0x004,
	FR = 0x018,
	ILPR = 0x020,
	IBRD = 0x024,
	FBRD = 0x028,
	LCR_H = 0x02c,
	CR = 0x030,
	IFLS = 0x034,
	IMSC = 0x038,
	RIS = 0x03c,
	MIS = 0x040,
	ICR = 0x044,
	DMACR = 0x048,
};

enum {
	FR_BUSY = 0x08,
	FR_RXFE = 0x10,
	FR_TXFF = 0x20,
	FR_TXFE = 0x80,
	LCR_H_PEN = 0x02,
	LCR_H_STP2 = 0x08,
	LCR_H_FEN = 0x10,
	LCR_H_WLEN_7 = 0x40,
	LCR_H_WLEN_8 = 0x60,
	CR_UARTEN = 0x001,
	CR_TXE = 0x100,
	CR_RXE = 0x200,
	INT_TX = 0x20,
};

/* A character stream that collects everything written to it. */
typedef struct Capture {
	CharStream stream;
	char data[128];
	size_t len;
} Capture;

static void capture_write(CharStream *s, const uint8_t *buf, size_t len)
{
	Capture *c = container_of(s, Capture, stream);

	for (size_t i = 0; i < len && c->len < sizeof(c->data) - 1; i++)
		c->data[c->len++] = (char)buf[i];
	c->data[c->len] = '\0';
}

static void capture_destroy(CharStream *s)
{
	(void)s;
}

static const CharStreamOps capture_ops = {
	.write = capture_write,
	.destroy = capture_destroy,
};

/* A machine with one UART at BASE that writes into a Capture. */
typedef struct Setup {
	Machine *m;
	Pl011 *uart;
	Capture out;
	IrqLine irq;
} Setup;

static void setup(Setup *s, const Pl011Config *config)
{
	memset(s, 0, sizeof(*s));
	s->out.stream.ops = &capture_ops;
	s->m = machine_create();
	CHECK(s->m != NULL);
	irq_init(&s->irq, NULL, NULL, 0);
	s->uart = pl011_create(s->m, config, CLOCK_HZ, &s->out.stream, &s->irq);
	CHECK(s->uart != NULL);
	CHECK_EQ(pl011_map(s->uart, s->m->bus, "uart", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	pl011_destroy(s->uart);
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

static void send(Setup *s, const char *text)
{
	for (; *text != '\0'; text++)
		reg_write(s, DR, (uint8_t)*text);
}

/* 115200 baud, then the frame format lcr_h. */
static void configure(Setup *s, uint32_t lcr_h)
{
	reg_write(s, IBRD, 26);
	reg_write(s, FBRD, 3);
	reg_write(s, LCR_H, lcr_h);
}

/* Move virtual time on to t. */
static void advance(Setup *s, uint64_t t)
{
	event_queue_run_until(&s->m->events, t);
}

/* Let the UART send everything it can. */
static void drain(Setup *s)
{
	advance(s, s->m->events.now + UINT64_C(1000000000));
}

static void test_reset_values(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);
	CHECK_EQ(reg_read(&s, CR), CR_RXE | CR_TXE);
	CHECK_EQ(reg_read(&s, IFLS), 0x12);
	CHECK_EQ(reg_read(&s, ILPR), 0);
	CHECK_EQ(reg_read(&s, IBRD), 0);
	CHECK_EQ(reg_read(&s, FBRD), 0);
	CHECK_EQ(reg_read(&s, LCR_H), 0);
	CHECK_EQ(reg_read(&s, IMSC), 0);
	CHECK_EQ(reg_read(&s, RIS), 0);
	CHECK_EQ(reg_read(&s, MIS), 0);
	CHECK_EQ(reg_read(&s, DMACR), 0);
	CHECK_EQ(reg_read(&s, RSR_ECR), 0);
	teardown(&s);
}

static void test_id_registers_r1p5(void)
{
	static const uint8_t expected[8] = {
		0x11, 0x10, 0x34, 0x00, /* UARTPeriphID0-3, r1p5 */
		0x0d, 0xf0, 0x05, 0xb1, /* UARTPCellID0-3 */
	};
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	for (unsigned i = 0; i < 8; i++)
		CHECK_EQ(reg_read(&s, 0xfe0 + 4 * i), expected[i]);
	teardown(&s);
}

static void test_id_registers_not_implemented(void)
{
	static const Pl011Config no_id = {.fifo_depth = 16, .periph_id = 0};
	Setup s;

	setup(&s, &no_id);
	for (unsigned i = 0; i < 8; i++)
		CHECK_EQ(reg_read(&s, 0xfe0 + 4 * i), 0);
	teardown(&s);
}

static void test_transmit_when_enabled(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_FEN | LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE | CR_RXE);
	send(&s, "Hello");
	CHECK_EQ(s.out.len, 0);
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_RXFE);
	drain(&s);
	CHECK(strcmp(s.out.data, "Hello") == 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);
	teardown(&s);
}

/* Sections 2.4.3 and 3.3.6: a frame at the baud rate, back to back. */
static void test_frame_time(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_FEN | LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	send(&s, "AB");

	/* A leaves the FIFO at once: TXFE, but BUSY while it is sent. */
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_RXFE);
	advance(&s, FRAME_8N1 - 1);
	CHECK_EQ(s.out.len, 0);
	advance(&s, FRAME_8N1);
	CHECK(strcmp(s.out.data, "A") == 0);
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_TXFE | FR_RXFE);
	advance(&s, 2 * FRAME_8N1 - 1);
	CHECK_EQ(s.out.len, 1);
	advance(&s, 2 * FRAME_8N1);
	CHECK(strcmp(s.out.data, "AB") == 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);

	/* 7 data bits, parity and 2 stop bits: 11 bits. */
	configure(&s, LCR_H_WLEN_7 | LCR_H_PEN | LCR_H_STP2);
	send(&s, "C");
	advance(&s, 2 * FRAME_8N1 + 95506 - 1);
	CHECK_EQ(s.out.len, 2);
	advance(&s, 2 * FRAME_8N1 + 95506);
	CHECK(strcmp(s.out.data, "ABC") == 0);
	teardown(&s);
}

/*
 * Section 3.3.7: UARTIBRD and UARTFBRD take effect at the next write of
 * UARTLCR_H. Section 3.3.6: a divisor below 1 is invalid, and so is
 * 0xffff with a fraction.
 */
static void test_divisor(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	reg_write(&s, IBRD, 26);
	reg_write(&s, FBRD, 3);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	send(&s, "A");
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message, "UARTIBRD 0 and UARTFBRD 0, is invalid") !=
	      NULL);
	s.m->stop = MACHINE_STOP_NONE;
	reg_write(&s, LCR_H, LCR_H_WLEN_8); /* takes the divisor */
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	advance(&s, FRAME_8N1);
	CHECK(strcmp(s.out.data, "A") == 0);

	/* The largest divisor, 65535, without a fraction. */
	reg_write(&s, IBRD, 0xffff);
	reg_write(&s, FBRD, 0);
	reg_write(&s, LCR_H, LCR_H_WLEN_8);
	send(&s, "B");
	advance(&s, FRAME_8N1 + 218450000 - 1);
	CHECK_EQ(s.out.len, 1);
	advance(&s, FRAME_8N1 + 218450000);
	CHECK(strcmp(s.out.data, "AB") == 0);
	reg_write(&s, FBRD, 1);
	reg_write(&s, LCR_H, LCR_H_WLEN_8);
	send(&s, "C");
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message, "UARTIBRD 65535 and UARTFBRD 1") != NULL);
	CHECK_EQ(s.out.len, 2);
	teardown(&s);
}

static void test_byte_write_to_dr(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	CHECK_EQ(bus_write(s.m->bus, BASE + DR, 1, 'x'), BUS_OK);
	drain(&s);
	CHECK(strcmp(s.out.data, "x") == 0);
	teardown(&s);
}

static void test_disabled_uart_holds_data(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_WLEN_8);

	/* After reset the UART is disabled: the byte waits. */
	send(&s, "A");
	drain(&s);
	CHECK_EQ(s.out.len, 0);

	/* Without FEN the FIFO is one byte deep, so it is now full. */
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_TXFF | FR_RXFE);
	send(&s, "B");

	/* Enabling the UART sends the waiting byte; B was dropped. */
	reg_write(&s, CR, CR_UARTEN | CR_TXE | CR_RXE);
	drain(&s);
	CHECK(strcmp(s.out.data, "A") == 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);
	teardown(&s);
}

/* Section 3.3.8: disabled while sending, the UART finishes the byte. */
static void test_disabled_while_sending(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_FEN | LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	send(&s, "AB");
	advance(&s, 1000);
	reg_write(&s, CR, 0);
	drain(&s);
	CHECK(strcmp(s.out.data, "A") == 0);
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_RXFE);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	drain(&s);
	CHECK(strcmp(s.out.data, "AB") == 0);
	teardown(&s);
}

static void test_transmitter_disabled_holds_data(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_RXE);
	send(&s, "A");
	drain(&s);
	CHECK_EQ(s.out.len, 0);
	reg_write(&s, CR, CR_UARTEN | CR_TXE | CR_RXE);
	drain(&s);
	CHECK(strcmp(s.out.data, "A") == 0);
	teardown(&s);
}

static void test_fifo_depth(void)
{
	static const Pl011Config bcm2835_like = {.fifo_depth = 16};
	Setup s;

	setup(&s, &bcm2835_like);
	configure(&s, LCR_H_FEN | LCR_H_WLEN_8);

	/* 15 bytes: not full yet. */
	send(&s, "abcdefghijklmno");
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_RXFE);

	/* The 16th fills the FIFO; the 17th is dropped. */
	send(&s, "pq");
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_TXFF | FR_RXFE);

	reg_write(&s, CR, CR_UARTEN | CR_TXE | CR_RXE);
	drain(&s);
	CHECK(strcmp(s.out.data, "abcdefghijklmnop") == 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);
	teardown(&s);
}

static void test_word_length(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);

	/* WLEN = 0b00: 5 data bits, 0b11: 8 data bits. */
	for (uint32_t wlen = 0; wlen < 4; wlen++) {
		reg_write(&s, LCR_H, wlen << 5);
		reg_write(&s, DR, 0xff);
		drain(&s);
	}

	CHECK_EQ(s.out.len, 4);
	CHECK_EQ((uint8_t)s.out.data[0], 0x1f);
	CHECK_EQ((uint8_t)s.out.data[1], 0x3f);
	CHECK_EQ((uint8_t)s.out.data[2], 0x7f);
	CHECK_EQ((uint8_t)s.out.data[3], 0xff);
	teardown(&s);
}

/*
 * Section 2.8.3: without FIFOs, the transmit interrupt sets when the
 * holding register empties, and a write clears it.
 */
static void test_tx_interrupt_without_fifo(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	reg_write(&s, IMSC, INT_TX);

	/* Not before anything was written. */
	CHECK_EQ(reg_read(&s, RIS), 0);
	CHECK(!irq_level(&s.irq));

	/* A goes from the holding register to the transmitter at once. */
	send(&s, "A");
	CHECK_EQ(reg_read(&s, RIS), INT_TX);
	CHECK_EQ(reg_read(&s, MIS), INT_TX);
	CHECK(irq_level(&s.irq));
	send(&s, "B");
	CHECK_EQ(reg_read(&s, RIS), 0);
	CHECK(!irq_level(&s.irq));
	advance(&s, FRAME_8N1);
	CHECK_EQ(reg_read(&s, RIS), INT_TX);
	CHECK(irq_level(&s.irq));

	/* Masked, it stays raw; cleared, it goes. */
	reg_write(&s, IMSC, 0);
	CHECK_EQ(reg_read(&s, RIS), INT_TX);
	CHECK_EQ(reg_read(&s, MIS), 0);
	CHECK(!irq_level(&s.irq));
	reg_write(&s, IMSC, INT_TX);
	CHECK(irq_level(&s.irq));
	reg_write(&s, ICR, ~(uint32_t)INT_TX);
	CHECK_EQ(reg_read(&s, RIS), INT_TX);
	reg_write(&s, ICR, INT_TX);
	CHECK_EQ(reg_read(&s, RIS), 0);
	CHECK(!irq_level(&s.irq));
	teardown(&s);
}

/*
 * Sections 2.8.3 and 3.3.9: with FIFOs, the transmit interrupt sets when
 * the FIFO goes through the trigger level, down, and clears when writes
 * take it above.
 */
static void test_tx_interrupt_with_fifo(void)
{
	static const Pl011Config bcm2835_like = {.fifo_depth = 16};
	Setup s;

	setup(&s, &bcm2835_like);
	configure(&s, LCR_H_FEN | LCR_H_WLEN_8);
	reg_write(&s, IMSC, INT_TX);

	/* The level at 1/2, 8 bytes: 12 bytes wait, one leaves at once. */
	send(&s, "abcdefghijkl");
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	CHECK_EQ(reg_read(&s, RIS), 0);

	/* At 8 bytes, as d starts, three frames on. */
	advance(&s, 3 * FRAME_8N1 - 1);
	CHECK_EQ(reg_read(&s, RIS), 0);
	advance(&s, 3 * FRAME_8N1);
	CHECK_EQ(reg_read(&s, RIS), INT_TX);
	CHECK(irq_level(&s.irq));

	/* Writing to 9 bytes clears it; going down to 8 sets it again. */
	send(&s, "m");
	CHECK_EQ(reg_read(&s, RIS), 0);
	advance(&s, 4 * FRAME_8N1);
	CHECK_EQ(reg_read(&s, RIS), INT_TX);
	drain(&s);
	CHECK(strcmp(s.out.data, "abcdefghijklm") == 0);

	/* At 1/8, 2 bytes: a byte that leaves at once never went through. */
	reg_write(&s, ICR, INT_TX);
	reg_write(&s, IFLS, 0);
	send(&s, "n");
	CHECK_EQ(reg_read(&s, RIS), 0);

	/* At 7/8, 14 bytes; at 3/4, 12; at 1/4, 4. */
	drain(&s);
	reg_write(&s, CR, 0);
	reg_write(&s, IFLS, 4);
	send(&s, "123456789012345");
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	CHECK_EQ(reg_read(&s, RIS), INT_TX); /* 15 to 14 */
	reg_write(&s, ICR, INT_TX);
	reg_write(&s, IFLS, 3);
	advance(&s, s.m->events.now + 2 * FRAME_8N1);
	CHECK_EQ(reg_read(&s, RIS), INT_TX); /* 13 to 12 */
	reg_write(&s, ICR, INT_TX);
	reg_write(&s, IFLS, 1);
	advance(&s, s.m->events.now + 7 * FRAME_8N1);
	CHECK_EQ(reg_read(&s, RIS), 0); /* 5 left */
	advance(&s, s.m->events.now + FRAME_8N1);
	CHECK_EQ(reg_read(&s, RIS), INT_TX); /* 5 to 4 */

	/* Going on down from the level is no transition through it. */
	reg_write(&s, ICR, INT_TX);
	advance(&s, s.m->events.now + FRAME_8N1);
	CHECK_EQ(reg_read(&s, RIS), 0); /* 4 to 3 */
	teardown(&s);
}

/* Section 3.3.9: trigger levels 0b101 to 0b111 are reserved. */
static void test_reserved_level(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_FEN | LCR_H_WLEN_8);
	reg_write(&s, IFLS, 5);
	send(&s, "A");
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message,
	             "transmit FIFO level 5 in UARTIFLS is reserved") != NULL);
	teardown(&s);
}

static void test_register_widths(void)
{
	static const struct {
		uint64_t offset;
		uint64_t mask;
	} regs[] = {
		{ILPR, 0xff}, {IBRD, 0xffff}, {FBRD, 0x3f},  {LCR_H, 0xff},
		{CR, 0xff87}, {IFLS, 0x3f},   {IMSC, 0x7ff}, {DMACR, 0x7},
	};
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
		reg_write(&s, regs[i].offset, 0xffffffff);
		CHECK_EQ(reg_read(&s, regs[i].offset), regs[i].mask);
	}
	teardown(&s);
}

static void test_read_only_and_reserved(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	reg_write(&s, FR, 0xffffffff);
	reg_write(&s, RIS, 0xffffffff);
	reg_write(&s, 0xfe0, 0xffffffff);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);
	CHECK_EQ(reg_read(&s, RIS), 0);
	CHECK_EQ(reg_read(&s, 0xfe0), 0x11);

	/* Reserved space reads as 0. */
	CHECK_EQ(reg_read(&s, 0x008), 0);
	CHECK_EQ(reg_read(&s, 0x800), 0);
	teardown(&s);
}

static void test_receive_not_implemented(void)
{
	Setup s;

	/* Until receiving exists, these registers do nothing. */
	setup(&s, &pl011_arm_r1p5);
	CHECK_EQ(reg_read(&s, DR), 0);
	reg_write(&s, RSR_ECR, 0xff);
	reg_write(&s, ICR, 0x7ff);
	reg_write(&s, IMSC, 0x7ff);
	CHECK_EQ(reg_read(&s, RSR_ECR), 0);
	CHECK_EQ(reg_read(&s, RIS), 0);
	CHECK_EQ(reg_read(&s, MIS), 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);
	teardown(&s);
}

static void test_reset_clears_state(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	configure(&s, LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	reg_write(&s, IMSC, INT_TX);
	send(&s, "h"); /* being sent, and the holding register empty */
	CHECK(irq_level(&s.irq));
	pl011_reset(s.uart);
	CHECK(!irq_level(&s.irq));

	CHECK_EQ(reg_read(&s, IBRD), 0);
	CHECK_EQ(reg_read(&s, LCR_H), 0);
	CHECK_EQ(reg_read(&s, RIS), 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);

	/* The divisor that UARTLCR_H took is back to 0 too. */
	reg_write(&s, IBRD, 26);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	send(&s, "x");
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	s.m->stop = MACHINE_STOP_NONE;
	pl011_reset(s.uart);

	/* The bytes that were waiting, or being sent, are gone. */
	configure(&s, LCR_H_FEN | LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	drain(&s);
	CHECK_EQ(s.out.len, 0);
	teardown(&s);
}

static void test_unconnected_output(void)
{
	Machine *m = machine_create();
	Pl011 *uart = pl011_create(m, &pl011_arm_r1p5, CLOCK_HZ, NULL, NULL);

	CHECK_EQ(pl011_map(uart, m->bus, "uart", BASE), BUS_OK);
	CHECK_EQ(bus_write(m->bus, BASE + IBRD, 4, 1), BUS_OK);
	CHECK_EQ(bus_write(m->bus, BASE + LCR_H, 4, LCR_H_WLEN_8), BUS_OK);
	CHECK_EQ(bus_write(m->bus, BASE + IMSC, 4, INT_TX), BUS_OK);
	CHECK_EQ(bus_write(m->bus, BASE + CR, 4, CR_UARTEN | CR_TXE), BUS_OK);
	CHECK_EQ(bus_write(m->bus, BASE + DR, 4, 'x'), BUS_OK);
	event_queue_run_until(&m->events, 1000000);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	pl011_destroy(uart);
	machine_destroy(m);
}

static void test_invalid_config(void)
{
	static const Pl011Config empty = {.fifo_depth = 0};
	static const Pl011Config too_deep = {.fifo_depth = 64};

	CHECK(pl011_create(NULL, &empty, CLOCK_HZ, NULL, NULL) == NULL);
	CHECK(pl011_create(NULL, &too_deep, CLOCK_HZ, NULL, NULL) == NULL);
	CHECK(pl011_create(NULL, &pl011_arm_r1p5, 0, NULL, NULL) == NULL);
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_id_registers_r1p5);
	RUN_TEST(test_id_registers_not_implemented);
	RUN_TEST(test_transmit_when_enabled);
	RUN_TEST(test_frame_time);
	RUN_TEST(test_divisor);
	RUN_TEST(test_byte_write_to_dr);
	RUN_TEST(test_disabled_uart_holds_data);
	RUN_TEST(test_disabled_while_sending);
	RUN_TEST(test_transmitter_disabled_holds_data);
	RUN_TEST(test_fifo_depth);
	RUN_TEST(test_word_length);
	RUN_TEST(test_tx_interrupt_without_fifo);
	RUN_TEST(test_tx_interrupt_with_fifo);
	RUN_TEST(test_reserved_level);
	RUN_TEST(test_register_widths);
	RUN_TEST(test_read_only_and_reserved);
	RUN_TEST(test_receive_not_implemented);
	RUN_TEST(test_reset_clears_state);
	RUN_TEST(test_unconnected_output);
	RUN_TEST(test_invalid_config);
	return test_summary();
}
