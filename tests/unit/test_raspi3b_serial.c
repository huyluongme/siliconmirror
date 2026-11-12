/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the serial cable on the Raspberry Pi 3's header
 * (src/machine/raspi3b_serial.c): which UART reaches the back end through
 * GPIO 14 and 15, and the receive lines of the UARTs.
 */

#include <stdint.h>
#include <string.h>

#include "core/char_stream.h"
#include "core/machine.h"
#include "hw/gpio/bcm2835_gpio.h"
#include "machine/raspi3b_serial.h"
#include "test.h"
#include "util/macros.h"

/* A back end that collects what it gets, and sends its input. */
typedef struct BackEnd {
	CharStream stream;
	char data[64];
	size_t len;
	const char *input;
	size_t input_pos;
} BackEnd;

static void back_write(CharStream *s, const uint8_t *buf, size_t len)
{
	BackEnd *b = container_of(s, BackEnd, stream);

	for (size_t i = 0; i < len && b->len < sizeof(b->data) - 1; i++)
		b->data[b->len++] = (char)buf[i];
	b->data[b->len] = '\0';
}

static bool back_read(CharStream *s, uint8_t *byte)
{
	BackEnd *b = container_of(s, BackEnd, stream);

	if (b->input == NULL || b->input[b->input_pos] == '\0')
		return false;
	*byte = (uint8_t)b->input[b->input_pos++];
	return true;
}

static bool back_wait(CharStream *s)
{
	BackEnd *b = container_of(s, BackEnd, stream);

	return b->input != NULL && b->input[b->input_pos] != '\0';
}

static void back_destroy(CharStream *s)
{
	(void)s;
}

static const CharStreamOps back_ops = {
	.write = back_write,
	.read = back_read,
	.wait = back_wait,
	.destroy = back_destroy,
};

typedef struct Setup {
	Machine *m;
	Bcm2835Gpio *gpio;
	Raspi3bSerial *cable;
	BackEnd back;
	CharStream *uart0, *uart1;
} Setup;

static void setup(Setup *s, bool back_end)
{
	IrqLine *irqs[BCM2835_GPIO_NUM_IRQS] = {NULL};

	memset(s, 0, sizeof(*s));
	s->back.stream.ops = &back_ops;
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->gpio = bcm2835_gpio_create(s->m, irqs);
	CHECK(s->gpio != NULL);
	s->cable =
		raspi3b_serial_create(s->m, s->gpio, back_end ? &s->back.stream : NULL);
	CHECK(s->cable != NULL);
	s->uart0 = raspi3b_serial_uart(s->cable, RASPI3B_SERIAL_UART0);
	s->uart1 = raspi3b_serial_uart(s->cable, RASPI3B_SERIAL_UART1);
}

static void teardown(Setup *s)
{
	raspi3b_serial_destroy(s->cable);
	bcm2835_gpio_destroy(s->gpio);
	machine_destroy(s->m);
}

static void put(CharStream *s, const char *text)
{
	char_stream_write(s, (const uint8_t *)text, strlen(text));
}

/* What reaches the back end: the UART whose TXD GPIO 14 carries. */
static void test_transmit(void)
{
	Setup s;

	setup(&s, true);
	put(s.uart0, "a");
	put(s.uart1, "b");
	CHECK_EQ(s.back.len, 0);
	bcm2835_gpio_set_function(s.gpio, 14, BCM2835_GPIO_ALT5);
	put(s.uart0, "c");
	put(s.uart1, "d");
	bcm2835_gpio_set_function(s.gpio, 14, BCM2835_GPIO_ALT0);
	put(s.uart0, "e");
	put(s.uart1, "f");
	CHECK(strcmp(s.back.data, "de") == 0);

	/* The Bluetooth pins and the others take nothing to the back end. */
	bcm2835_gpio_set_function(s.gpio, 14, BCM2835_GPIO_INPUT);
	bcm2835_gpio_set_function(s.gpio, 32, BCM2835_GPIO_ALT3);
	bcm2835_gpio_set_function(s.gpio, 40, BCM2835_GPIO_ALT5);
	put(s.uart0, "g");
	put(s.uart1, "h");
	CHECK(strcmp(s.back.data, "de") == 0);
	teardown(&s);
}

/* The UART whose RXD GPIO 15 carries receives what the back end sends. */
static void test_receive(void)
{
	Setup s;
	uint8_t byte = 0;

	setup(&s, true);
	s.back.input = "xy";
	CHECK(!char_stream_read(s.uart0, &byte));
	CHECK(!char_stream_read(s.uart1, &byte));
	CHECK(!char_stream_wait(s.uart1));
	CHECK_EQ(s.back.input_pos, 0);
	bcm2835_gpio_set_function(s.gpio, 15, BCM2835_GPIO_ALT5);
	CHECK(!char_stream_read(s.uart0, &byte));
	CHECK(!char_stream_wait(s.uart0));
	CHECK(char_stream_wait(s.uart1));
	CHECK(char_stream_read(s.uart1, &byte));
	CHECK_EQ(byte, 'x');
	bcm2835_gpio_set_function(s.gpio, 15, BCM2835_GPIO_ALT0);
	CHECK(char_stream_read(s.uart0, &byte));
	CHECK_EQ(byte, 'y');
	CHECK(!char_stream_wait(s.uart0)); /* the input has ended */

	/* GPIO 33 carries RXD from the Bluetooth module: nothing comes. */
	s.back.input = "z";
	s.back.input_pos = 0;
	bcm2835_gpio_set_function(s.gpio, 15, BCM2835_GPIO_INPUT);
	bcm2835_gpio_set_function(s.gpio, 33, BCM2835_GPIO_ALT3);
	CHECK(!char_stream_line_low(s.uart0)); /* Bluetooth keeps it high */
	CHECK(!char_stream_read(s.uart0, &byte));
	CHECK(!char_stream_wait(s.uart0));
	CHECK_EQ(s.back.input_pos, 0);
	teardown(&s);
}

/*
 * A receive line is held low when no pin carries RXD, or the pin that
 * does is low; the lowest-numbered pin counts.
 */
static void test_line_low(void)
{
	Setup s;

	setup(&s, true);
	CHECK(char_stream_line_low(s.uart0));
	CHECK(char_stream_line_low(s.uart1));
	bcm2835_gpio_set_function(s.gpio, 15, BCM2835_GPIO_ALT5);
	CHECK(!char_stream_line_low(s.uart1)); /* the cable keeps it high */
	CHECK(char_stream_line_low(s.uart0));
	bcm2835_gpio_set_function(s.gpio, 41, BCM2835_GPIO_ALT5);
	CHECK(!char_stream_line_low(s.uart1));
	bcm2835_gpio_set_function(s.gpio, 15, BCM2835_GPIO_INPUT);
	CHECK(char_stream_line_low(s.uart1)); /* GPIO 41: pulled down */
	bcm2835_gpio_set_function(s.gpio, 33, BCM2835_GPIO_ALT5);
	CHECK(!char_stream_line_low(s.uart1)); /* Bluetooth keeps 33 high */
	bcm2835_gpio_set_function(s.gpio, 37, BCM2835_GPIO_ALT2);
	CHECK(char_stream_line_low(s.uart0));
	bcm2835_gpio_drive(s.gpio, 37, BCM2835_GPIO_HIGH);
	CHECK(!char_stream_line_low(s.uart0));
	bcm2835_gpio_set_function(s.gpio, 33, BCM2835_GPIO_INPUT);
	bcm2835_gpio_drive(s.gpio, 41, BCM2835_GPIO_HIGH);
	CHECK(!char_stream_line_low(s.uart1));
	teardown(&s);

	/* Without a back end, the cable still keeps GPIO 15 high. */
	setup(&s, false);
	bcm2835_gpio_set_function(s.gpio, 15, BCM2835_GPIO_ALT0);
	CHECK(!char_stream_line_low(s.uart0));
	CHECK_EQ(s.m->num_inputs, 0);
	teardown(&s);
}

/* The UARTs keep their TXD pins high; the cable keeps GPIO 15 high. */
static void test_levels(void)
{
	static const struct {
		unsigned pin;
		Bcm2835GpioFunction function;
	} txd[] = {
		{14, BCM2835_GPIO_ALT0}, {14, BCM2835_GPIO_ALT5},
		{32, BCM2835_GPIO_ALT3}, {32, BCM2835_GPIO_ALT5},
		{36, BCM2835_GPIO_ALT2}, {40, BCM2835_GPIO_ALT5},
	};
	Setup s;

	setup(&s, true);
	for (unsigned i = 0; i < ARRAY_SIZE(txd); i++) {
		CHECK(!bcm2835_gpio_level(s.gpio, txd[i].pin) || txd[i].pin == 36);
		bcm2835_gpio_set_function(s.gpio, txd[i].pin, txd[i].function);
		CHECK(bcm2835_gpio_level(s.gpio, txd[i].pin));
		bcm2835_gpio_set_function(s.gpio, txd[i].pin, BCM2835_GPIO_INPUT);
	}
	CHECK(bcm2835_gpio_level(s.gpio, 15));
	CHECK(bcm2835_gpio_level(s.gpio, 33));
	/* Pin 36 is pulled up: what drives it from outside shows the UART. */
	bcm2835_gpio_drive(s.gpio, 36, BCM2835_GPIO_LOW);
	CHECK(!bcm2835_gpio_level(s.gpio, 36));
	bcm2835_gpio_set_function(s.gpio, 36, BCM2835_GPIO_ALT2);
	CHECK(bcm2835_gpio_level(s.gpio, 36));
	char_stream_destroy(s.uart0); /* the cable owns its ends: no effect */
	put(s.uart0, "a");
	teardown(&s);
}

/*
 * What the back end sends while no UART listens on GPIO 15 is lost, at
 * the machine's poll; the cable never waits.
 */
static void test_lost_input(void)
{
	Setup s;
	MachineInput *input;
	int contexts[MACHINE_MAX_INPUTS];

	setup(&s, true);
	CHECK_EQ(s.m->num_inputs, 1);
	input = &s.m->inputs[0];
	s.back.input = "ab";
	bcm2835_gpio_set_function(s.gpio, 15, BCM2835_GPIO_ALT5);
	input->poll(input->ctx);
	CHECK_EQ(s.back.input_pos, 0);
	bcm2835_gpio_set_function(s.gpio, 15, BCM2835_GPIO_ALT0);
	input->poll(input->ctx);
	CHECK_EQ(s.back.input_pos, 0);
	bcm2835_gpio_set_function(s.gpio, 15, BCM2835_GPIO_ALT3);
	CHECK(!input->wait(input->ctx));
	input->poll(input->ctx);
	CHECK_EQ(s.back.input_pos, 2);
	raspi3b_serial_destroy(s.cable); /* removes its input */
	CHECK_EQ(s.m->num_inputs, 0);
	bcm2835_gpio_destroy(s.gpio);
	machine_destroy(s.m);

	/* A machine without room for the cable's input. */
	setup(&s, false);
	for (unsigned i = 0; i < MACHINE_MAX_INPUTS; i++)
		CHECK(machine_add_input(s.m, NULL, NULL, &contexts[i]));
	CHECK(raspi3b_serial_create(s.m, s.gpio, &s.back.stream) == NULL);
	raspi3b_serial_destroy(s.cable);
	CHECK_EQ(s.m->num_inputs, MACHINE_MAX_INPUTS);
	bcm2835_gpio_destroy(s.gpio);
	machine_destroy(s.m);
}

int main(void)
{
	RUN_TEST(test_transmit);
	RUN_TEST(test_receive);
	RUN_TEST(test_line_low);
	RUN_TEST(test_levels);
	RUN_TEST(test_lost_input);
	return test_summary();
}
