/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835 auxiliary peripherals; see bcm2835_aux.h. Section numbers
 * refer to chapter 2 of "BCM2835 ARM Peripherals".
 *
 * Transmitting: a byte written to AUX_MU_IO_REG goes into the transmit
 * FIFO, and the transmitter shifts out its frame: a start bit, 7 or 8
 * data bits and a stop bit (2.2), each bit lasting 8 * (AUX_MU_BAUD_REG
 * + 1) cycles of the system clock (2.2.1). The byte reaches the host at
 * the end of its frame, and the next one starts at once.
 *
 * Receiving: the bytes that the host sends come on the line one after
 * the other, each taking the time of a frame in the mini UART's format.
 * At the end of its frame, a byte goes into the receive FIFO, or, when
 * the FIFO is full, is lost and sets the overrun bits. Bytes that come
 * while the mini UART or its receiver is disabled are lost, as no one
 * listens to the line. A line held low, as when no GPIO pin carries RXD1,
 * looks like a start bit that never ends: the receiver gets 0x00 after
 * 0x00 (2.1.1).
 *
 * The interrupt (2.2.2) is high while the receive interrupt is enabled
 * and the receive FIFO holds a byte, or the transmit interrupt is enabled
 * and the transmit FIFO is empty. AUX_IRQ shows it, and it is the
 * block's interrupt output.
 *
 * What the documents leave open, or get wrong, this model chooses:
 *  - In AUX_MU_IER_REG, bit 0 enables the receive interrupt and bit 1 the
 *    transmit interrupt, as in a 16550 and as Linux's 8250_bcm2835aux
 *    driver uses them on the real chip; both documents swap them.
 *  - AUX_MU_STAT_REG bit 9 is the AND of bits 3 and 8, as its text says,
 *    not of bits 2 and 8 as its last sentence says.
 *  - The CTS input is asserted: AUX_MU_MSR_REG bit 5 reads 1, its reset
 *    value, and AUX_MU_STAT_REG bit 7 reads 0. AUX_MU_STAT_REG bit 6
 *    reads 1 while RTS is asserted: with receive flow control, while the
 *    receive FIFO has more free entries than AUX_MU_CNTL_REG bits 5:4
 *    ask; else as AUX_MU_MCR_REG bit 1 sets it.
 *  - The receiver is busy, AUX_MU_STAT_REG bit 2 clear, while its line is
 *    held low.
 *  - A write to a full transmit FIFO is lost.
 *  - A transmitter or receiver that is disabled finishes the byte it is
 *    sending or receiving, as AUX_MU_CNTL_REG says; so does a mini UART
 *    that AUX_ENABLES disables.
 *  - The document says that the registers of a disabled module cannot be
 *    read or written, but not what happens: such an access stops the
 *    machine. So do accesses other than 32-bit ones, writes of the
 *    read-only registers, and accesses to offsets the document does not
 *    describe.
 */

#include "hw/char/bcm2835_aux.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "util/muldiv.h"

#define NS_PER_SECOND UINT64_C(1000000000)
#define FIFO_DEPTH 8

/* Register offsets, section 2.1. */
enum {
	REG_IRQ = 0x00,
	REG_ENABLES = 0x04,
	REG_IO = 0x40,
	REG_IER = 0x44,
	REG_IIR = 0x48,
	REG_LCR = 0x4c,
	REG_MCR = 0x50,
	REG_LSR = 0x54,
	REG_MSR = 0x58,
	REG_SCRATCH = 0x5c,
	REG_CNTL = 0x60,
	REG_STAT = 0x64,
	REG_BAUD = 0x68,
	REG_SPI1 = 0x80, /* the two SPI masters, to 0xd7 */
	REG_SPI_END = 0xd8,
};

enum {
	ENABLE_UART = 1 << 0,
	IER_RX = 1 << 0,
	IER_TX = 1 << 1,
	IIR_CLEAR_RX = 1 << 1,
	IIR_CLEAR_TX = 1 << 2,
	LCR_8BIT = 1 << 0,
	LCR_BREAK = 1 << 6,
	LCR_DLAB = 1 << 7,
	MCR_RTS = 1 << 1,
	LSR_DATA_READY = 1 << 0,
	LSR_OVERRUN = 1 << 1,
	LSR_TX_EMPTY = 1 << 5,
	LSR_TX_IDLE = 1 << 6,
	MSR_CTS = 1 << 5,
	CNTL_RX = 1 << 0,
	CNTL_TX = 1 << 1,
	CNTL_RX_FLOW = 1 << 2,
	CNTL_TX_FLOW = 1 << 3,
};

struct Bcm2835Aux {
	Machine *machine;
	uint64_t clock_hz;
	CharStream *serial;
	IrqLine *irq;

	uint32_t enables;

	/* Bytes waiting to be sent, oldest first, and the one being sent. */
	uint8_t tx_fifo[FIFO_DEPTH];
	unsigned tx_count;
	bool sending;
	uint8_t tx_byte;
	Event tx_done;

	/* Bytes received, oldest first, and the one coming on the line. */
	uint8_t rx_fifo[FIFO_DEPTH];
	unsigned rx_count;
	bool receiving;
	uint8_t rx_byte;
	Event rx_done;
	bool overrun; /* a byte was lost since the last read of LSR */

	uint32_t ier;
	uint32_t lcr;
	uint32_t mcr;
	uint32_t scratch;
	uint32_t cntl;
	uint32_t baud;
};

static bool uart_enabled(const Bcm2835Aux *aux)
{
	return aux->enables & ENABLE_UART;
}

static uint8_t data_mask(const Bcm2835Aux *aux)
{
	return aux->lcr & LCR_8BIT ? 0xff : 0x7f;
}

/* The time a frame takes, in ns: section 2.2.1. */
static uint64_t frame_time(const Bcm2835Aux *aux)
{
	unsigned bits = 1 + (aux->lcr & LCR_8BIT ? 8 : 7) + 1;

	return mul_div_ceil((uint64_t)bits * 8 * (aux->baud + 1), NS_PER_SECOND,
	                    aux->clock_hz);
}

static bool interrupt(const Bcm2835Aux *aux)
{
	return uart_enabled(aux) && (((aux->ier & IER_RX) && aux->rx_count != 0) ||
	                             ((aux->ier & IER_TX) && aux->tx_count == 0));
}

static void update_irq(Bcm2835Aux *aux)
{
	irq_set(aux->irq, interrupt(aux));
}

/* Start sending the oldest byte, if the transmitter is free and enabled. */
static void tx_start(Bcm2835Aux *aux)
{
	if (aux->sending || aux->tx_count == 0 || !uart_enabled(aux) ||
	    !(aux->cntl & CNTL_TX))
		return;
	aux->tx_byte = aux->tx_fifo[0] & data_mask(aux);
	aux->tx_count--;
	memmove(aux->tx_fifo, aux->tx_fifo + 1, aux->tx_count);
	aux->sending = true;
	event_schedule(&aux->machine->events, &aux->tx_done,
	               machine_now(aux->machine) + frame_time(aux));
	update_irq(aux);
}

/* The stop bit of the byte being sent is done. */
static void tx_finished(void *ctx)
{
	Bcm2835Aux *aux = ctx;

	aux->sending = false;
	char_stream_write(aux->serial, &aux->tx_byte, 1);
	tx_start(aux);
}

/*
 * Take the next byte that the host sent onto the line, if the receiver
 * is free. Returns whether a byte started.
 */
static bool rx_start(Bcm2835Aux *aux)
{
	uint8_t byte;

	if (aux->receiving)
		return false;
	if (!uart_enabled(aux) || !(aux->cntl & CNTL_RX)) {
		while (char_stream_read(aux->serial, &byte))
			continue;
		return false;
	}
	/*
	 * A line held low brings 0x00 after 0x00 (2.1.1). Once the FIFO is
	 * full and the overrun known, more of them change nothing that can be
	 * seen: they wait until a read makes room or clears the overrun.
	 */
	if (char_stream_line_low(aux->serial)) {
		if (aux->rx_count == FIFO_DEPTH && aux->overrun)
			return false;
		byte = 0;
	} else if (!char_stream_read(aux->serial, &byte)) {
		return false;
	}
	aux->rx_byte = byte;
	aux->receiving = true;
	event_schedule(&aux->machine->events, &aux->rx_done,
	               machine_now(aux->machine) + frame_time(aux));
	return true;
}

/* The stop bit of the byte coming on the line is done. */
static void rx_finished(void *ctx)
{
	Bcm2835Aux *aux = ctx;

	aux->receiving = false;
	if (aux->rx_count == FIFO_DEPTH) {
		aux->overrun = true;
	} else {
		aux->rx_fifo[aux->rx_count++] = aux->rx_byte & data_mask(aux);
		update_irq(aux);
	}
	rx_start(aux);
}

/* The machine's input: take what the host sent, or wait for it. */
static void serial_poll(void *ctx)
{
	rx_start(ctx);
}

static bool serial_wait(void *ctx)
{
	Bcm2835Aux *aux = ctx;

	if (!uart_enabled(aux) || !(aux->cntl & CNTL_RX))
		return false;
	if (char_stream_line_low(aux->serial))
		return rx_start(aux);
	if (!char_stream_wait(aux->serial))
		return false;
	rx_start(aux);
	return true;
}

static void write_io(Bcm2835Aux *aux, uint32_t value)
{
	if (aux->tx_count == FIFO_DEPTH)
		return;
	aux->tx_fifo[aux->tx_count++] = (uint8_t)value;
	tx_start(aux);
}

/* A read of the data: the oldest byte received, or 0 when there is none. */
static uint32_t read_io(Bcm2835Aux *aux)
{
	uint8_t byte;

	if (aux->rx_count == 0)
		return 0;
	byte = aux->rx_fifo[0];
	aux->rx_count--;
	memmove(aux->rx_fifo, aux->rx_fifo + 1, aux->rx_count);
	rx_start(aux);
	update_irq(aux);
	return byte;
}

/* AUX_MU_IIR_REG: the receive interrupt first, as in a 16550. */
static uint32_t read_iir(const Bcm2835Aux *aux)
{
	unsigned id = 0; /* no interrupt */

	if ((aux->ier & IER_RX) && aux->rx_count != 0)
		id = 2; /* the receiver holds a byte */
	else if ((aux->ier & IER_TX) && aux->tx_count == 0)
		id = 1; /* the transmit FIFO is empty */
	return 0xc0 | id << 1 | (id == 0 ? 1 : 0);
}

static uint32_t read_lsr(Bcm2835Aux *aux)
{
	uint32_t lsr = 0;

	if (aux->rx_count != 0)
		lsr |= LSR_DATA_READY;
	if (aux->overrun)
		lsr |= LSR_OVERRUN;
	if (aux->tx_count < FIFO_DEPTH)
		lsr |= LSR_TX_EMPTY;
	if (aux->tx_count == 0 && !aux->sending)
		lsr |= LSR_TX_IDLE;
	aux->overrun = false;
	rx_start(aux);
	return lsr;
}

static bool rts_asserted(const Bcm2835Aux *aux)
{
	static const unsigned levels[4] = {3, 2, 1, 4}; /* free entries */

	if (aux->cntl & CNTL_RX_FLOW)
		return FIFO_DEPTH - aux->rx_count > levels[(aux->cntl >> 4) & 3];
	return aux->mcr & MCR_RTS;
}

static uint32_t read_stat(Bcm2835Aux *aux)
{
	bool tx_idle = !aux->sending;
	bool tx_empty = aux->tx_count == 0;

	return (uint32_t)aux->tx_count << 24 | (uint32_t)aux->rx_count << 16 |
	       (uint32_t)(tx_idle && tx_empty) << 9 | (uint32_t)tx_empty << 8 |
	       (uint32_t)rts_asserted(aux) << 6 |
	       (uint32_t)(aux->tx_count == FIFO_DEPTH) << 5 |
	       (uint32_t)aux->overrun << 4 | (uint32_t)tx_idle << 3 |
	       (uint32_t)(!aux->receiving && !char_stream_line_low(aux->serial))
	           << 2 |
	       (uint32_t)(aux->tx_count < FIFO_DEPTH) << 1 |
	       (uint32_t)(aux->rx_count != 0);
}

static void stop(Bcm2835Aux *aux, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(aux->machine, MACHINE_STOP_ERROR,
	             "bcm2835-aux: %u-byte %s offset 0x%02" PRIx64 " %s", size,
	             access, offset, why);
}

/*
 * Whether an access can go on: it stops the machine when it is not a
 * 32-bit access to a mini UART register of an enabled mini UART, or to
 * AUX_IRQ or AUX_ENABLES.
 */
static bool accessible(Bcm2835Aux *aux, const char *access, uint64_t offset,
                       unsigned size)
{
	if (size != 4 || (offset & 3) != 0 ||
	    (offset > REG_ENABLES && offset < REG_IO) ||
	    (offset > REG_BAUD && offset < REG_SPI1) || offset >= REG_SPI_END) {
		stop(aux, access, offset, size, "is not described by the datasheet");
		return false;
	}
	if (offset >= REG_SPI1) {
		stop(aux, access, offset, size,
		     "is not implemented yet (the SPI masters)");
		return false;
	}
	if (offset >= REG_IO && !uart_enabled(aux)) {
		stop(aux, access, offset, size,
		     "is not possible: AUX_ENABLES disables the mini UART");
		return false;
	}
	return true;
}

static uint64_t aux_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Aux *aux = ctx;
	bool dlab = aux->lcr & LCR_DLAB;

	if (!accessible(aux, "read from", offset, size))
		return 0;
	switch (offset) {
	case REG_IRQ:
		return interrupt(aux) ? 1 : 0;
	case REG_ENABLES:
		return aux->enables;
	case REG_IO:
		return dlab ? aux->baud & 0xff : read_io(aux);
	case REG_IER:
		return dlab ? aux->baud >> 8 : aux->ier;
	case REG_IIR:
		return read_iir(aux);
	case REG_LCR:
		return aux->lcr;
	case REG_MCR:
		return aux->mcr;
	case REG_LSR:
		return read_lsr(aux);
	case REG_MSR:
		return MSR_CTS;
	case REG_SCRATCH:
		return aux->scratch;
	case REG_CNTL:
		return aux->cntl;
	case REG_STAT:
		return read_stat(aux);
	default: /* REG_BAUD */
		return aux->baud;
	}
}

static void write_cntl(Bcm2835Aux *aux, uint64_t offset, uint32_t value)
{
	if (value & CNTL_TX_FLOW) {
		stop(aux, "write to", offset, 4,
		     "is not implemented yet (transmit flow control with CTS)");
		return;
	}
	aux->cntl = value & 0xff;
	tx_start(aux);
	rx_start(aux);
}

static void aux_write(void *ctx, uint64_t offset, uint64_t value, unsigned size)
{
	Bcm2835Aux *aux = ctx;
	uint32_t v = (uint32_t)value;
	bool dlab = aux->lcr & LCR_DLAB;

	if (!accessible(aux, "write to", offset, size))
		return;
	switch (offset) {
	case REG_ENABLES:
		aux->enables = v & 7;
		tx_start(aux);
		rx_start(aux);
		break;
	case REG_IO:
		if (dlab)
			aux->baud = (aux->baud & 0xff00) | (v & 0xff);
		else
			write_io(aux, v);
		break;
	case REG_IER:
		if (dlab)
			aux->baud = (aux->baud & 0xff) | (v & 0xff) << 8;
		else
			aux->ier = v & (IER_RX | IER_TX);
		break;
	case REG_IIR:
		if (v & IIR_CLEAR_RX) {
			aux->rx_count = 0;
			rx_start(aux);
		}
		if (v & IIR_CLEAR_TX)
			aux->tx_count = 0;
		break;
	case REG_LCR:
		aux->lcr = v & (LCR_DLAB | LCR_BREAK | LCR_8BIT);
		break;
	case REG_MCR:
		aux->mcr = v & MCR_RTS;
		break;
	case REG_SCRATCH:
		aux->scratch = v & 0xff;
		break;
	case REG_CNTL:
		write_cntl(aux, offset, v);
		break;
	case REG_BAUD:
		aux->baud = v & 0xffff;
		break;
	default: /* AUX_IRQ, LSR, MSR and STAT are read-only */
		stop(aux, "write to", offset, size, "is not possible: it is read-only");
		return;
	}
	update_irq(aux);
}

static const BusMmioOps aux_mmio_ops = {
	.read = aux_read,
	.write = aux_write,
};

void bcm2835_aux_reset(Bcm2835Aux *aux)
{
	/* Reset values from section 2.2.2. */
	event_cancel(&aux->machine->events, &aux->tx_done);
	event_cancel(&aux->machine->events, &aux->rx_done);
	aux->enables = 0;
	aux->tx_count = 0;
	aux->sending = false;
	aux->rx_count = 0;
	aux->receiving = false;
	aux->overrun = false;
	aux->ier = 0;
	aux->lcr = 0;
	aux->mcr = 0;
	aux->scratch = 0;
	aux->cntl = CNTL_RX | CNTL_TX;
	aux->baud = 0;
	update_irq(aux);
}

Bcm2835Aux *bcm2835_aux_create(Machine *m, uint64_t clock_hz,
                               CharStream *serial, IrqLine *irq)
{
	if (clock_hz == 0)
		return NULL;

	Bcm2835Aux *aux = calloc(1, sizeof(*aux));

	if (aux == NULL)
		return NULL;
	aux->machine = m;
	aux->clock_hz = clock_hz;
	aux->serial = serial;
	aux->irq = irq;
	event_init(&aux->tx_done, tx_finished, aux);
	event_init(&aux->rx_done, rx_finished, aux);
	if (char_stream_has_input(serial) &&
	    !machine_add_input(m, serial_poll, serial_wait, aux)) {
		free(aux);
		return NULL;
	}
	bcm2835_aux_reset(aux);
	return aux;
}

void bcm2835_aux_destroy(Bcm2835Aux *aux)
{
	Machine *m = aux->machine;

	machine_remove_input(m, aux);
	event_cancel(&m->events, &aux->tx_done);
	event_cancel(&m->events, &aux->rx_done);
	free(aux);
}

BusStatus bcm2835_aux_map(Bcm2835Aux *aux, Bus *bus, const char *name,
                          uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_AUX_MMIO_SIZE, &aux_mmio_ops,
	                    aux);
}
