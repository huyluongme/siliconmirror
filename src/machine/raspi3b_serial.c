/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The serial cable on the Raspberry Pi 3's header; see raspi3b_serial.h. */

#include "machine/raspi3b_serial.h"

#include <stdlib.h>

#define CABLE_TX 14 /* GPIO 14: the board sends to the cable */
#define CABLE_RX 15 /* GPIO 15: the board receives from the cable */
#define BLUETOOTH_TX 33

#define NUM_PINS 3 /* pins that carry each signal of a UART */

typedef struct Pin {
	unsigned pin;
	Bcm2835GpioFunction function;
} Pin;

/* The pins of TXD and RXD of each UART, the cable's first. */
static const Pin tx_pins[RASPI3B_SERIAL_NUM_UARTS][NUM_PINS] = {
	{{14, BCM2835_GPIO_ALT0}, {32, BCM2835_GPIO_ALT3}, {36, BCM2835_GPIO_ALT2}},
	{{14, BCM2835_GPIO_ALT5}, {32, BCM2835_GPIO_ALT5}, {40, BCM2835_GPIO_ALT5}},
};
static const Pin rx_pins[RASPI3B_SERIAL_NUM_UARTS][NUM_PINS] = {
	{{15, BCM2835_GPIO_ALT0}, {33, BCM2835_GPIO_ALT3}, {37, BCM2835_GPIO_ALT2}},
	{{15, BCM2835_GPIO_ALT5}, {33, BCM2835_GPIO_ALT5}, {41, BCM2835_GPIO_ALT5}},
};

/* The end of the cable that a UART sees. */
typedef struct UartEnd {
	CharStream stream; /* first, so that a CharStream * is a UartEnd * */
	Raspi3bSerial *serial;
	unsigned uart;
} UartEnd;

struct Raspi3bSerial {
	Machine *machine;
	Bcm2835Gpio *gpio;
	CharStream *back_end;
	UartEnd ends[RASPI3B_SERIAL_NUM_UARTS];
};

/* The pin that carries the RXD of the UART of end, or -1 when none does. */
static int rx_pin(const UartEnd *end)
{
	for (unsigned i = 0; i < NUM_PINS; i++) {
		const Pin *p = &rx_pins[end->uart][i];

		if (bcm2835_gpio_function(end->serial->gpio, p->pin) == p->function)
			return (int)p->pin;
	}
	return -1;
}

static void end_write(CharStream *s, const uint8_t *buf, size_t len)
{
	UartEnd *end = (UartEnd *)s;

	if (bcm2835_gpio_function(end->serial->gpio, CABLE_TX) ==
	    tx_pins[end->uart][0].function)
		char_stream_write(end->serial->back_end, buf, len);
}

static bool end_read(CharStream *s, uint8_t *byte)
{
	UartEnd *end = (UartEnd *)s;

	return rx_pin(end) == CABLE_RX &&
	       char_stream_read(end->serial->back_end, byte);
}

static bool end_wait(CharStream *s)
{
	UartEnd *end = (UartEnd *)s;

	return rx_pin(end) == CABLE_RX && char_stream_wait(end->serial->back_end);
}

static bool end_line_low(CharStream *s)
{
	UartEnd *end = (UartEnd *)s;
	int pin = rx_pin(end);

	return pin < 0 || !bcm2835_gpio_level(end->serial->gpio, (unsigned)pin);
}

static void end_destroy(CharStream *s)
{
	(void)s; /* the cable owns its ends */
}

static const CharStreamOps end_ops = {
	.write = end_write,
	.read = end_read,
	.wait = end_wait,
	.line_low = end_line_low,
	.destroy = end_destroy,
};

/* The machine's input: lose what the back end sends when no one listens. */
static void cable_poll(void *ctx)
{
	Raspi3bSerial *serial = ctx;
	uint8_t byte;

	for (unsigned uart = 0; uart < RASPI3B_SERIAL_NUM_UARTS; uart++)
		if (rx_pin(&serial->ends[uart]) == CABLE_RX)
			return;
	while (char_stream_read(serial->back_end, &byte))
		continue;
}

static bool cable_wait(void *ctx)
{
	(void)ctx;
	return false;
}

Raspi3bSerial *raspi3b_serial_create(Machine *m, Bcm2835Gpio *gpio,
                                     CharStream *back_end)
{
	Raspi3bSerial *serial = calloc(1, sizeof(*serial));

	if (serial == NULL)
		return NULL;
	serial->machine = m;
	serial->gpio = gpio;
	serial->back_end = back_end;
	for (unsigned uart = 0; uart < RASPI3B_SERIAL_NUM_UARTS; uart++) {
		serial->ends[uart] =
			(UartEnd){.stream.ops = &end_ops, .serial = serial, .uart = uart};
		for (unsigned i = 0; i < NUM_PINS; i++)
			bcm2835_gpio_drive_alt(gpio, tx_pins[uart][i].pin,
			                       tx_pins[uart][i].function,
			                       BCM2835_GPIO_HIGH);
	}
	bcm2835_gpio_drive(gpio, CABLE_RX, BCM2835_GPIO_HIGH);
	bcm2835_gpio_drive(gpio, BLUETOOTH_TX, BCM2835_GPIO_HIGH);
	if (char_stream_has_input(back_end) &&
	    !machine_add_input(m, cable_poll, cable_wait, serial)) {
		free(serial);
		return NULL;
	}
	return serial;
}

void raspi3b_serial_destroy(Raspi3bSerial *serial)
{
	machine_remove_input(serial->machine, serial);
	free(serial);
}

CharStream *raspi3b_serial_uart(Raspi3bSerial *serial, unsigned uart)
{
	return &serial->ends[uart].stream;
}
