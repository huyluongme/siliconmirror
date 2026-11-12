/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The GPIO block of the BCM2835: 54 pins. Written from chapter 6 of
 * Broadcom's "BCM2835 ARM Peripherals" (2012), and section 5.1 of
 * Raspberry Pi's "BCM2711 ARM Peripherals" (2022) for the banks and the
 * interrupt lines.
 *
 * Each pin has a function (GPFSELn), an output latch (GPSETn, GPCLRn)
 * and a pull: none, down or up, which GPPUD and GPPUDCLKn change. Its
 * level, in GPLEVn, is:
 *  - for an output, the latch;
 *  - for an alternate function whose peripheral drives the pin, what
 *    the board says that peripheral drives (a UART's TX line is high
 *    while idle);
 *  - otherwise, what drives the pin from outside the chip, which the
 *    board says too, else its pull. A pin that nothing drives and that
 *    has no pull floats; this model reads it as 0.
 *
 * Edges and levels set GPEDSn as GPRENn, GPFENn, GPHENn, GPLENn, GPARENn
 * and GPAFENn ask. There are three banks, pins 0 to 27, 28 to 45 and 46
 * to 53, each with an interrupt line that is high while one of its pins
 * has its GPEDSn bit set, and a fourth line for every pin.
 *
 * Accesses other than 32-bit ones, to the test register, to reserved
 * offsets, writes of GPLEVn and reads of GPSETn and GPCLRn, which the
 * document does not describe, stop the machine. So does clocking the
 * reserved GPPUD value 3 into a pin.
 */

#ifndef HW_GPIO_BCM2835_GPIO_H
#define HW_GPIO_BCM2835_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"

/* The register block, up to the PL011 at offset 0x1000. */
#define BCM2835_GPIO_MMIO_SIZE 0x1000

#define BCM2835_GPIO_NUM_PINS 54

/* The interrupt lines: one for each bank, then one for all the pins. */
#define BCM2835_GPIO_NUM_IRQS 4

/* The functions of a pin, as GPFSELn encodes them. */
typedef enum Bcm2835GpioFunction {
	BCM2835_GPIO_INPUT = 0,
	BCM2835_GPIO_OUTPUT = 1,
	BCM2835_GPIO_ALT5 = 2,
	BCM2835_GPIO_ALT4 = 3,
	BCM2835_GPIO_ALT0 = 4,
	BCM2835_GPIO_ALT1 = 5,
	BCM2835_GPIO_ALT2 = 6,
	BCM2835_GPIO_ALT3 = 7,
} Bcm2835GpioFunction;

/* What drives a pin from outside the chip, or an alternate function. */
typedef enum Bcm2835GpioDrive {
	BCM2835_GPIO_UNDRIVEN,
	BCM2835_GPIO_LOW,
	BCM2835_GPIO_HIGH,
} Bcm2835GpioDrive;

typedef struct Bcm2835Gpio Bcm2835Gpio;

/*
 * Create the block in its reset state, which stops m on what it does
 * not emulate. irqs are its interrupt outputs, each NULL when not
 * connected. Returns NULL when memory runs out.
 */
Bcm2835Gpio *bcm2835_gpio_create(Machine *m,
                                 IrqLine *irqs[BCM2835_GPIO_NUM_IRQS]);
void bcm2835_gpio_destroy(Bcm2835Gpio *gpio);

/*
 * Put the block back in its reset state: every pin an input with the
 * pull that the table of section 6.2 gives it.
 */
void bcm2835_gpio_reset(Bcm2835Gpio *gpio);

/* Map the registers on bus at base. */
BusStatus bcm2835_gpio_map(Bcm2835Gpio *gpio, Bus *bus, const char *name,
                           uint64_t base);

/* The function of a pin, and setting it as a write of GPFSELn would. */
Bcm2835GpioFunction bcm2835_gpio_function(const Bcm2835Gpio *gpio,
                                          unsigned pin);
void bcm2835_gpio_set_function(Bcm2835Gpio *gpio, unsigned pin,
                               Bcm2835GpioFunction function);

/* The level of a pin, as GPLEVn reads it. */
bool bcm2835_gpio_level(const Bcm2835Gpio *gpio, unsigned pin);

/* What drives a pin from outside the chip. */
void bcm2835_gpio_drive(Bcm2835Gpio *gpio, unsigned pin,
                        Bcm2835GpioDrive drive);

/* What the peripheral of an alternate function drives a pin to. */
void bcm2835_gpio_drive_alt(Bcm2835Gpio *gpio, unsigned pin,
                            Bcm2835GpioFunction function,
                            Bcm2835GpioDrive drive);

#endif /* HW_GPIO_BCM2835_GPIO_H */
