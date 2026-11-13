/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835's External Mass Media Controller (EMMC), an Arasan SD host
 * controller, from chapter 5 of the BCM2835 ARM Peripherals datasheet.
 *
 * On the Raspberry Pi 3 Model B it drives the WiFi chip, a BCM43438 that
 * is an SDIO card only (bcm2710-rpi-3-b.dts: mmcnr, non-removable). The
 * chip is not modelled. It does not answer the commands that set up a
 * memory card, so neither does the slot here: such a command ends with a
 * command timeout, after its 48 bits and the 64 clocks of Ncr. An SDIO
 * command (CMD5, CMD52, CMD53), which the chip would answer, stops the
 * machine.
 *
 * The registers are 32 bits wide, and accessed so: other sizes stop the
 * machine. The datasheet does not list the capability registers at 0x40
 * and 0x44, which drivers read: they hold the values that Linux's
 * sdhci-iproc.c gives this controller (blocks of 1024 bytes, 3.3 V, high
 * speed; driver types A and C). SDVERSION reads 2, SD Host Controller
 * 3.00, the version whose 10-bit clock divider and UHS mode bits the
 * datasheet describes; VENDOR reads 0, its documented reset value.
 */

#ifndef HW_SD_BCM2835_EMMC_H
#define HW_SD_BCM2835_EMMC_H

#include <stdint.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"

#define BCM2835_EMMC_MMIO_SIZE 0x100

typedef struct Bcm2835Emmc Bcm2835Emmc;

/*
 * A controller whose base clock runs at base_hz, the firmware's EMMC
 * clock. irq is GPU interrupt 62. NULL when base_hz is 0 or memory runs
 * out.
 */
Bcm2835Emmc *bcm2835_emmc_create(Machine *m, uint64_t base_hz, IrqLine *irq);
void bcm2835_emmc_destroy(Bcm2835Emmc *emmc);
void bcm2835_emmc_reset(Bcm2835Emmc *emmc);
BusStatus bcm2835_emmc_map(Bcm2835Emmc *emmc, Bus *bus, const char *name,
                           uint64_t base);

#endif /* HW_SD_BCM2835_EMMC_H */
