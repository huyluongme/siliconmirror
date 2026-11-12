/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The auxiliary peripherals of the BCM2835: the mini UART (UART1), and
 * two SPI masters that share its enable and interrupt registers.
 *
 * Written from chapter 2 of Broadcom's "BCM2835 ARM Peripherals" (2012)
 * and chapter 2 of Raspberry Pi's "BCM2711 ARM Peripherals" (2022), which
 * fixes the swapped titles of AUX_MU_IER_REG and AUX_MU_IIR_REG.
 *
 * Implemented: AUX_IRQ, AUX_ENABLES and the mini UART, with its 8-byte
 * FIFOs, its baud rate of system clock / (8 * (AUX_MU_BAUD_REG + 1)), the
 * 7-bit and 8-bit formats and its interrupt.
 *
 * Not implemented yet: the SPI masters, whose registers stop the
 * machine; break, which the bytes sent to the host cannot carry; and
 * flow control with CTS, which stops the machine as no board connects
 * CTS. RTS goes nowhere.
 */

#ifndef HW_CHAR_BCM2835_AUX_H
#define HW_CHAR_BCM2835_AUX_H

#include <stdint.h>

#include "core/bus.h"
#include "core/char_stream.h"
#include "core/irq.h"
#include "core/machine.h"

/* The register block, from 0x7E215000 to the end of SPI 2. */
#define BCM2835_AUX_MMIO_SIZE 0x100

typedef struct Bcm2835Aux Bcm2835Aux;

/*
 * Create the block in its reset state, with a system clock of clock_hz.
 * It takes the time from m, and stops m on what it does not emulate.
 * The mini UART sends to serial and receives what comes from it: when
 * serial has input, the block becomes one of m's inputs. irq is the
 * block's interrupt output. Either may be NULL when not connected.
 * Returns NULL when clock_hz is 0, m has no room for another input, or
 * memory runs out.
 */
Bcm2835Aux *bcm2835_aux_create(Machine *m, uint64_t clock_hz,
                               CharStream *serial, IrqLine *irq);
void bcm2835_aux_destroy(Bcm2835Aux *aux);

/* Put the block back in its reset state. */
void bcm2835_aux_reset(Bcm2835Aux *aux);

/* Map the registers on bus at base. */
BusStatus bcm2835_aux_map(Bcm2835Aux *aux, Bus *bus, const char *name,
                          uint64_t base);

#endif /* HW_CHAR_BCM2835_AUX_H */
