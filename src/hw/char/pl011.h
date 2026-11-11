/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * ARM PrimeCell UART (PL011).
 *
 * Written from the PrimeCell UART (PL011) Technical Reference Manual,
 * revision r1p5 (ARM DDI 0183G), and chapter 13 of the BCM2835 ARM
 * Peripherals datasheet for the Raspberry Pi variant.
 *
 * Implemented so far: transmitting and receiving, the flag register, the
 * control and configuration registers, the interrupt registers and the
 * identification registers. A byte takes the time of its frame at the
 * baud rate to go out or to come in; the transmit and receive interrupts
 * follow the FIFOs through their trigger levels.
 *
 * Not implemented yet: loopback, and IrDA. The bytes received are never
 * in error but for overruns, and the modem status inputs never change, so
 * they never interrupt.
 */

#ifndef HW_CHAR_PL011_H
#define HW_CHAR_PL011_H

#include <stdint.h>

#include "core/bus.h"
#include "core/char_stream.h"
#include "core/irq.h"
#include "core/machine.h"

/* Size of the register block. */
#define PL011_MMIO_SIZE 0x1000

/* The largest FIFO depth a PL011 can have. */
#define PL011_MAX_FIFO_DEPTH 32

/* Differences between implementations of the PL011. */
typedef struct Pl011Config {
	/* Entries in each FIFO: 32 on r1p5, 16 on the BCM2835. */
	unsigned fifo_depth;

	/*
	 * UARTPeriphID0-3, with ID0 in the low byte; 0x00341011 on r1p5.
	 * 0 means the identification registers are not implemented: they
	 * and UARTPCellID0-3 then read as 0.
	 */
	uint32_t periph_id;
} Pl011Config;

/* The PL011 as ARM's manual describes it, revision r1p5. */
extern const Pl011Config pl011_arm_r1p5;

typedef struct Pl011 Pl011;

/*
 * Create a UART in its reset state, with a reference clock UARTCLK of
 * clock_hz. It takes the time from m, and stops m when software asks it
 * to send or receive with a baud rate divisor that the manual calls
 * invalid. Bytes it sends go to serial, and the bytes that come from
 * serial are what it receives: when serial has input, the UART becomes
 * the input of m. irq is its interrupt output (UARTINTR). Either may be
 * NULL when not connected. Returns NULL when config or clock_hz is
 * invalid, or memory runs out.
 */
Pl011 *pl011_create(Machine *m, const Pl011Config *config, uint64_t clock_hz,
                    CharStream *serial, IrqLine *irq);

void pl011_destroy(Pl011 *uart);

/* Put the UART back in its reset state. */
void pl011_reset(Pl011 *uart);

/* Map the registers on bus at base. */
BusStatus pl011_map(Pl011 *uart, Bus *bus, const char *name, uint64_t base);

#endif /* HW_CHAR_PL011_H */
