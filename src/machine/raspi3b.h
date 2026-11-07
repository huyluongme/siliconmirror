/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Raspberry Pi 3 Model B. See docs/boards/raspi3b.md. */

#ifndef MACHINE_RASPI3B_H
#define MACHINE_RASPI3B_H

#include "machine/board.h"

/* Memory map, as the Arm cores see it. */
#define RASPI3B_RAM_SIZE 0x3f000000 /* RAM from 0 up to the peripherals */
#define RASPI3B_PERIPHERAL_BASE 0x3f000000 /* bus address 0x7e000000 */
#define RASPI3B_UART0_BASE (RASPI3B_PERIPHERAL_BASE + 0x201000)

#define RASPI3B_NUM_CPUS 4

/* Where the firmware loads a 64-bit kernel, and boots core 0. */
#define RASPI3B_KERNEL_ADDRESS 0x80000

/*
 * The spin table: core n waits for an address to jump to in the 64-bit
 * word at RASPI3B_SPIN_TABLE + 8 * n (cpu-release-addr in Linux's
 * bcm2837.dtsi). The loop that waits lives at RASPI3B_SPIN_LOOP.
 */
#define RASPI3B_SPIN_TABLE 0xd8
#define RASPI3B_SPIN_LOOP 0x0

extern const Board raspi3b_board;

#endif /* MACHINE_RASPI3B_H */
