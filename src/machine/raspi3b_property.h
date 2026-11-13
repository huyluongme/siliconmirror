/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The firmware's answers on the property channel of the mailboxes, as
 * the Raspberry Pi 3 Model B gives them. The Arm writes a buffer of tags
 * to memory and sends its address on channel 8; the firmware writes its
 * answers into the buffer and sends the address back.
 *
 * Written from the Raspberry Pi firmware wiki, "Mailbox property
 * interface": the buffer format, the tags and their values. Tags that
 * neither it nor Linux's include/soc/bcm2835/raspberrypi-firmware.h
 * names are unknown to the firmware, and left unanswered as the wiki
 * says. A known tag that is not emulated stops the machine, as do the
 * values the wiki leaves to the firmware and nothing documents for this
 * board (the state of a clock or of a power domain at boot, the rates of
 * the EMMC, PIXEL and PWM clocks, ...).
 *
 * Answered: the firmware revision, the board revision, MAC address and
 * serial number, the memory split, the command line, power on and off,
 * the rates of the clocks and turbo. Clock rates come from the Raspberry
 * Pi documentation (config.txt, overclocking options: the defaults of
 * the Raspberry Pi 3). Turning a power domain on or off and changing the
 * Arm's clock change nothing else in the emulation.
 */

#ifndef MACHINE_RASPI3B_PROPERTY_H
#define MACHINE_RASPI3B_PROPERTY_H

#include <stdbool.h>
#include <stdint.h>

#include "core/machine.h"

#define RASPI3B_PROPERTY_CHANNEL 8 /* property tags, Arm to VideoCore */

/* Board revision code: Pi 3 Model B, 1 GB, Sony UK, revision 1.2. */
#define RASPI3B_BOARD_REVISION 0xa02082

/* Our board's serial number; the MAC address ends with its low 24 bits. */
#define RASPI3B_BOARD_SERIAL 0x00000000c0ffee03ull

#define RASPI3B_TOTAL_MEM_MB 1024 /* the SDRAM of a Pi 3 Model B */
#define RASPI3B_GPU_MEM_MB 76     /* gpu_mem by default with 1 GB */

/* What config.txt decides of the answers. */
typedef struct Raspi3bVcSettings {
	uint32_t total_mem_mb; /* total_mem: the SDRAM in use */
	uint32_t gpu_mem_mb;   /* gpu_mem: the GPU's share, at the top */

	/*
	 * enable_uart=1, which implies core_freq=250 (Raspberry Pi
	 * documentation, "Device Trees, overlays, and parameters"): the core
	 * clock stays at 250 MHz. Otherwise it may run up to 400 MHz.
	 */
	bool core_freq_fixed;

	const char *cmdline; /* the kernel's command line; copied */
} Raspi3bVcSettings;

typedef struct Raspi3bProperty Raspi3bProperty;

/*
 * Create the firmware's property service of m, with the default
 * settings: those of a config.txt without settings, and an empty command
 * line. Buffers must be in the first ram_size bytes of m's bus. Returns
 * NULL when memory runs out.
 */
Raspi3bProperty *raspi3b_property_create(Machine *m, uint32_t ram_size);
void raspi3b_property_destroy(Raspi3bProperty *p);

/* Answer with settings from now on. Returns false when memory runs out. */
bool raspi3b_property_configure(Raspi3bProperty *p,
                                const Raspi3bVcSettings *settings);

/* The firmware starts again: clocks, turbo and power as at boot. */
void raspi3b_property_reset(Raspi3bProperty *p);

/* The memory of the Arm: from 0, this many bytes. */
uint32_t raspi3b_property_arm_memory(const Raspi3bProperty *p);

/* The MAC address of the board's Ethernet. */
void raspi3b_property_mac(uint8_t mac[6]);

/*
 * The VideoCore's end of the mailboxes (Bcm2835MboxVc.receive): answers
 * a message on the property channel; others stop the machine.
 */
bool raspi3b_property_receive(void *ctx, uint32_t message, uint32_t *reply);

#endif /* MACHINE_RASPI3B_PROPERTY_H */
