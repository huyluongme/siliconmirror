/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */
/*
 * The BCM2835's HVS (Hardware Video Scaler) at 0x7E400000, which
 * composes the planes of up to three displays. Broadcom does not
 * document it; its registers behave here as Linux's vc4 driver
 * (drivers/gpu/drm/vc4, vc4_regs.h and vc4_hvs.c of Raspberry Pi's
 * Linux, at cff533a) uses them on a board with no display attached:
 *   DISPCTRL, 0x00: the enable, the AXI panic levels, the output muxes
 *     and the interrupt enables.
 *   DISPSTAT, 0x04: the status and interrupts, written 1 to clear.
 *   DISPECTRL 0x0c, DISPDITHER 0x14, DISPEOLN 0x18: the muxes of
 *     outputs 2, 5 and 4 in their top bits.
 *   DISPBASE0 to 2, 0x4c, 0x5c and 0x6c: each channel's share of the
 *     composite output buffer, top in bits 31:16, base in 15:0.
 *   0x2000 to 0x5fff: the display list memory, 4096 words, where the
 *     driver puts the display lists and the scaling filters' kernels.
 *
 * At bind, vc4_hvs_hw_init enables the HVS and sets the muxes, the
 * panic levels and the interrupts, the filter kernels go into the
 * display list memory, and vc4_hvs_cob_init shares the buffer out.
 * With no display, no channel runs: nothing is composed, DISPSTAT stays
 * 0, and the HVS's interrupt (GPU interrupt 33) never comes. smirror
 * keeps the values; it shows no picture.
 *
 * smirror's choices, where nothing documents the block: the registers
 * and the memory read 0 after a reset (what the firmware leaves there
 * when it hands the display over is not documented), and DISPSTAT
 * ignores what is written to it, as none of its bits is ever set.
 *
 * Not modelled, and the machine stops when software uses them: accesses
 * other than 32-bit ones, and the other registers, which a running
 * channel needs (DISPLISTx, DISPCTRLx, DISPBKGNDx, DISPSTATx, the gamma
 * RAM) or no driver uses on this board (DISPID, DISPPROF).
 */

#ifndef HW_DISPLAY_BCM2835_HVS_H
#define HW_DISPLAY_BCM2835_HVS_H

#include "core/bus.h"
#include "core/machine.h"

#define BCM2835_HVS_MMIO_SIZE 0x6000 /* the device tree's */

typedef struct Bcm2835Hvs Bcm2835Hvs;

/* The HVS at reset. NULL when memory runs out. */
Bcm2835Hvs *bcm2835_hvs_create(Machine *m);
void bcm2835_hvs_destroy(Bcm2835Hvs *hvs);

/* A reset of the board: the registers and the memory read 0. */
void bcm2835_hvs_reset(Bcm2835Hvs *hvs);

/* Map the registers and the display list memory on bus at base. */
BusStatus bcm2835_hvs_map(Bcm2835Hvs *hvs, Bus *bus, const char *name,
                          uint64_t base);

#endif /* HW_DISPLAY_BCM2835_HVS_H */
