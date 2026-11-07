/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the PL011 UART (src/hw/char/pl011.c). Expected values
 * come from the PL011 Technical Reference Manual, ARM DDI 0183G.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/char_stream.h"
#include "hw/char/pl011.h"
#include "test.h"
#include "util/macros.h"

#define BASE 0x3f201000

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
	LCR_H_FEN = 0x10,
	LCR_H_WLEN_8 = 0x60,
	CR_UARTEN = 0x001,
	CR_TXE = 0x100,
	CR_RXE = 0x200,
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

/* A bus with one UART at BASE that writes into a Capture. */
typedef struct Setup {
	Bus *bus;
	Pl011 *uart;
	Capture out;
} Setup;

static void setup(Setup *s, const Pl011Config *config)
{
	memset(s, 0, sizeof(*s));
	s->out.stream.ops = &capture_ops;
	s->bus = bus_create();
	s->uart = pl011_create(config, &s->out.stream, NULL);
	CHECK(s->uart != NULL);
	CHECK_EQ(pl011_map(s->uart, s->bus, "uart", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	pl011_destroy(s->uart);
	bus_destroy(s->bus);
}

static uint64_t reg_read(Setup *s, uint64_t offset)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->bus, BASE + offset, 4, &value), BUS_OK);
	return value;
}

static void reg_write(Setup *s, uint64_t offset, uint64_t value)
{
	CHECK_EQ(bus_write(s->bus, BASE + offset, 4, value), BUS_OK);
}

static void send(Setup *s, const char *text)
{
	for (; *text != '\0'; text++)
		reg_write(s, DR, (uint8_t)*text);
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
	reg_write(&s, LCR_H, LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE | CR_RXE);
	send(&s, "Hello");
	CHECK(strcmp(s.out.data, "Hello") == 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);
	teardown(&s);
}

static void test_byte_write_to_dr(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	reg_write(&s, LCR_H, LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	CHECK_EQ(bus_write(s.bus, BASE + DR, 1, 'x'), BUS_OK);
	CHECK(strcmp(s.out.data, "x") == 0);
	teardown(&s);
}

static void test_disabled_uart_holds_data(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	reg_write(&s, LCR_H, LCR_H_WLEN_8);

	/* After reset the UART is disabled: the byte waits. */
	send(&s, "A");
	CHECK_EQ(s.out.len, 0);

	/* Without FEN the FIFO is one byte deep, so it is now full. */
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_TXFF | FR_RXFE);
	send(&s, "B");

	/* Enabling the UART sends the waiting byte; B was dropped. */
	reg_write(&s, CR, CR_UARTEN | CR_TXE | CR_RXE);
	CHECK(strcmp(s.out.data, "A") == 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);
	teardown(&s);
}

static void test_transmitter_disabled_holds_data(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	reg_write(&s, LCR_H, LCR_H_WLEN_8);
	reg_write(&s, CR, CR_UARTEN | CR_RXE);
	send(&s, "A");
	CHECK_EQ(s.out.len, 0);
	reg_write(&s, CR, CR_UARTEN | CR_TXE | CR_RXE);
	CHECK(strcmp(s.out.data, "A") == 0);
	teardown(&s);
}

static void test_fifo_depth(void)
{
	static const Pl011Config bcm2835_like = {.fifo_depth = 16};
	Setup s;

	setup(&s, &bcm2835_like);
	reg_write(&s, LCR_H, LCR_H_FEN | LCR_H_WLEN_8);

	/* 15 bytes: not full yet. */
	send(&s, "abcdefghijklmno");
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_RXFE);

	/* The 16th fills the FIFO; the 17th is dropped. */
	send(&s, "pq");
	CHECK_EQ(reg_read(&s, FR), FR_BUSY | FR_TXFF | FR_RXFE);

	reg_write(&s, CR, CR_UARTEN | CR_TXE | CR_RXE);
	CHECK(strcmp(s.out.data, "abcdefghijklmnop") == 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);
	teardown(&s);
}

static void test_word_length(void)
{
	Setup s;

	setup(&s, &pl011_arm_r1p5);
	reg_write(&s, CR, CR_UARTEN | CR_TXE);

	/* WLEN = 0b00: 5 data bits, 0b11: 8 data bits. */
	reg_write(&s, LCR_H, 0x00);
	reg_write(&s, DR, 0xff);
	reg_write(&s, LCR_H, 0x20);
	reg_write(&s, DR, 0xff);
	reg_write(&s, LCR_H, 0x40);
	reg_write(&s, DR, 0xff);
	reg_write(&s, LCR_H, LCR_H_WLEN_8);
	reg_write(&s, DR, 0xff);

	CHECK_EQ(s.out.len, 4);
	CHECK_EQ((uint8_t)s.out.data[0], 0x1f);
	CHECK_EQ((uint8_t)s.out.data[1], 0x3f);
	CHECK_EQ((uint8_t)s.out.data[2], 0x7f);
	CHECK_EQ((uint8_t)s.out.data[3], 0xff);
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

static void test_receive_and_interrupts_not_implemented(void)
{
	Setup s;

	/* Until receiving and interrupts exist, these registers do nothing. */
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
	reg_write(&s, IBRD, 26);
	reg_write(&s, LCR_H, LCR_H_FEN | LCR_H_WLEN_8);
	send(&s, "held");
	pl011_reset(s.uart);

	CHECK_EQ(reg_read(&s, IBRD), 0);
	CHECK_EQ(reg_read(&s, LCR_H), 0);
	CHECK_EQ(reg_read(&s, FR), FR_TXFE | FR_RXFE);

	/* The bytes that were waiting are gone. */
	reg_write(&s, CR, CR_UARTEN | CR_TXE);
	CHECK_EQ(s.out.len, 0);
	teardown(&s);
}

static void test_unconnected_output(void)
{
	Bus *bus = bus_create();
	Pl011 *uart = pl011_create(&pl011_arm_r1p5, NULL, NULL);

	CHECK_EQ(pl011_map(uart, bus, "uart", BASE), BUS_OK);
	CHECK_EQ(bus_write(bus, BASE + CR, 4, CR_UARTEN | CR_TXE), BUS_OK);
	CHECK_EQ(bus_write(bus, BASE + DR, 4, 'x'), BUS_OK);
	pl011_destroy(uart);
	bus_destroy(bus);
}

static void test_invalid_config(void)
{
	static const Pl011Config empty = {.fifo_depth = 0};
	static const Pl011Config too_deep = {.fifo_depth = 64};

	CHECK(pl011_create(&empty, NULL, NULL) == NULL);
	CHECK(pl011_create(&too_deep, NULL, NULL) == NULL);
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_id_registers_r1p5);
	RUN_TEST(test_id_registers_not_implemented);
	RUN_TEST(test_transmit_when_enabled);
	RUN_TEST(test_byte_write_to_dr);
	RUN_TEST(test_disabled_uart_holds_data);
	RUN_TEST(test_transmitter_disabled_holds_data);
	RUN_TEST(test_fifo_depth);
	RUN_TEST(test_word_length);
	RUN_TEST(test_register_widths);
	RUN_TEST(test_read_only_and_reserved);
	RUN_TEST(test_receive_and_interrupts_not_implemented);
	RUN_TEST(test_reset_clears_state);
	RUN_TEST(test_unconnected_output);
	RUN_TEST(test_invalid_config);
	return test_summary();
}
