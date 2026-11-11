/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * ARM PrimeCell UART (PL011). Section numbers refer to the Technical
 * Reference Manual, ARM DDI 0183G.
 *
 * Transmitting (sections 2.4.3, 2.4.5 and 3.3.6): the transmitter takes the
 * oldest byte out of the transmit FIFO and shifts out its frame, a start
 * bit, the data bits, the parity bit if enabled and the stop bits, each
 * bit lasting 16 periods of Baud16, that is 16 * (UARTIBRD + UARTFBRD/64)
 * periods of UARTCLK. The byte reaches the output stream when its last
 * stop bit is done, and the next byte starts at once. The FIFO keeps the
 * 8 bits written; the transmitter sends the WLEN of them that the frame
 * format has when the byte starts. The divisor and
 * the frame format take effect at the next write of UARTLCR_H (3.3.7),
 * and for the next byte. A UART that is disabled finishes the byte it is
 * sending first (3.3.8).
 *
 * The transmit interrupt (2.8.3, 3.3.9) changes on a transition through
 * the trigger level: it sets when a byte leaving the FIFO brings it from
 * above the level to the level or below, and clears when a write brings
 * the FIFO above the level. Without FIFOs, the level is an empty holding
 * register. A reserved trigger level stops the machine when the UART
 * needs it, and so does sending with UARTIBRD 0, or with UARTIBRD 0xffff
 * and a UARTFBRD that is not 0, which the manual calls invalid (3.3.6).
 *
 * Receiving (2.4.3, 3.3.1, 3.3.2): the bytes that the host sends come on
 * the line one after the other, each taking the time of a frame in the
 * UART's own format, as if the other end used the same settings; so
 * they are never in error, and only the low WLEN bits of each are kept.
 * At the end of its frame, a byte goes into the receive FIFO, or, when
 * the FIFO is full, is lost: the overrun error then sets in UARTRSR and
 * in the next byte stored. Bytes that come while the UART or its
 * receiver is disabled are lost, as no one listens to the line; a byte
 * that started before goes on to the FIFO (3.3.8).
 *
 * The receive interrupt (2.8.2) sets when the receive FIFO goes up
 * through its trigger level, or without FIFOs when a byte comes, and
 * clears when reads take it below. The receive timeout interrupt (2.8.4)
 * sets when the FIFO holds data and no byte has come for 32 bit periods,
 * and clears when the FIFO empties. The overrun interrupt sets with the
 * overrun error.
 */

#include "hw/char/pl011.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "util/muldiv.h"

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
	FR_RXFF = 1u << 6,
	FR_TXFE = 1u << 7,
};

/* The overrun error in UARTRSR (3.3.2) and in UARTDR (3.3.1). */
enum {
	RSR_OE = 1u << 3,
	DR_OE = 1u << 11,
};

/* Line control register bits, section 3.3.7. */
enum {
	LCR_H_PEN = 1u << 1,
	LCR_H_STP2 = 1u << 3,
	LCR_H_FEN = 1u << 4,
	LCR_H_WLEN_SHIFT = 5,
};

/* Interrupt bits of UARTIMSC, UARTRIS, UARTMIS and UARTICR, 3.3.10. */
enum {
	INT_RX = 1u << 4,
	INT_TX = 1u << 5,
	INT_RT = 1u << 6,
	INT_OE = 1u << 10,
	INT_ALL = 0x7ff,
};

#define NS_PER_SECOND UINT64_C(1000000000)

/* Control register bits, section 3.3.8. */
enum {
	CR_UARTEN = 1u << 0,
	CR_TXE = 1u << 8,
	CR_RXE = 1u << 9,
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
	Machine *machine;
	Pl011Config config;
	uint64_t clock_hz;
	CharStream *serial;
	IrqLine *irq;

	/* Bytes waiting to be sent, oldest first. */
	uint8_t tx_fifo[PL011_MAX_FIFO_DEPTH];
	unsigned tx_count;

	/* The byte being sent, and the end of its frame. */
	bool sending;
	uint8_t tx_byte;
	Event tx_done;

	/*
	 * Bytes received, oldest first, each with its error bits as UARTDR
	 * gives them.
	 */
	uint16_t rx_fifo[PL011_MAX_FIFO_DEPTH];
	unsigned rx_count;

	/* The byte coming on the line, and the end of its frame. */
	bool receiving;
	uint8_t rx_byte;
	Event rx_done;
	Event rx_timeout; /* 32 bit periods after the last byte came */
	bool overrun;     /* a byte was lost: the next one stored says so */
	uint32_t rsr;     /* UARTRSR */

	/* UARTIBRD and UARTFBRD as the last write of UARTLCR_H took them. */
	uint32_t divisor_int;
	uint32_t divisor_frac;
	uint32_t ris; /* raw interrupt status */

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

static void update_irq(Pl011 *uart)
{
	irq_set(uart->irq, (uart->ris & uart->imsc) != 0);
}

/*
 * The trigger level, in bytes, that the 3-bit select of UARTIFLS gives
 * the FIFO (3.3.9). Returns false, after stopping the machine, for a
 * reserved one.
 */
static bool trigger_level(Pl011 *uart, unsigned select, const char *fifo,
                          unsigned *level)
{
	static const unsigned eighths[] = {1, 2, 4, 6, 7};

	if (select >= 5) {
		machine_stop(uart->machine, MACHINE_STOP_ERROR,
		             "pl011: %s FIFO level %u in UARTIFLS is reserved", fifo,
		             select);
		return false;
	}
	*level = uart->config.fifo_depth * eighths[select] / 8;
	return true;
}

/* The transmit trigger level: at or below it, or an empty holding register. */
static bool tx_level(Pl011 *uart, unsigned *level)
{
	if (!(uart->lcr_h & LCR_H_FEN)) {
		*level = 0;
		return true;
	}
	return trigger_level(uart, uart->ifls & 7, "transmit", level);
}

/* The receive trigger level: at or above it, or a full holding register. */
static bool rx_level(Pl011 *uart, unsigned *level)
{
	if (!(uart->lcr_h & LCR_H_FEN)) {
		*level = 1;
		return true;
	}
	return trigger_level(uart, uart->ifls >> 3 & 7, "receive", level);
}

/* The transmit FIFO went from before bytes to tx_count. */
static void tx_count_changed(Pl011 *uart, unsigned before)
{
	unsigned level;

	if (!tx_level(uart, &level))
		return;
	if (uart->tx_count > level)
		uart->ris &= ~INT_TX;
	else if (before > level)
		uart->ris |= INT_TX;
	update_irq(uart);
}

/*
 * The time that bits periods take on the line at the baud rate, in ns.
 * Returns false, after stopping the machine, for an invalid divisor.
 */
static bool line_time(Pl011 *uart, unsigned bits, uint64_t *ns)
{
	uint64_t divisor = 64 * (uint64_t)uart->divisor_int + uart->divisor_frac;

	if (uart->divisor_int == 0 ||
	    (uart->divisor_int == IBRD_MASK && uart->divisor_frac != 0)) {
		machine_stop(uart->machine, MACHINE_STOP_ERROR,
		             "pl011: the baud rate divisor, UARTIBRD %" PRIu32
		             " and UARTFBRD %" PRIu32
		             ", is invalid: set it, then write UARTLCR_H",
		             uart->divisor_int, uart->divisor_frac);
		return false;
	}
	/* bits * 16 * divisor / 64 periods of UARTCLK */
	*ns = mul_div_ceil(bits * divisor, NS_PER_SECOND, 4 * uart->clock_hz);
	return true;
}

/* The time a frame takes, in ns: see line_time(). */
static bool frame_time(Pl011 *uart, uint64_t *ns)
{
	unsigned bits = 1 + word_length(uart) + (uart->lcr_h & LCR_H_PEN ? 1 : 0) +
	                (uart->lcr_h & LCR_H_STP2 ? 2 : 1);

	return line_time(uart, bits, ns);
}

/*
 * Start sending the oldest byte of the FIFO, if the transmitter is free
 * and the UART and its transmitter are both enabled (UARTEN and TXE,
 * section 3.3.8).
 */
static void tx_start(Pl011 *uart)
{
	uint64_t ns;
	unsigned before = uart->tx_count;

	if (uart->sending || uart->tx_count == 0)
		return;
	if ((uart->cr & (CR_UARTEN | CR_TXE)) != (CR_UARTEN | CR_TXE))
		return;
	if (!frame_time(uart, &ns))
		return;
	/* Only the low WLEN bits of the character are sent. */
	uart->tx_byte = uart->tx_fifo[0] & (uint8_t)((1u << word_length(uart)) - 1);
	uart->tx_count--;
	memmove(uart->tx_fifo, uart->tx_fifo + 1, uart->tx_count);
	uart->sending = true;
	event_schedule(&uart->machine->events, &uart->tx_done,
	               machine_now(uart->machine) + ns);
	tx_count_changed(uart, before);
}

/* The last stop bit of the byte being sent is done. */
static void tx_finished(void *ctx)
{
	Pl011 *uart = ctx;

	uart->sending = false;
	char_stream_write(uart->serial, &uart->tx_byte, 1);
	tx_start(uart);
}

static void write_dr(Pl011 *uart, uint32_t value)
{
	unsigned before = uart->tx_count;

	/*
	 * The manual does not say what happens when the FIFO is full. The
	 * byte is dropped, which is what software that ignores TXFF sees.
	 */
	if (uart->tx_count == tx_capacity(uart))
		return;
	uart->tx_fifo[uart->tx_count++] = (uint8_t)value;
	tx_count_changed(uart, before);
	tx_start(uart);
}

static unsigned rx_capacity(const Pl011 *uart)
{
	return (uart->lcr_h & LCR_H_FEN) ? uart->config.fifo_depth : 1;
}

/* The receive FIFO went from before bytes to rx_count. */
static void rx_count_changed(Pl011 *uart, unsigned before)
{
	unsigned level;

	if (!rx_level(uart, &level))
		return;
	if (uart->rx_count < level)
		uart->ris &= ~INT_RX;
	else if (before < level)
		uart->ris |= INT_RX;
	if (uart->rx_count == 0)
		uart->ris &= ~INT_RT;
	update_irq(uart);
}

/*
 * Take the next byte that the host sent onto the line, if the receiver
 * is free. The bytes that come while the UART or its receiver is
 * disabled are lost. Returns whether a byte started.
 */
static bool rx_start(Pl011 *uart)
{
	uint8_t byte;
	uint64_t ns;

	if (uart->receiving)
		return false;
	if ((uart->cr & (CR_UARTEN | CR_RXE)) != (CR_UARTEN | CR_RXE)) {
		while (char_stream_read(uart->serial, &byte))
			continue;
		return false;
	}
	if (!char_stream_read(uart->serial, &byte) || !frame_time(uart, &ns))
		return false;
	uart->rx_byte = byte;
	uart->receiving = true;
	event_schedule(&uart->machine->events, &uart->rx_done,
	               machine_now(uart->machine) + ns);
	return true;
}

/* The last stop bit of the byte coming on the line is done. */
static void rx_finished(void *ctx)
{
	Pl011 *uart = ctx;
	unsigned before = uart->rx_count;
	uint64_t ns;

	uart->receiving = false;
	if (uart->rx_count == rx_capacity(uart)) {
		uart->rsr |= RSR_OE;
		uart->overrun = true;
		uart->ris |= INT_OE;
		update_irq(uart);
	} else {
		/* Only the low WLEN bits of the character come. */
		uint16_t entry = uart->rx_byte & ((1u << word_length(uart)) - 1);

		if (uart->overrun)
			entry |= DR_OE;
		uart->overrun = false;
		uart->rx_fifo[uart->rx_count++] = entry;
		rx_count_changed(uart, before);
	}
	if (line_time(uart, 32, &ns)) /* the FIFO is not empty */
		event_schedule(&uart->machine->events, &uart->rx_timeout,
		               machine_now(uart->machine) + ns);
	rx_start(uart);
}

/* No byte came for 32 bit periods. */
static void rx_timed_out(void *ctx)
{
	Pl011 *uart = ctx;

	if (uart->rx_count != 0) {
		uart->ris |= INT_RT;
		update_irq(uart);
	}
}

/* A read of UARTDR: the oldest byte received, or 0 when there is none. */
static uint32_t read_dr(Pl011 *uart)
{
	unsigned before = uart->rx_count;
	uint16_t entry;

	if (uart->rx_count == 0)
		return 0;
	entry = uart->rx_fifo[0];
	uart->rx_count--;
	memmove(uart->rx_fifo, uart->rx_fifo + 1,
	        uart->rx_count * sizeof(uart->rx_fifo[0]));
	rx_count_changed(uart, before);
	return entry;
}

/* The machine's input: take what the host sent, or wait for it. */
static void serial_poll(void *ctx)
{
	rx_start(ctx);
}

static bool serial_wait(void *ctx)
{
	Pl011 *uart = ctx;

	if ((uart->cr & (CR_UARTEN | CR_RXE)) != (CR_UARTEN | CR_RXE) ||
	    !char_stream_wait(uart->serial))
		return false;
	rx_start(uart);
	return true;
}

static uint32_t read_fr(const Pl011 *uart)
{
	uint32_t fr = 0;

	if (uart->rx_count == 0)
		fr |= FR_RXFE;
	if (uart->rx_count == rx_capacity(uart))
		fr |= FR_RXFF;
	if (uart->tx_count == 0)
		fr |= FR_TXFE;
	/* BUSY: sending, or data waiting, enabled or not */
	if (uart->sending || uart->tx_count != 0)
		fr |= FR_BUSY;
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
		return read_dr(uart);
	case REG_RSR_ECR:
		return uart->rsr;
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
		return uart->ris;
	case REG_MIS:
		return uart->ris & uart->imsc;
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
		uart->rsr = 0;
		break;
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
		uart->divisor_int = uart->ibrd;
		uart->divisor_frac = uart->fbrd;
		tx_start(uart);
		break;
	case REG_CR:
		uart->cr = v & CR_MASK;
		tx_start(uart);
		break;
	case REG_IFLS:
		uart->ifls = v & IFLS_MASK;
		break;
	case REG_IMSC:
		uart->imsc = v & IMSC_MASK;
		update_irq(uart);
		break;
	case REG_ICR:
		uart->ris &= ~(v & INT_ALL);
		update_irq(uart);
		break;
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
	event_cancel(&uart->machine->events, &uart->tx_done);
	event_cancel(&uart->machine->events, &uart->rx_done);
	event_cancel(&uart->machine->events, &uart->rx_timeout);
	uart->sending = false;
	uart->tx_count = 0;
	uart->receiving = false;
	uart->rx_count = 0;
	uart->overrun = false;
	uart->rsr = 0;
	uart->divisor_int = 0;
	uart->divisor_frac = 0;
	uart->ris = 0;
	uart->ilpr = 0;
	uart->ibrd = 0;
	uart->fbrd = 0;
	uart->lcr_h = 0;
	uart->cr = 0x0300; /* TXE and RXE set, UART disabled */
	uart->ifls = 0x12; /* both FIFO levels at 1/2 */
	uart->imsc = 0;
	uart->dmacr = 0;
	update_irq(uart);
}

Pl011 *pl011_create(Machine *m, const Pl011Config *config, uint64_t clock_hz,
                    CharStream *serial, IrqLine *irq)
{
	if (config->fifo_depth == 0 || config->fifo_depth > PL011_MAX_FIFO_DEPTH ||
	    clock_hz == 0)
		return NULL;

	Pl011 *uart = calloc(1, sizeof(*uart));

	if (uart == NULL)
		return NULL;
	uart->machine = m;
	uart->config = *config;
	uart->clock_hz = clock_hz;
	uart->serial = serial;
	uart->irq = irq;
	event_init(&uart->tx_done, tx_finished, uart);
	event_init(&uart->rx_done, rx_finished, uart);
	event_init(&uart->rx_timeout, rx_timed_out, uart);
	if (char_stream_has_input(serial))
		machine_set_input(m, serial_poll, serial_wait, uart);
	pl011_reset(uart);
	return uart;
}

void pl011_destroy(Pl011 *uart)
{
	Machine *m = uart->machine;

	if (m->input.ctx == uart)
		machine_set_input(m, NULL, NULL, NULL);
	event_cancel(&m->events, &uart->rx_done);
	event_cancel(&m->events, &uart->rx_timeout);
	event_cancel(&uart->machine->events, &uart->tx_done);
	free(uart);
}

BusStatus pl011_map(Pl011 *uart, Bus *bus, const char *name, uint64_t base)
{
	return bus_map_mmio(bus, name, base, PL011_MMIO_SIZE, &pl011_mmio_ops,
	                    uart);
}
