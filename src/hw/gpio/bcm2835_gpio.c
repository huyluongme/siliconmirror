/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835 GPIO block; see bcm2835_gpio.h. Tables are from chapter 6
 * of "BCM2835 ARM Peripherals".
 *
 * What the document leaves open, this model chooses:
 *  - The output latch is 0 after reset, so a pin made an output before
 *    any write of GPSETn or GPCLRn drives 0.
 *  - A write of GPPUDCLKn clocks GPPUD into every pin whose bit it sets,
 *    at once: the waits of 150 cycles that the document asks for are not
 *    needed, and doing them does no harm.
 *  - The pins change level at once, so the synchronous edge detection of
 *    GPRENn and GPFENn sees every edge, like the asynchronous one.
 */

#include "hw/gpio/bcm2835_gpio.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>

/* Register offsets, table 6-1. Each n holds pins 0-31, n + 4 pins 32-53. */
enum {
	REG_GPFSEL0 = 0x00, /* to GPFSEL5 at 0x14 */
	REG_GPSET0 = 0x1c,
	REG_GPCLR0 = 0x28,
	REG_GPLEV0 = 0x34,
	REG_GPEDS0 = 0x40,
	REG_GPREN0 = 0x4c,
	REG_GPFEN0 = 0x58,
	REG_GPHEN0 = 0x64,
	REG_GPLEN0 = 0x70,
	REG_GPAREN0 = 0x7c,
	REG_GPAFEN0 = 0x88,
	REG_GPPUD = 0x94,
	REG_GPPUDCLK0 = 0x98,
};

/* GPPUD, table 6-28. */
enum {
	PULL_NONE = 0,
	PULL_DOWN = 1,
	PULL_UP = 2,
	PULL_RESERVED = 3,
};

#define PIN_MASK ((UINT64_C(1) << BCM2835_GPIO_NUM_PINS) - 1)
#define FUNCTION_NUM 8

struct Bcm2835Gpio {
	Machine *machine;
	IrqLine *irqs[BCM2835_GPIO_NUM_IRQS];

	uint8_t function[BCM2835_GPIO_NUM_PINS]; /* Bcm2835GpioFunction */
	uint8_t pull[BCM2835_GPIO_NUM_PINS];
	/* What drives each pin from outside, and its alternate functions. */
	uint8_t drive[BCM2835_GPIO_NUM_PINS];
	uint8_t alt_drive[BCM2835_GPIO_NUM_PINS][FUNCTION_NUM];

	/* Bit n of each is pin n. */
	uint64_t latch;  /* the output latch */
	uint64_t levels; /* the level of every pin */
	uint64_t eds, ren, fen, hen, len, aren, afen;

	uint32_t pud;
	uint32_t pudclk[2];
};

/*
 * The pull of each pin after reset, from the table of section 6.2: high
 * for pins 0-8, 34-36 and 46-53; none for 28, 29, 44 and 45.
 */
static uint8_t reset_pull(unsigned pin)
{
	if (pin <= 8 || (pin >= 34 && pin <= 36) || pin >= 46)
		return PULL_UP;
	if (pin == 28 || pin == 29 || pin == 44 || pin == 45)
		return PULL_NONE;
	return PULL_DOWN;
}

static bool pin_level(const Bcm2835Gpio *gpio, unsigned pin)
{
	unsigned function = gpio->function[pin];
	unsigned drive = gpio->drive[pin];

	if (function == BCM2835_GPIO_OUTPUT)
		return (gpio->latch >> pin) & 1;
	if (function != BCM2835_GPIO_INPUT &&
	    gpio->alt_drive[pin][function] != BCM2835_GPIO_UNDRIVEN)
		drive = gpio->alt_drive[pin][function];
	if (drive != BCM2835_GPIO_UNDRIVEN)
		return drive == BCM2835_GPIO_HIGH;
	return gpio->pull[pin] == PULL_UP;
}

static uint64_t all_levels(const Bcm2835Gpio *gpio)
{
	uint64_t levels = 0;

	for (unsigned pin = 0; pin < BCM2835_GPIO_NUM_PINS; pin++)
		levels |= (uint64_t)pin_level(gpio, pin) << pin;
	return levels;
}

/* The interrupt lines follow GPEDSn: banks 0-27, 28-45 and 46-53. */
static void update_irqs(Bcm2835Gpio *gpio)
{
	static const uint64_t banks[BCM2835_GPIO_NUM_IRQS - 1] = {
		(UINT64_C(1) << 28) - 1,
		((UINT64_C(1) << 46) - 1) & ~((UINT64_C(1) << 28) - 1),
		PIN_MASK & ~((UINT64_C(1) << 46) - 1),
	};

	for (unsigned i = 0; i < BCM2835_GPIO_NUM_IRQS - 1; i++)
		irq_set(gpio->irqs[i], (gpio->eds & banks[i]) != 0);
	irq_set(gpio->irqs[BCM2835_GPIO_NUM_IRQS - 1], gpio->eds != 0);
}

/*
 * After anything that can change a level: take the new levels, record
 * the events they make, and drive the interrupt lines.
 */
static void update(Bcm2835Gpio *gpio)
{
	uint64_t levels = all_levels(gpio);
	uint64_t rise = levels & ~gpio->levels;
	uint64_t fall = gpio->levels & ~levels;

	gpio->levels = levels;
	gpio->eds |= ((gpio->ren | gpio->aren) & rise) |
	             ((gpio->fen | gpio->afen) & fall) | (gpio->hen & levels) |
	             (gpio->len & ~levels & PIN_MASK);
	update_irqs(gpio);
}

static void unknown(Bcm2835Gpio *gpio, const char *access, uint64_t offset,
                    unsigned size)
{
	machine_stop(gpio->machine, MACHINE_STOP_ERROR,
	             "bcm2835-gpio: %u-byte %s offset 0x%03" PRIx64
	             " is not described by the datasheet",
	             size, access, offset);
}

/* The half of a pin bitmap that the register at offset holds. */
static uint32_t half(uint64_t bits, uint64_t offset, uint64_t reg0)
{
	return (uint32_t)(offset == reg0 ? bits : bits >> 32);
}

/* Set the half of a pin bitmap that the register at offset holds. */
static void set_half(uint64_t *bits, uint64_t offset, uint64_t reg0,
                     uint32_t value)
{
	if (offset == reg0)
		*bits = (*bits & ~(uint64_t)UINT32_MAX) | value;
	else
		*bits = (*bits & UINT32_MAX) | ((uint64_t)value << 32 & PIN_MASK);
}

/* The bits of a write to the register at offset, as a pin bitmap. */
static uint64_t pins_of(uint64_t offset, uint64_t reg0, uint32_t value)
{
	return offset == reg0 ? value : ((uint64_t)value << 32) & PIN_MASK;
}

/* The pin bitmap, if any, that the register at offset is half of. */
static uint64_t *bitmap(Bcm2835Gpio *gpio, uint64_t offset, uint64_t *reg0)
{
	static const struct {
		uint64_t reg0;
		size_t field;
	} maps[] = {
		{REG_GPEDS0, offsetof(Bcm2835Gpio, eds)},
		{REG_GPREN0, offsetof(Bcm2835Gpio, ren)},
		{REG_GPFEN0, offsetof(Bcm2835Gpio, fen)},
		{REG_GPHEN0, offsetof(Bcm2835Gpio, hen)},
		{REG_GPLEN0, offsetof(Bcm2835Gpio, len)},
		{REG_GPAREN0, offsetof(Bcm2835Gpio, aren)},
		{REG_GPAFEN0, offsetof(Bcm2835Gpio, afen)},
	};

	*reg0 = 0; /* unused without a bitmap; set so that compilers see it */
	for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
		if (offset == maps[i].reg0 || offset == maps[i].reg0 + 4) {
			*reg0 = maps[i].reg0;
			return (uint64_t *)((char *)gpio + maps[i].field);
		}
	}
	return NULL;
}

static uint32_t read_fsel(const Bcm2835Gpio *gpio, unsigned reg)
{
	uint32_t value = 0;

	for (unsigned i = 0; i < 10 && 10 * reg + i < BCM2835_GPIO_NUM_PINS; i++)
		value |= (uint32_t)gpio->function[10 * reg + i] << (3 * i);
	return value;
}

static void write_fsel(Bcm2835Gpio *gpio, unsigned reg, uint32_t value)
{
	for (unsigned i = 0; i < 10 && 10 * reg + i < BCM2835_GPIO_NUM_PINS; i++)
		gpio->function[10 * reg + i] = (value >> (3 * i)) & 7;
}

static uint64_t gpio_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Gpio *gpio = ctx;
	uint64_t reg0;
	uint64_t *bits = bitmap(gpio, offset, &reg0);

	if (size != 4 || (offset & 3) != 0) {
		unknown(gpio, "read from", offset, size);
		return 0;
	}
	if (bits != NULL)
		return half(*bits, offset, reg0);
	if (offset <= REG_GPFSEL0 + 0x14)
		return read_fsel(gpio, (unsigned)(offset / 4));
	switch (offset) {
	case REG_GPLEV0:
	case REG_GPLEV0 + 4:
		return half(gpio->levels, offset, REG_GPLEV0);
	case REG_GPPUD:
		return gpio->pud;
	case REG_GPPUDCLK0:
	case REG_GPPUDCLK0 + 4:
		return gpio->pudclk[(offset - REG_GPPUDCLK0) / 4];
	default:
		unknown(gpio, "read from", offset, size);
		return 0;
	}
}

/* GPPUDCLKn: clock GPPUD into the pins whose bit value sets. */
static void write_pudclk(Bcm2835Gpio *gpio, uint64_t offset, uint32_t value)
{
	uint64_t pins = pins_of(offset, REG_GPPUDCLK0, value);

	gpio->pudclk[(offset - REG_GPPUDCLK0) / 4] =
		offset == REG_GPPUDCLK0 ? value : value & 0x3fffff;
	if (pins == 0)
		return;
	if (gpio->pud == PULL_RESERVED) {
		machine_stop(gpio->machine, MACHINE_STOP_ERROR,
		             "bcm2835-gpio: GPPUD holds the reserved value 3 when "
		             "GPPUDCLK%u clocks it into pins",
		             (unsigned)(offset - REG_GPPUDCLK0) / 4);
		return;
	}
	for (unsigned pin = 0; pin < BCM2835_GPIO_NUM_PINS; pin++)
		if ((pins >> pin) & 1)
			gpio->pull[pin] = (uint8_t)gpio->pud;
}

static void gpio_write(void *ctx, uint64_t offset, uint64_t value,
                       unsigned size)
{
	Bcm2835Gpio *gpio = ctx;
	uint32_t v = (uint32_t)value;
	uint64_t reg0;
	uint64_t *bits = bitmap(gpio, offset, &reg0);

	if (size != 4 || (offset & 3) != 0) {
		unknown(gpio, "write to", offset, size);
		return;
	}
	if (bits == &gpio->eds) {
		gpio->eds &= ~pins_of(offset, reg0, v); /* write 1 to clear */
	} else if (bits != NULL) {
		set_half(bits, offset, reg0, v);
	} else if (offset <= REG_GPFSEL0 + 0x14) {
		write_fsel(gpio, (unsigned)(offset / 4), v);
	} else if (offset == REG_GPSET0 || offset == REG_GPSET0 + 4) {
		gpio->latch |= pins_of(offset, REG_GPSET0, v);
	} else if (offset == REG_GPCLR0 || offset == REG_GPCLR0 + 4) {
		gpio->latch &= ~pins_of(offset, REG_GPCLR0, v);
	} else if (offset == REG_GPPUD) {
		gpio->pud = v & 3;
		return;
	} else if (offset == REG_GPPUDCLK0 || offset == REG_GPPUDCLK0 + 4) {
		write_pudclk(gpio, offset, v);
	} else {
		unknown(gpio, "write to", offset, size);
		return;
	}
	update(gpio);
}

static const BusMmioOps gpio_mmio_ops = {
	.read = gpio_read,
	.write = gpio_write,
};

void bcm2835_gpio_reset(Bcm2835Gpio *gpio)
{
	for (unsigned pin = 0; pin < BCM2835_GPIO_NUM_PINS; pin++) {
		gpio->function[pin] = BCM2835_GPIO_INPUT;
		gpio->pull[pin] = reset_pull(pin);
	}
	gpio->latch = 0;
	gpio->eds = gpio->ren = gpio->fen = gpio->hen = gpio->len = 0;
	gpio->aren = gpio->afen = 0;
	gpio->pud = 0;
	gpio->pudclk[0] = gpio->pudclk[1] = 0;
	gpio->levels = all_levels(gpio);
	update_irqs(gpio);
}

Bcm2835Gpio *bcm2835_gpio_create(Machine *m,
                                 IrqLine *irqs[BCM2835_GPIO_NUM_IRQS])
{
	Bcm2835Gpio *gpio = calloc(1, sizeof(*gpio));

	if (gpio == NULL)
		return NULL;
	gpio->machine = m;
	for (unsigned i = 0; i < BCM2835_GPIO_NUM_IRQS; i++)
		gpio->irqs[i] = irqs[i];
	bcm2835_gpio_reset(gpio);
	return gpio;
}

void bcm2835_gpio_destroy(Bcm2835Gpio *gpio)
{
	free(gpio);
}

BusStatus bcm2835_gpio_map(Bcm2835Gpio *gpio, Bus *bus, const char *name,
                           uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_GPIO_MMIO_SIZE, &gpio_mmio_ops,
	                    gpio);
}

Bcm2835GpioFunction bcm2835_gpio_function(const Bcm2835Gpio *gpio, unsigned pin)
{
	return (Bcm2835GpioFunction)gpio->function[pin];
}

void bcm2835_gpio_set_function(Bcm2835Gpio *gpio, unsigned pin,
                               Bcm2835GpioFunction function)
{
	gpio->function[pin] = (uint8_t)function;
	update(gpio);
}

bool bcm2835_gpio_level(const Bcm2835Gpio *gpio, unsigned pin)
{
	return (gpio->levels >> pin) & 1;
}

void bcm2835_gpio_drive(Bcm2835Gpio *gpio, unsigned pin, Bcm2835GpioDrive drive)
{
	gpio->drive[pin] = (uint8_t)drive;
	update(gpio);
}

void bcm2835_gpio_drive_alt(Bcm2835Gpio *gpio, unsigned pin,
                            Bcm2835GpioFunction function,
                            Bcm2835GpioDrive drive)
{
	gpio->alt_drive[pin][function] = (uint8_t)drive;
	update(gpio);
}
