/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */
/* The BCM2835's BSC, its I2C master, with nothing on its bus; see the .h. */

#include "hw/i2c/bcm2835_bsc.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "core/event.h"
#include "util/muldiv.h"

enum {
	REG_C = 0x00,
	REG_S = 0x04,
	REG_DLEN = 0x08,
	REG_A = 0x0c,
	REG_FIFO = 0x10,
	REG_DIV = 0x14,
	REG_DEL = 0x18,
	REG_CLKT = 0x1c,
};

/* C */
#define C_READ 0x0001u
#define C_CLEAR 0x0030u /* either bit clears the FIFO */
#define C_ST 0x0080u
#define C_INTD 0x0100u
#define C_INTT 0x0200u
#define C_INTR 0x0400u
#define C_I2CEN 0x8000u
#define C_KEPT (C_I2CEN | C_INTR | C_INTT | C_INTD | C_READ) /* read back */
#define C_BITS (C_KEPT | C_ST | C_CLEAR)

/* S */
#define S_TA 0x001u
#define S_DONE 0x002u
#define S_TXW 0x004u
#define S_RXR 0x008u
#define S_TXD 0x010u
#define S_RXD 0x020u
#define S_TXE 0x040u
#define S_RXF 0x080u
#define S_ERR 0x100u
#define S_CLKT 0x200u
#define S_W1C (S_DONE | S_ERR | S_CLKT)
#define S_BITS 0x3ffu

#define DLEN_BITS 0xffffu
#define A_BITS 0x7fu
#define FIFO_BITS 0xffu
#define DIV_BITS 0xffffu
#define CLKT_BITS 0xffffu

/* The reset values: 100 kHz from a 150 MHz core clock, the delays. */
#define DIV_RESET 0x5dcu
#define DEL_RESET 0x00300030u
#define CLKT_RESET 0x40u

#define CDIV_ZERO 32768u /* "If CDIV is set to 0, the divisor is 32768" */
#define NAK_CLOCKS 10u   /* the address byte, its ACK clock, a STOP */
#define TXW_BELOW 4u     /* a quarter of the FIFO */
#define RXR_FROM 12u     /* three quarters */
#define NS_PER_S 1000000000u

struct Bcm2835Bsc {
	Machine *machine;
	const char *name;
	uint64_t core_hz;
	IrqLine *irq;
	Event done; /* the end of the transfer */

	uint32_t c;    /* the bits of C_KEPT */
	uint32_t s;    /* the bits of S_W1C, and S_TA */
	uint32_t dlen; /* as written */
	uint32_t a;
	uint32_t div;
	uint32_t del;
	uint32_t clkt;
	uint8_t fifo[BCM2835_BSC_FIFO_SIZE];
	unsigned head, count; /* the FIFO's oldest byte, and how many */
};

static void stop(Bcm2835Bsc *bsc, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(bsc->machine, MACHINE_STOP_ERROR,
	             "%s: %u-byte %s offset 0x%02" PRIx64 " %s", bsc->name, size,
	             access, offset, why);
}

static uint32_t status(const Bcm2835Bsc *bsc)
{
	uint32_t s = bsc->s;
	bool active = s & S_TA;

	if (active && !(bsc->c & C_READ) && bsc->count < TXW_BELOW &&
	    bsc->count < bsc->dlen)
		s |= S_TXW;
	if (active && (bsc->c & C_READ) && bsc->count >= RXR_FROM)
		s |= S_RXR;
	if (bsc->count < BCM2835_BSC_FIFO_SIZE)
		s |= S_TXD;
	if (bsc->count > 0)
		s |= S_RXD;
	if (bsc->count == 0)
		s |= S_TXE;
	if (bsc->count == BCM2835_BSC_FIFO_SIZE)
		s |= S_RXF;
	return s;
}

static void update_irq(Bcm2835Bsc *bsc)
{
	uint32_t s = status(bsc);

	irq_set(bsc->irq, ((bsc->c & C_INTD) && (s & S_DONE)) ||
	                      ((bsc->c & C_INTT) && (s & S_TXW)) ||
	                      ((bsc->c & C_INTR) && (s & S_RXR)));
}

/* No slave acknowledged the address: ERR, then the STOP, DONE. */
static void transfer_done(void *ctx)
{
	Bcm2835Bsc *bsc = ctx;

	bsc->s = (bsc->s & ~S_TA) | S_ERR | S_DONE;
	update_irq(bsc);
}

static void start(Bcm2835Bsc *bsc)
{
	uint64_t cdiv = bsc->div & ~1u;

	if (cdiv == 0)
		cdiv = CDIV_ZERO;
	bsc->s |= S_TA;
	event_schedule(&bsc->machine->events, &bsc->done,
	               machine_now(bsc->machine) +
	                   mul_div_ceil(NAK_CLOCKS * cdiv, NS_PER_S, bsc->core_hz));
}

static uint64_t bsc_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Bsc *bsc = ctx;
	uint8_t byte;

	if (size != 4 || offset % 4 != 0) {
		stop(bsc, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	switch (offset) {
	case REG_C:
		return bsc->c;
	case REG_S:
		return status(bsc);
	case REG_DLEN:
		/* With TA or DONE, the bytes left: no byte ever moves. */
		return bsc->dlen;
	case REG_A:
		return bsc->a;
	case REG_FIFO:
		if (bsc->count == 0) {
			stop(bsc, "read of", offset, size,
			     "is not emulated: the FIFO is empty, which gives "
			     "invalid data");
			return 0;
		}
		byte = bsc->fifo[bsc->head];
		bsc->head = (bsc->head + 1) % BCM2835_BSC_FIFO_SIZE;
		bsc->count--;
		update_irq(bsc);
		return byte;
	case REG_DIV:
		return bsc->div;
	case REG_DEL:
		return bsc->del;
	default: /* REG_CLKT: the map holds only the eight registers */
		return bsc->clkt;
	}
}

/* Whether v has bits outside the register's; stops the machine if so. */
static bool reserved(Bcm2835Bsc *bsc, uint64_t offset, uint32_t v,
                     uint32_t bits)
{
	if ((v & ~bits) == 0)
		return false;
	stop(bsc, "write to", offset, 4,
	     "is not emulated: a 1 in a reserved bit, which is written as 0");
	return true;
}

static void write_c(Bcm2835Bsc *bsc, uint32_t v)
{
	bool active = bsc->s & S_TA;

	if (reserved(bsc, REG_C, v, C_BITS))
		return;
	if (active && (v & (C_ST | C_CLEAR))) {
		stop(bsc, "write to", REG_C, 4,
		     "is not emulated: ST or CLEAR while a transfer is active");
		return;
	}
	bsc->c = v & C_KEPT;
	if (v & C_CLEAR)
		bsc->head = bsc->count = 0;
	if ((v & C_ST) && (v & C_I2CEN))
		start(bsc);
	update_irq(bsc);
}

static void bsc_write(void *ctx, uint64_t offset, uint64_t value, unsigned size)
{
	Bcm2835Bsc *bsc = ctx;
	uint32_t v = (uint32_t)value;
	bool active = bsc->s & S_TA;

	if (size != 4 || offset % 4 != 0) {
		stop(bsc, "write to", offset, size, "is not a 32-bit register write");
		return;
	}
	switch (offset) {
	case REG_C:
		write_c(bsc, v);
		return;
	case REG_S:
		if (reserved(bsc, offset, v, S_BITS))
			return;
		bsc->s &= ~(v & S_W1C);
		update_irq(bsc);
		return;
	case REG_DLEN:
	case REG_A:
		if (reserved(bsc, offset, v, offset == REG_A ? A_BITS : DLEN_BITS))
			return;
		if (active) {
			stop(bsc, "write to", offset, size,
			     "is not emulated: DLEN or A while a transfer is active");
			return;
		}
		if (offset == REG_A)
			bsc->a = v;
		else
			bsc->dlen = v;
		return;
	case REG_FIFO:
		if (reserved(bsc, offset, v, FIFO_BITS))
			return;
		/* "Any writes to a full TX FIFO will be ignored." */
		if (bsc->count < BCM2835_BSC_FIFO_SIZE) {
			bsc->fifo[(bsc->head + bsc->count) % BCM2835_BSC_FIFO_SIZE] =
				(uint8_t)v;
			bsc->count++;
		}
		update_irq(bsc);
		return;
	case REG_DIV:
		if (!reserved(bsc, offset, v, DIV_BITS))
			bsc->div = v;
		return;
	case REG_DEL:
		bsc->del = v; /* FEDL and REDL fill the 32 bits */
		return;
	default: /* REG_CLKT */
		if (!reserved(bsc, offset, v, CLKT_BITS))
			bsc->clkt = v;
		return;
	}
}

static const BusMmioOps bsc_mmio_ops = {
	.read = bsc_read,
	.write = bsc_write,
};

Bcm2835Bsc *bcm2835_bsc_create(Machine *m, const char *name, uint64_t core_hz,
                               IrqLine *irq)
{
	Bcm2835Bsc *bsc;

	if (core_hz == 0)
		return NULL;
	bsc = calloc(1, sizeof(*bsc));
	if (bsc == NULL)
		return NULL;
	bsc->machine = m;
	bsc->name = name;
	bsc->core_hz = core_hz;
	bsc->irq = irq;
	event_init(&bsc->done, transfer_done, bsc);
	bcm2835_bsc_reset(bsc);
	return bsc;
}

void bcm2835_bsc_destroy(Bcm2835Bsc *bsc)
{
	if (bsc == NULL)
		return;
	event_cancel(&bsc->machine->events, &bsc->done);
	free(bsc);
}

void bcm2835_bsc_reset(Bcm2835Bsc *bsc)
{
	event_cancel(&bsc->machine->events, &bsc->done);
	bsc->c = 0;
	bsc->s = 0;
	bsc->dlen = 0;
	bsc->a = 0;
	bsc->div = DIV_RESET;
	bsc->del = DEL_RESET;
	bsc->clkt = CLKT_RESET;
	bsc->head = bsc->count = 0;
	update_irq(bsc);
}

BusStatus bcm2835_bsc_map(Bcm2835Bsc *bsc, Bus *bus, const char *name,
                          uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_BSC_MMIO_SIZE, &bsc_mmio_ops,
	                    bsc);
}
