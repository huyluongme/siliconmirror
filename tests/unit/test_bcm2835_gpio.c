/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835 GPIO block (src/hw/gpio/bcm2835_gpio.c).
 * Expected values come from chapter 6 of "BCM2835 ARM Peripherals", and
 * the interrupt lines from section 5.1 of "BCM2711 ARM Peripherals".
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/gpio/bcm2835_gpio.h"
#include "test.h"

#define BASE 0x3f200000

enum {
	GPFSEL0 = 0x00,
	GPFSEL1 = 0x04,
	GPFSEL2 = 0x08,
	GPFSEL5 = 0x14,
	GPSET0 = 0x1c,
	GPSET1 = 0x20,
	GPCLR0 = 0x28,
	GPCLR1 = 0x2c,
	GPLEV0 = 0x34,
	GPLEV1 = 0x38,
	GPEDS0 = 0x40,
	GPEDS1 = 0x44,
	GPREN0 = 0x4c,
	GPREN1 = 0x50,
	GPFEN0 = 0x58,
	GPHEN0 = 0x64,
	GPHEN1 = 0x68,
	GPLEN0 = 0x70,
	GPLEN1 = 0x74,
	GPAREN0 = 0x7c,
	GPAFEN0 = 0x88,
	GPPUD = 0x94,
	GPPUDCLK0 = 0x98,
	GPPUDCLK1 = 0x9c,
};

/* The levels after reset: the pulls of the table in section 6.2. */
#define RESET_LEV0 UINT64_C(0x000001ff) /* pins 0-8 high */
#define RESET_LEV1 UINT64_C(0x003fc01c) /* pins 34-36 and 46-53 high */

typedef struct Setup {
	Machine *m;
	Bcm2835Gpio *gpio;
	IrqLine irqs[BCM2835_GPIO_NUM_IRQS];
} Setup;

static void setup(Setup *s)
{
	IrqLine *lines[BCM2835_GPIO_NUM_IRQS];

	memset(s, 0, sizeof(*s));
	s->m = machine_create();
	CHECK(s->m != NULL);
	for (unsigned i = 0; i < BCM2835_GPIO_NUM_IRQS; i++) {
		irq_init(&s->irqs[i], NULL, NULL, i);
		lines[i] = &s->irqs[i];
	}
	s->gpio = bcm2835_gpio_create(s->m, lines);
	CHECK(s->gpio != NULL);
	CHECK_EQ(bcm2835_gpio_map(s->gpio, s->m->bus, "gpio", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_gpio_destroy(s->gpio);
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

/* The levels of the four interrupt lines, bit n for line n. */
static unsigned irqs(const Setup *s)
{
	unsigned levels = 0;

	for (unsigned i = 0; i < BCM2835_GPIO_NUM_IRQS; i++)
		levels |= (unsigned)irq_level(&s->irqs[i]) << i;
	return levels;
}

static void test_reset_values(void)
{
	Setup s;

	setup(&s);
	for (uint64_t offset = GPFSEL0; offset <= GPFSEL5; offset += 4)
		CHECK_EQ(reg_read(&s, offset), 0);
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0);
	CHECK_EQ(reg_read(&s, GPLEV1), RESET_LEV1);
	for (uint64_t offset = GPEDS0; offset <= GPAFEN0 + 4; offset += 12) {
		CHECK_EQ(reg_read(&s, offset), 0);
		CHECK_EQ(reg_read(&s, offset + 4), 0);
	}
	CHECK_EQ(reg_read(&s, GPPUD), 0);
	CHECK_EQ(reg_read(&s, GPPUDCLK0), 0);
	CHECK_EQ(reg_read(&s, GPPUDCLK1), 0);
	CHECK_EQ(irqs(&s), 0);

	/*
	 * Reset puts back the functions, the latch, the pulls, the events and
	 * their enables, GPPUD and GPPUDCLKn, and lowers the lines.
	 */
	reg_write(&s, GPFSEL2, 1 << 3); /* pin 21: output */
	reg_write(&s, GPSET0, 1u << 21 | 1u << 0);
	reg_write(&s, GPPUD, 2);
	reg_write(&s, GPPUDCLK0, 1u << 9);
	reg_write(&s, GPPUDCLK1, 1u << 9);
	for (uint64_t offset = GPREN0; offset <= GPAFEN0 + 4; offset += 12) {
		reg_write(&s, offset, 1u << 3);
		reg_write(&s, offset + 4, 1u << 3);
	}
	CHECK_EQ(irqs(&s), 0xb); /* high levels on pins 3 and 35 */
	bcm2835_gpio_reset(s.gpio);
	CHECK_EQ(irqs(&s), 0);
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0);
	CHECK_EQ(reg_read(&s, GPFSEL2), 0);
	for (uint64_t offset = GPEDS0; offset <= GPAFEN0 + 4; offset += 12) {
		CHECK_EQ(reg_read(&s, offset), 0);
		CHECK_EQ(reg_read(&s, offset + 4), 0);
	}
	CHECK_EQ(reg_read(&s, GPPUD), 0);
	CHECK_EQ(reg_read(&s, GPPUDCLK0), 0);
	CHECK_EQ(reg_read(&s, GPPUDCLK1), 0);
	reg_write(&s, GPFSEL2, 1 << 3);
	reg_write(&s, GPFSEL0, 1 << 0); /* pin 0, pulled up, as an output */
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0 & ~UINT64_C(1));
	teardown(&s);
}

/* GPFSELn: three bits a pin, ten pins a register, 54 pins. */
static void test_function_select(void)
{
	Setup s;

	setup(&s);
	reg_write(&s, GPFSEL1, 0x12000);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 14), BCM2835_GPIO_ALT5);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 15), BCM2835_GPIO_ALT5);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 13), BCM2835_GPIO_INPUT);
	CHECK_EQ(reg_read(&s, GPFSEL1), 0x12000);
	reg_write(&s, GPFSEL0, UINT32_MAX);
	CHECK_EQ(reg_read(&s, GPFSEL0), 0x3fffffff);
	CHECK_EQ(reg_read(&s, GPFSEL1), 0x12000);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 9), BCM2835_GPIO_ALT3);
	reg_write(&s, GPFSEL5, UINT32_MAX);
	CHECK_EQ(reg_read(&s, GPFSEL5), 0xfff);
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0); /* the pulls are intact */
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 53), BCM2835_GPIO_ALT3);
	bcm2835_gpio_set_function(s.gpio, 40, BCM2835_GPIO_ALT0);
	CHECK_EQ(reg_read(&s, GPFSEL0 + 16), BCM2835_GPIO_ALT0);

	/* The codes of Table 6-2: 000 input ... 010 ALT5. */
	reg_write(&s, GPFSEL2,
	          0 | 1 << 3 | 2 << 6 | 3 << 9 | 4 << 12 | 5 << 15 | 6 << 18 |
	              7 << 21);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 20), BCM2835_GPIO_INPUT);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 21), BCM2835_GPIO_OUTPUT);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 22), BCM2835_GPIO_ALT5);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 23), BCM2835_GPIO_ALT4);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 24), BCM2835_GPIO_ALT0);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 25), BCM2835_GPIO_ALT1);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 26), BCM2835_GPIO_ALT2);
	CHECK_EQ(bcm2835_gpio_function(s.gpio, 27), BCM2835_GPIO_ALT3);
	CHECK_EQ(BCM2835_GPIO_INPUT, 0);
	CHECK_EQ(BCM2835_GPIO_OUTPUT, 1);
	CHECK_EQ(BCM2835_GPIO_ALT5, 2);
	CHECK_EQ(BCM2835_GPIO_ALT4, 3);
	CHECK_EQ(BCM2835_GPIO_ALT0, 4);
	CHECK_EQ(BCM2835_GPIO_ALT1, 5);
	CHECK_EQ(BCM2835_GPIO_ALT2, 6);
	CHECK_EQ(BCM2835_GPIO_ALT3, 7);
	teardown(&s);
}

/*
 * GPSETn and GPCLRn change the output latch, which an output drives; an
 * input keeps it for when it becomes an output.
 */
static void test_outputs(void)
{
	Setup s;

	setup(&s);
	reg_write(&s, GPSET0, 1u << 21); /* an input: no change yet */
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0);
	reg_write(&s, GPFSEL2, 1 << 3 | 1 << 0); /* pins 20 and 21 */
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0 | 1u << 21);
	reg_write(&s, GPSET0, 1u << 20);
	reg_write(&s, GPCLR0, 1u << 21);
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0 | 1u << 20);
	reg_write(&s, GPCLR0, 0);
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0 | 1u << 20);

	/* Bank 1: pins 32 to 53, bits 0 to 21. */
	bcm2835_gpio_set_function(s.gpio, 53, BCM2835_GPIO_OUTPUT);
	bcm2835_gpio_set_function(s.gpio, 32, BCM2835_GPIO_OUTPUT);
	reg_write(&s, GPCLR1, UINT32_MAX);
	CHECK_EQ(reg_read(&s, GPLEV1), RESET_LEV1 & ~(UINT64_C(1) << 21));
	reg_write(&s, GPSET1, UINT32_MAX);
	CHECK_EQ(reg_read(&s, GPLEV1), RESET_LEV1 | 1);
	CHECK(bcm2835_gpio_level(s.gpio, 32));
	reg_write(&s, GPCLR1, 1u << 21);
	CHECK(!bcm2835_gpio_level(s.gpio, 53));
	CHECK(bcm2835_gpio_level(s.gpio, 32));
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/*
 * The level of an input or an alternate function comes from what drives
 * it, else its pull; a peripheral drives the pins of its function.
 */
static void test_drives(void)
{
	Setup s;

	setup(&s);
	bcm2835_gpio_drive(s.gpio, 5, BCM2835_GPIO_LOW);
	CHECK(!bcm2835_gpio_level(s.gpio, 5));
	bcm2835_gpio_drive(s.gpio, 5, BCM2835_GPIO_UNDRIVEN);
	CHECK(bcm2835_gpio_level(s.gpio, 5)); /* pulled up */
	bcm2835_gpio_drive(s.gpio, 28, BCM2835_GPIO_HIGH);
	CHECK(bcm2835_gpio_level(s.gpio, 28));
	bcm2835_gpio_drive(s.gpio, 28, BCM2835_GPIO_UNDRIVEN);
	CHECK(!bcm2835_gpio_level(s.gpio, 28)); /* floating: 0 */

	bcm2835_gpio_drive_alt(s.gpio, 14, BCM2835_GPIO_ALT0, BCM2835_GPIO_HIGH);
	CHECK(!bcm2835_gpio_level(s.gpio, 14));
	reg_write(&s, GPFSEL1, BCM2835_GPIO_ALT0 << 12);
	CHECK(bcm2835_gpio_level(s.gpio, 14));
	reg_write(&s, GPFSEL1, BCM2835_GPIO_ALT5 << 12);
	CHECK(!bcm2835_gpio_level(s.gpio, 14)); /* no peripheral drives it */
	bcm2835_gpio_drive(s.gpio, 14, BCM2835_GPIO_HIGH);
	CHECK(bcm2835_gpio_level(s.gpio, 14));
	bcm2835_gpio_drive_alt(s.gpio, 14, BCM2835_GPIO_ALT5, BCM2835_GPIO_LOW);
	CHECK(!bcm2835_gpio_level(s.gpio, 14)); /* the peripheral first */

	/* An output drives its latch, whatever drives the pin. */
	bcm2835_gpio_drive(s.gpio, 6, BCM2835_GPIO_LOW);
	reg_write(&s, GPSET0, 1u << 6);
	reg_write(&s, GPFSEL0, 1 << 18);
	CHECK(bcm2835_gpio_level(s.gpio, 6));

	/* The machine stops on no access to the block. */
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/*
 * GPPUD and GPPUDCLKn: a write of GPPUDCLKn clocks GPPUD into the pins
 * whose bits it sets.
 */
static void test_pulls(void)
{
	Setup s;

	setup(&s);
	reg_write(&s, GPPUD, 2); /* up */
	CHECK_EQ(reg_read(&s, GPPUD), 2);
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0);
	reg_write(&s, GPPUDCLK0, 1u << 10 | 1u << 28);
	CHECK_EQ(reg_read(&s, GPPUDCLK0), 1u << 10 | 1u << 28);
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0 | 1u << 10 | 1u << 28);
	reg_write(&s, GPPUD, 0);
	reg_write(&s, GPPUDCLK0, 0);
	CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0 | 1u << 10 | 1u << 28);

	/* Down, then none, in bank 1. */
	reg_write(&s, GPPUD, 1);
	reg_write(&s, GPPUDCLK1, UINT32_MAX);
	CHECK_EQ(reg_read(&s, GPPUDCLK1), 0x3fffff);
	CHECK_EQ(reg_read(&s, GPLEV1), 0);
	reg_write(&s, GPPUD, 2);
	reg_write(&s, GPPUDCLK1, 1u << 2);
	CHECK_EQ(reg_read(&s, GPLEV1), 1u << 2);
	reg_write(&s, GPPUD, 0x7);
	CHECK_EQ(reg_read(&s, GPPUD), 3);
	reg_write(&s, GPPUDCLK1, 0); /* clocks no pin: nothing happens */
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);

	/* Pin 0, the first of GPPUDCLK0. */
	reg_write(&s, GPPUD, 1);
	reg_write(&s, GPPUDCLK0, 1);
	CHECK_EQ(reg_read(&s, GPLEV0),
	         (RESET_LEV0 | 1u << 10 | 1u << 28) & ~UINT64_C(1));
	teardown(&s);
}

/*
 * GPEDSn records the edges and levels that the enable registers ask for,
 * until written with 1; a level stays recorded while it lasts. The lines
 * follow GPEDSn: banks 0-27, 28-45 and 46-53, then every pin.
 */
static void test_events(void)
{
	Setup s;

	setup(&s);
	bcm2835_gpio_set_function(s.gpio, 22, BCM2835_GPIO_OUTPUT);
	reg_write(&s, GPREN0, 1u << 22);
	CHECK_EQ(reg_read(&s, GPREN0), 1u << 22);
	reg_write(&s, GPCLR0, 1u << 22); /* no edge */
	CHECK_EQ(reg_read(&s, GPEDS0), 0);
	reg_write(&s, GPSET0, 1u << 22);
	CHECK_EQ(reg_read(&s, GPEDS0), 1u << 22);
	CHECK_EQ(irqs(&s), 0x9);
	reg_write(&s, GPEDS0, 1u << 21); /* another pin: no change */
	CHECK_EQ(reg_read(&s, GPEDS0), 1u << 22);
	reg_write(&s, GPEDS0, 1u << 22);
	CHECK_EQ(reg_read(&s, GPEDS0), 0);
	CHECK_EQ(irqs(&s), 0);
	reg_write(&s, GPCLR0, 1u << 22); /* a falling edge: not enabled */
	CHECK_EQ(reg_read(&s, GPEDS0), 0);

	/* Falling edges, synchronous and asynchronous, rising asynchronous. */
	reg_write(&s, GPREN0, 0);
	reg_write(&s, GPFEN0, 1u << 22);
	reg_write(&s, GPSET0, 1u << 22);
	CHECK_EQ(reg_read(&s, GPEDS0), 0);
	reg_write(&s, GPCLR0, 1u << 22);
	CHECK_EQ(reg_read(&s, GPEDS0), 1u << 22);
	reg_write(&s, GPEDS0, UINT32_MAX);
	reg_write(&s, GPFEN0, 0);
	reg_write(&s, GPAFEN0, 1u << 22);
	reg_write(&s, GPSET0, 1u << 22);
	CHECK_EQ(reg_read(&s, GPEDS0), 0);
	reg_write(&s, GPCLR0, 1u << 22);
	CHECK_EQ(reg_read(&s, GPEDS0), 1u << 22);
	reg_write(&s, GPEDS0, UINT32_MAX);
	reg_write(&s, GPAFEN0, 0);
	reg_write(&s, GPAREN0, 1u << 22);
	reg_write(&s, GPSET0, 1u << 22);
	CHECK_EQ(reg_read(&s, GPEDS0), 1u << 22);
	reg_write(&s, GPEDS0, UINT32_MAX);
	CHECK_EQ(reg_read(&s, GPAREN0), 1u << 22);

	/* A high level, at once; it stays while the pin is high. */
	reg_write(&s, GPHEN0, 1u << 0);
	CHECK_EQ(reg_read(&s, GPEDS0), 1u << 0);
	reg_write(&s, GPEDS0, 1u << 0);
	CHECK_EQ(reg_read(&s, GPEDS0), 1u << 0);
	bcm2835_gpio_drive(s.gpio, 0, BCM2835_GPIO_LOW);
	reg_write(&s, GPEDS0, 1u << 0);
	CHECK_EQ(reg_read(&s, GPEDS0), 0);
	reg_write(&s, GPHEN0, 0);

	/* Low levels in bank 1 (pin 30), and high in bank 2 (pin 50). */
	reg_write(&s, GPLEN0, 1u << 30);
	CHECK_EQ(reg_read(&s, GPEDS0), 1u << 30);
	CHECK_EQ(irqs(&s), 0xa);
	reg_write(&s, GPLEN0, 0);
	reg_write(&s, GPEDS0, 1u << 30);
	reg_write(&s, GPHEN1, 1u << 18);
	CHECK_EQ(reg_read(&s, GPEDS1), 1u << 18);
	CHECK_EQ(irqs(&s), 0xc);
	reg_write(&s, GPHEN1, 0);
	reg_write(&s, GPEDS1, UINT32_MAX);
	CHECK_EQ(irqs(&s), 0);
	reg_write(&s, GPLEN1, UINT32_MAX);
	CHECK_EQ(reg_read(&s, GPLEN1), 0x3fffff);
	CHECK_EQ(reg_read(&s, GPEDS1), ~RESET_LEV1 & 0x3fffff);
	CHECK_EQ(irqs(&s), 0xa);

	/* The first and last pins of each bank, and pin 0 low. */
	static const struct {
		unsigned pin;
		unsigned lines;
	} edges[] = {{27, 0x9}, {28, 0xa}, {45, 0xa}, {46, 0xc}, {53, 0xc}};

	reg_write(&s, GPLEN1, 0);
	reg_write(&s, GPEDS1, UINT32_MAX);
	for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
		unsigned pin = edges[i].pin;
		uint64_t hen = pin < 32 ? GPHEN0 : GPHEN1;
		uint64_t eds = pin < 32 ? GPEDS0 : GPEDS1;

		bcm2835_gpio_drive(s.gpio, pin, BCM2835_GPIO_HIGH);
		reg_write(&s, hen, 1u << (pin % 32));
		CHECK_EQ(irqs(&s), edges[i].lines);
		reg_write(&s, hen, 0);
		reg_write(&s, eds, UINT32_MAX);
		CHECK_EQ(irqs(&s), 0);
		bcm2835_gpio_drive(s.gpio, pin, BCM2835_GPIO_UNDRIVEN);
	}
	bcm2835_gpio_drive(s.gpio, 0, BCM2835_GPIO_LOW);
	reg_write(&s, GPLEN0, 1);
	CHECK_EQ(reg_read(&s, GPEDS0), 1);
	reg_write(&s, GPLEN0, 0);
	reg_write(&s, GPEDS0, 1);

	/* Edges from what drives an input, in bank 1. */
	reg_write(&s, GPLEN1, 0);
	reg_write(&s, GPEDS1, UINT32_MAX);
	reg_write(&s, GPREN1, 1u << 1);
	bcm2835_gpio_drive(s.gpio, 33, BCM2835_GPIO_HIGH);
	CHECK_EQ(reg_read(&s, GPEDS1), 1u << 1);
	teardown(&s);
}

/* What the document does not describe stops the machine. */
static void test_stops(void)
{
	static const struct {
		bool write;
		uint64_t offset;
		unsigned size;
		const char *message;
	} stops[] = {
		{false, GPLEV0, 2, "2-byte read from offset 0x034"},
		{false, 0x01, 4, "4-byte read from offset 0x001"},
		{true, 0x05, 4, "4-byte write to offset 0x005"},
		{true, GPSET0, 1, "1-byte write to offset 0x01c"},
		{false, 0x18, 4, "4-byte read from offset 0x018"},
		{true, 0x3c, 4, "4-byte write to offset 0x03c"},
		{false, 0xb0, 4, "4-byte read from offset 0x0b0"},
		{false, GPSET0, 4, "4-byte read from offset 0x01c"},
		{false, GPCLR1, 4, "4-byte read from offset 0x02c"},
		{true, GPLEV1, 4, "4-byte write to offset 0x038"},
		{true, 0x99, 4, "4-byte write to offset 0x099"},
		{false, 0xffc, 4, "4-byte read from offset 0xffc"}, /* the last */
	};

	for (unsigned i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
		Setup s;
		uint64_t value = 0;

		setup(&s);
		if (stops[i].write)
			bus_write(s.m->bus, BASE + stops[i].offset, stops[i].size,
			          UINT32_MAX);
		else
			bus_read(s.m->bus, BASE + stops[i].offset, stops[i].size, &value);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		CHECK(strstr(s.m->stop_message, "bcm2835-gpio: ") != NULL);
		CHECK_EQ(value, 0);
		/* The access changed nothing. */
		for (unsigned r = 0; r < 6; r++)
			CHECK_EQ(reg_read(&s, GPFSEL0 + 4 * r), 0);
		CHECK_EQ(reg_read(&s, GPLEV0), RESET_LEV0);
		CHECK_EQ(reg_read(&s, GPLEV1), RESET_LEV1);
		teardown(&s);
	}

	/* The reserved pull, clocked into a pin. */
	Setup s;

	setup(&s);
	reg_write(&s, GPPUD, 3);
	reg_write(&s, GPPUDCLK1, 1u << 14); /* pin 46, pulled up */
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message,
	             "GPPUD holds the reserved value 3 when GPPUDCLK1 clocks it") !=
	      NULL);
	CHECK_EQ(reg_read(&s, GPLEV1), RESET_LEV1);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_function_select);
	RUN_TEST(test_outputs);
	RUN_TEST(test_drives);
	RUN_TEST(test_pulls);
	RUN_TEST(test_events);
	RUN_TEST(test_stops);
	return test_summary();
}
