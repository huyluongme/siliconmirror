/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */
/*
 * A BSC (Broadcom Serial Controller), the BCM2835's I2C master: BSC0 at
 * 0x7E205000, BSC1 at 0x7E804000 and BSC2, the HDMI's DDC, at
 * 0x7E805000 (BCM2835 ARM Peripherals, chapter 3; the fractions that
 * the 2012 text lost come from the BCM2711 ARM Peripherals, whose BSC is
 * the same). Eight 32-bit registers:
 *   C, 0x0: I2CEN, the interrupts INTR, INTT and INTD, ST, CLEAR, READ.
 *   S, 0x4: TA, DONE, TXW, RXR, TXD, RXD, TXE, RXF, ERR, CLKT.
 *   DLEN, 0x8; A, 0xc, the slave's 7-bit address; FIFO, 0x10, 16 bytes;
 *   DIV, 0x14, SCL = core clock / CDIV; DEL, 0x18; CLKT, 0x1c.
 *
 * Nothing is attached to the buses: no device on the GPIO header, no
 * camera or display on BSC0, and no HDMI display, so no EDID, on BSC2.
 * Every transfer is a START and the address byte that no slave
 * acknowledges: after nine SCL clocks (the address, R/W and the ACK
 * clock) and a STOP, smirror's ten, the transfer ends with ERR and DONE
 * set, TA clear, and the FIFO and DLEN as they were: no byte moved.
 * During it, TXW is set in a write while the FIFO holds less than 4
 * bytes (a quarter) and less than DLEN, and RXR in a read while it
 * holds 12 or more (three quarters). The interrupt is raised while
 * INTD and DONE, INTT and TXW, or INTR and RXR are both set.
 *
 * smirror's choices, where the datasheet is silent: DIV reads back as
 * written (the controller divides by CDIV rounded down to even, 32768
 * for 0); DLEN reads, with TA or DONE set, the bytes left, here all of
 * them; ST without I2CEN does nothing ("transfers will not be
 * performed").
 *
 * Not modelled, and the machine stops when software does it: accesses
 * other than 32-bit ones ("All accesses are assumed to be 32-bit"); a
 * 1 written to a reserved bit; ST or CLEAR while a transfer is active
 * (CLEAR then aborts it); a write of DLEN or A then; a read of the FIFO
 * when it is empty ("invalid data").
 */

#ifndef HW_I2C_BCM2835_BSC_H
#define HW_I2C_BCM2835_BSC_H

#include <stdint.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"

#define BCM2835_BSC_MMIO_SIZE 0x20 /* the eight registers */
#define BCM2835_BSC_FIFO_SIZE 16

typedef struct Bcm2835Bsc Bcm2835Bsc;

/*
 * A controller at reset, named name in messages, its SCL derived from a
 * core clock of core_hz, raising irq (which may be NULL). NULL when
 * core_hz is 0 or memory runs out.
 */
Bcm2835Bsc *bcm2835_bsc_create(Machine *m, const char *name, uint64_t core_hz,
                               IrqLine *irq);
void bcm2835_bsc_destroy(Bcm2835Bsc *bsc);

/* A reset of the board: the registers' reset values, no transfer. */
void bcm2835_bsc_reset(Bcm2835Bsc *bsc);

/*
 * Map the eight registers on bus at base. The device tree gives BSC0
 * 0x200 bytes and the others 0x1000: what lies past the registers is not
 * documented, so it is left unmapped.
 */
BusStatus bcm2835_bsc_map(Bcm2835Bsc *bsc, Bus *bus, const char *name,
                          uint64_t base);

#endif /* HW_I2C_BCM2835_BSC_H */
