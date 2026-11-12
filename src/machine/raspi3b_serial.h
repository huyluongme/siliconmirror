/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The serial cable on the header of the Raspberry Pi 3 Model B: header
 * pin 8 is GPIO 14, which the board sends on, and pin 10 is GPIO 15,
 * which it receives on (Raspberry Pi documentation, "Configure UARTs").
 * The host's back end, such as its terminal, is at the other end.
 *
 * A UART reaches the cable through the GPIO pins that carry its signals,
 * as the table of section 6.2 of "BCM2835 ARM Peripherals" gives them:
 *
 *            TXD                         RXD
 *   UART0    GPIO 14 ALT0, 32 ALT3,      GPIO 15 ALT0, 33 ALT3,
 *   (PL011)  36 ALT2                     37 ALT2
 *   UART1    GPIO 14 ALT5, 32 ALT5,      GPIO 15 ALT5, 33 ALT5,
 *   (mini)   40 ALT5                     41 ALT5
 *
 * What a UART sends reaches the back end while GPIO 14 carries its TXD,
 * and it receives what the back end sends while GPIO 15 carries its RXD.
 * GPIO 32 and 33 go to the Bluetooth module (bcm2710-rpi-3-b.dts), which
 * is not modelled: it takes nothing, and keeps its TX, GPIO 33, idle and
 * high. The other pins of the table go to no serial device.
 *
 * A UART's receive line is held low when no pin carries its RXD, or when
 * the pin that does is low; the lowest-numbered pin counts when several
 * carry it. The cable keeps GPIO 15 high while the back end sends
 * nothing, also without a back end, and a UART keeps its TXD pins high.
 * Bytes that the back end sends while no UART listens on GPIO 15 are
 * lost.
 */

#ifndef MACHINE_RASPI3B_SERIAL_H
#define MACHINE_RASPI3B_SERIAL_H

#include "core/char_stream.h"
#include "core/machine.h"
#include "hw/gpio/bcm2835_gpio.h"

enum {
	RASPI3B_SERIAL_UART0, /* the PL011 */
	RASPI3B_SERIAL_UART1, /* the mini UART */
	RASPI3B_SERIAL_NUM_UARTS,
};

typedef struct Raspi3bSerial Raspi3bSerial;

/*
 * Plug the cable into the pins of gpio, with back_end, which may be NULL,
 * at its other end. The cable does not own back_end. Returns NULL when m
 * has no room for another input, or memory runs out.
 */
Raspi3bSerial *raspi3b_serial_create(Machine *m, Bcm2835Gpio *gpio,
                                     CharStream *back_end);
void raspi3b_serial_destroy(Raspi3bSerial *serial);

/* The stream that a UART sends to and receives from. */
CharStream *raspi3b_serial_uart(Raspi3bSerial *serial, unsigned uart);

#endif /* MACHINE_RASPI3B_SERIAL_H */
