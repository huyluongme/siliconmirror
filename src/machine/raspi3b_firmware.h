/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * What the VideoCore firmware does on a Raspberry Pi 3 Model B before
 * the Arm cores start, done by smirror instead of running the
 * firmware (high-level emulation): see docs/boards/raspi3b.md, "Boot from
 * the SD card".
 *
 * From the SD card: the first partition must be a FAT file system with
 * bootcode.bin and the start file; config.txt and cmdline.txt set up the
 * memory split, the console, the kernel, its initramfs and its device
 * tree, which gets the board's values. With -kernel, the image is the
 * kernel, and the settings are the defaults with enable_uart=1.
 *
 * Either way the GPIO pins are set as the firmware's dt-blob.dts sets
 * them for the Pi 3 Model B rev 1.2, the boot stub goes to address 0 and
 * the cores start in it.
 */

#ifndef MACHINE_RASPI3B_FIRMWARE_H
#define MACHINE_RASPI3B_FIRMWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/block.h"
#include "core/machine.h"
#include "hw/gpio/bcm2835_gpio.h"
#include "hw/misc/bcm2835_pm.h"
#include "machine/raspi3b_property.h"

typedef struct Raspi3bFirmwareConfig {
	Machine *machine;
	Bcm2835Gpio *gpio;
	Raspi3bProperty *property;
	Bcm2835Pm *pm;      /* its RSTS gives the partition, or NULL for 0 */
	const char *kernel; /* -kernel: an image on the host, or NULL */
	BlockDevice *card;  /* the SD card, or NULL */

	/* Messages about the boot that do not stop it; warn may be NULL. */
	void (*warn)(void *ctx, const char *message);
	void *warn_ctx;
} Raspi3bFirmwareConfig;

/*
 * Boot the board: set it up and load the kernel, as the firmware does.
 * On failure, which would leave a real board stuck before its Arm cores
 * start, writes a message to error and returns false.
 */
bool raspi3b_firmware_boot(const Raspi3bFirmwareConfig *config, char *error,
                           size_t error_size);

#endif /* MACHINE_RASPI3B_FIRMWARE_H */
