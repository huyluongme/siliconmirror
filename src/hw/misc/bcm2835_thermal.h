/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835's temperature sensor at 0x7E212000. Broadcom does not
 * document it; its two registers behave here as Linux's
 * bcm2835_thermal.c uses them. TSENSCTL, at 0x0, the firmware sets up at
 * boot, as the driver says ("right now the FW does set up the
 * HW-block"). TSENSSTAT, at 0x4, holds VALID in bit 10 and the ADC's
 * value in bits 9:0, which Linux turns into a temperature with the
 * coefficients of the board's device tree: 412000 - 538 x ADC, in
 * thousandths of a degree Celsius.
 *
 * smirror's choices, where nothing documents the block: TSENSCTL holds
 * at boot what the driver would write itself, "using 'sane' values used
 * by the firmware right now": CTRL 1 (bits 4:2), the critical
 * temperature's ADC value, 561 for 110 C (bits 17:8), a reset delay of
 * 0xfe (bits 25:18), REGULEN (bit 26) and RSTB (bit 1). The board stays
 * at one temperature, that of ADC 682: 45.084 C, as a Pi 3 at rest may
 * be.
 *
 * Writes stop the machine: the driver makes them only when the firmware
 * has not set the sensor up.
 */

#ifndef HW_MISC_BCM2835_THERMAL_H
#define HW_MISC_BCM2835_THERMAL_H

#include "core/bus.h"
#include "core/machine.h"

#define BCM2835_THERMAL_MMIO_SIZE 0x8 /* the device tree's */
#define BCM2835_THERMAL_CTL_BOOT 0x07fa3106u
#define BCM2835_THERMAL_ADC 682

typedef struct Bcm2835Thermal Bcm2835Thermal;

/* The sensor as the firmware leaves it. NULL when memory runs out. */
Bcm2835Thermal *bcm2835_thermal_create(Machine *m);
void bcm2835_thermal_destroy(Bcm2835Thermal *t);

/* Map the registers on bus at base. */
BusStatus bcm2835_thermal_map(Bcm2835Thermal *t, Bus *bus, const char *name,
                              uint64_t base);

#endif /* HW_MISC_BCM2835_THERMAL_H */
