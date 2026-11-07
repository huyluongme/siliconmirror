/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * ARM PrimeCell UART (PL011). Section numbers refer to the Technical
 * Reference Manual, ARM DDI 0183G.
 */

#include "hw/char/pl011.h"

#include <stdbool.h>
#include <stdlib.h>

/* Register offsets, table 3-1. */
enum {
	REG_DR = 0x000,
	REG_RSR_ECR = 0x004,
	REG_FR = 0x018,
	REG_ILPR = 0x020,
	REG_IBRD = 0x024,
	REG_FBRD = 0x028,
	REG_LCR_H = 0x02c,
	REG_CR = 0x030,
	REG_IFLS = 0x034,
	REG_IMSC = 0x038,
	REG_RIS = 0x03c,
	REG_MIS = 0x040,
	REG_ICR = 0x044,
	REG_DMACR = 0x048,
	REG_PERIPH_ID0 = 0xfe0,
	REG_PCELL_ID0 = 0xff0,
};

/* Flag register bits, section 3.3.3. */
enum {
	FR_BUSY = 1u << 3,
	FR_RXFE = 1u << 4,
	FR_TXFF = 1u << 5,
	FR_TXFE = 1u << 7,
};

/* Line control register bits, section 3.3.7. */
enum {
	LCR_H_FEN = 1u << 4,
	LCR_H_WLEN_SHIFT = 5,
};

/* Control register bits, section 3.3.8. */
enum {
	CR_UARTEN = 1u << 0,
	CR_TXE = 1u << 8,
};

/* Writable bits of each register, from its width in table 3-1. */
enum {
	ILPR_MASK = 0xff,
	IBRD_MASK = 0xffff,
	FBRD_MASK = 0x3f,
	LCR_H_MASK = 0xff,
	CR_MASK = 0xff87, /* bits 6:3 are reserved */
	IFLS_MASK = 0x3f,
	IMSC_MASK = 0x7ff,
	DMACR_MASK = 0x7,
};

/* UARTPCellID0-3, section 3.3.16. */
#define PCELL_ID 0xb105f00du

const Pl011Config pl011_arm_r1p5 = {
	.fifo_depth = 32,
	.periph_id = 0x00341011,
};

struct Pl011 {
	Pl011Config config;
	CharStream *out;
	IrqLine *irq;

	/* Bytes waiting to be sent, oldest first. */
	uint8_t tx_fifo[PL011_MAX_FIFO_DEPTH];
	unsigned tx_count;

	uint32_t ilpr;
	uint32_t ibrd;
	uint32_t fbrd;
	uint32_t lcr_h;
	uint32_t cr;
	uint32_t ifls;
	uint32_t imsc;
	uint32_t dmacr;
};

/* Without FEN, each FIFO is a one-byte holding register (section 3.3.7). */
static unsigned tx_capacity(const Pl011 *uart)
{
	return (uart->lcr_h & LCR_H_FEN) ? uart->config.fifo_depth : 1;
}

/* Number of data bits per character, 5 to 8 (WLEN, section 3.3.7). */
static unsigned word_length(const Pl011 *uart)
{
	return 5 + ((uart->lcr_h >> LCR_H_WLEN_SHIFT) & 3);
}

/*
 * Send everything waiting in the transmit FIFO, if the UART and its
 * transmitter are both enabled (UARTEN and TXE, section 3.3.8).
 */
static void flush_tx(Pl011 *uart)
{
	if ((uart->cr & (CR_UARTEN | CR_TXE)) != (CR_UARTEN | CR_TXE))
		return;
	if (uart->tx_count == 0)
		return;

	char_stream_write(uart->out, uart->tx_fifo, uart->tx_count);
	uart->tx_count = 0;
}

static void write_dr(Pl011 *uart, uint32_t value)
{
	/*
	 * The manual does not say what happens when the FIFO is full. The
	 * byte is dropped, which is what software that ignores TXFF sees.
	 */
	if (uart->tx_count == tx_capacity(uart))
		return;

	/* Only the low WLEN bits of the character are sent. */
	uint8_t mask = (uint8_t)((1u << word_length(uart)) - 1);

	uart->tx_fifo[uart->tx_count++] = (uint8_t)value & mask;
	flush_tx(uart);
}

static uint32_t read_fr(const Pl011 *uart)
{
	/* Nothing is ever received yet, so the receive FIFO stays empty. */
	uint32_t fr = FR_RXFE;

	if (uart->tx_count == 0)
		fr |= FR_TXFE;
	else
		fr |= FR_BUSY; /* set while data waits, enabled or not */
	if (uart->tx_count == tx_capacity(uart))
		fr |= FR_TXFF;
	return fr;
}

static uint32_t read_id(const Pl011 *uart, uint64_t offset)
{
	if (uart->config.periph_id == 0)
		return 0;

	unsigned byte = (unsigned)(offset - REG_PERIPH_ID0) / 4;
	uint32_t id = byte < 4 ? uart->config.periph_id : PCELL_ID;

	return (id >> (8 * (byte % 4))) & 0xff;
}

static uint64_t pl011_read(void *ctx, uint64_t offset, unsigned size)
{
	Pl011 *uart = ctx;

	(void)size;
	switch (offset) {
	case REG_DR:
		return 0; /* nothing received yet */
	case REG_RSR_ECR:
		return 0; /* no receive errors */
	case REG_FR:
		return read_fr(uart);
	case REG_ILPR:
		return uart->ilpr;
	case REG_IBRD:
		return uart->ibrd;
	case REG_FBRD:
		return uart->fbrd;
	case REG_LCR_H:
		return uart->lcr_h;
	case REG_CR:
		return uart->cr;
	case REG_IFLS:
		return uart->ifls;
	case REG_IMSC:
		return uart->imsc;
	case REG_RIS:
	case REG_MIS:
		return 0; /* no interrupt sources yet */
	case REG_DMACR:
		return uart->dmacr;
	}
	if (offset >= REG_PERIPH_ID0 && offset < PL011_MMIO_SIZE && offset % 4 == 0)
		return read_id(uart, offset);

	/* Reserved and test registers read as 0. */
	return 0;
}

static void pl011_write(void *ctx, uint64_t offset, uint64_t value,
                        unsigned size)
{
	Pl011 *uart = ctx;
	uint32_t v = (uint32_t)value;

	(void)size;
	switch (offset) {
	case REG_DR:
		write_dr(uart, v);
		break;
	case REG_RSR_ECR:
		break; /* clears receive errors, of which there are none yet */
	case REG_ILPR:
		uart->ilpr = v & ILPR_MASK;
		break;
	case REG_IBRD:
		uart->ibrd = v & IBRD_MASK;
		break;
	case REG_FBRD:
		uart->fbrd = v & FBRD_MASK;
		break;
	case REG_LCR_H:
		uart->lcr_h = v & LCR_H_MASK;
		break;
	case REG_CR:
		uart->cr = v & CR_MASK;
		flush_tx(uart);
		break;
	case REG_IFLS:
		uart->ifls = v & IFLS_MASK;
		break;
	case REG_IMSC:
		uart->imsc = v & IMSC_MASK;
		break;
	case REG_ICR:
		break; /* no interrupt sources yet */
	case REG_DMACR:
		uart->dmacr = v & DMACR_MASK;
		break;
	default:
		break; /* read-only, reserved and test registers */
	}
}

static const BusMmioOps pl011_mmio_ops = {
	.read = pl011_read,
	.write = pl011_write,
};

void pl011_reset(Pl011 *uart)
{
	/* Reset values from table 3-1. */
	uart->tx_count = 0;
	uart->ilpr = 0;
	uart->ibrd = 0;
	uart->fbrd = 0;
	uart->lcr_h = 0;
	uart->cr = 0x0300; /* TXE and RXE set, UART disabled */
	uart->ifls = 0x12; /* both FIFO levels at 1/2 */
	uart->imsc = 0;
	uart->dmacr = 0;
}

Pl011 *pl011_create(const Pl011Config *config, CharStream *out, IrqLine *irq)
{
	if (config->fifo_depth == 0 || config->fifo_depth > PL011_MAX_FIFO_DEPTH)
		return NULL;

	Pl011 *uart = calloc(1, sizeof(*uart));

	if (uart == NULL)
		return NULL;
	uart->config = *config;
	uart->out = out;
	uart->irq = irq;
	pl011_reset(uart);
	return uart;
}

void pl011_destroy(Pl011 *uart)
{
	free(uart);
}

BusStatus pl011_map(Pl011 *uart, Bus *bus, const char *name, uint64_t base)
{
	return bus_map_mmio(bus, name, base, PL011_MMIO_SIZE, &pl011_mmio_ops,
	                    uart);
}
