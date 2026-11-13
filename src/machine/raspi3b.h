/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Raspberry Pi 3 Model B. See docs/boards/raspi3b.md. */

#ifndef MACHINE_RASPI3B_H
#define MACHINE_RASPI3B_H

#include "machine/board.h"

/* Memory map, as the Arm cores see it. */
#define RASPI3B_RAM_SIZE 0x3f000000 /* RAM from 0 up to the peripherals */
#define RASPI3B_PERIPHERAL_BASE 0x3f000000 /* bus address 0x7e000000 */
#define RASPI3B_IC_BASE (RASPI3B_PERIPHERAL_BASE + 0xb200)   /* interrupts */
#define RASPI3B_MBOX_BASE (RASPI3B_PERIPHERAL_BASE + 0xb880) /* mailboxes */
#define RASPI3B_PM_BASE (RASPI3B_PERIPHERAL_BASE + 0x100000) /* watchdog */
#define RASPI3B_GPIO_BASE (RASPI3B_PERIPHERAL_BASE + 0x200000)
#define RASPI3B_UART0_BASE (RASPI3B_PERIPHERAL_BASE + 0x201000)
#define RASPI3B_SDHOST_BASE (RASPI3B_PERIPHERAL_BASE + 0x202000)
#define RASPI3B_AUX_BASE (RASPI3B_PERIPHERAL_BASE + 0x215000)  /* mini UART */
#define RASPI3B_EMMC_BASE (RASPI3B_PERIPHERAL_BASE + 0x300000) /* Arasan */
#define RASPI3B_USB_BASE (RASPI3B_PERIPHERAL_BASE + 0x980000)  /* DWC2 */
#define RASPI3B_LOCAL_BASE 0x40000000 /* the ARM local peripherals */

#define RASPI3B_NUM_CPUS 4

/*
 * The core (VPU) clock, of the mini UART and SDHOST: with enable_uart=1,
 * the firmware keeps it at 250 MHz (Raspberry Pi documentation,
 * "Configure UARTs"); smirror always does.
 */
#define RASPI3B_CORE_CLOCK_HZ 250000000

/*
 * The EMMC clock of the Arasan controller: 200 MHz, which the firmware
 * sets from its commit 7fdcd00e on (U-Boot's bcm2835_sdhci.c history).
 */
#define RASPI3B_EMMC_CLOCK_HZ 200000000

/* Where the firmware loads a 64-bit kernel, and boots core 0. */
#define RASPI3B_KERNEL_ADDRESS 0x80000

/*
 * The boot stub, raspi3b_stub.s, which the cores run from reset at EL3.
 * It ends with data at the offsets of the firmware's armstub8:
 *
 * - The spin table: core n waits for an address to jump to in the
 *   64-bit word at RASPI3B_SPIN_TABLE + 8 * n (cpu-release-addr in
 *   Linux's bcm2837.dtsi).
 * - At RASPI3B_STUB_MAGIC, a magic number and a version, which overlap
 *   the spin table entry of core 3; the board clears them.
 * - The 32-bit addresses of the device tree and of the kernel, which the
 *   board writes and core 0 reads.
 */
#define RASPI3B_STUB_ADDRESS 0x0 /* RVBAR_EL3, where the cores start */
#define RASPI3B_STUB_SIZE 0x100
#define RASPI3B_SPIN_TABLE 0xd8
#define RASPI3B_STUB_MAGIC 0xf0
#define RASPI3B_STUB_DTB 0xf8
#define RASPI3B_STUB_KERNEL 0xfc

extern const Board raspi3b_board;

#endif /* MACHINE_RASPI3B_H */
