/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The DWC2 USB controller, as a host; see dwc2.h. */

#include "hw/usb/dwc2.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Global registers. */
enum {
	GOTGCTL = 0x000,
	GOTGINT = 0x004,
	GAHBCFG = 0x008,
	GUSBCFG = 0x00c,
	GRSTCTL = 0x010,
	GINTSTS = 0x014,
	GINTMSK = 0x018,
	GRXFSIZ = 0x024,
	GNPTXFSIZ = 0x028,
	GNPTXSTS = 0x02c,
	GI2CCTL = 0x030,
	GPVNDCTL = 0x034,
	GGPIO = 0x038,
	GUID = 0x03c,
	GSNPSID = 0x040,
	GHWCFG1 = 0x044,
	GHWCFG2 = 0x048,
	GHWCFG3 = 0x04c,
	GHWCFG4 = 0x050,
	HPTXFSIZ = 0x100,
	HCFG = 0x400,
	HFIR = 0x404,
	HFNUM = 0x408,
	HPTXSTS = 0x410,
	HAINT = 0x414,
	HAINTMSK = 0x418,
	HPRT0 = 0x440,
	HC_BASE = 0x500, /* channel n at HC_BASE + 0x20 n */
	PCGCCTL = 0xe00,
};

/* Registers of a host channel, from its base. */
enum { HCCHAR, HCSPLT = 4, HCINT = 8, HCINTMSK = 12, HCTSIZ = 16, HCDMA = 20 };

#define GOTGCTL_CONIDSTS (1u << 16) /* 0: an A-device, a host */

#define GAHBCFG_GLBLINTRMSK (1u << 0)

#define GUSBCFG_FORCEDEVMODE (1u << 30)

#define GRSTCTL_SELF_CLEARING 0x37u /* resets and FIFO flushes */
#define GRSTCTL_TXFNUM (0x1fu << 6)
#define GRSTCTL_AHBIDLE (1u << 31)

#define GINTSTS_CURMODE_HOST (1u << 0)
#define GINTSTS_PRTINT (1u << 24)
#define GINTSTS_HCHINT (1u << 25)
#define GINTSTS_W1C 0xf030fc0au /* the bits that writing 1 clears */

#define HPRT0_CONNSTS (1u << 0)
#define HPRT0_CONNDET (1u << 1)
#define HPRT0_ENA (1u << 2)
#define HPRT0_ENCHNG (1u << 3)
#define HPRT0_OVRCURRCHNG (1u << 5)
#define HPRT0_CHANGES (HPRT0_CONNDET | HPRT0_ENCHNG | HPRT0_OVRCURRCHNG)
#define HPRT0_RW 0x1f1c0u /* RES, SUSP, RST, PWR and TSTCTL */

#define HCCHAR_CHDIS (1u << 30)
#define HCCHAR_CHENA (1u << 31)
#define HCINT_CHHLTD (1u << 1)

struct Dwc2 {
	Machine *machine;
	Dwc2Config config;
	IrqLine *irq;
	unsigned num_channels;

	uint32_t gotgctl, gahbcfg, gusbcfg, grstctl, gintsts, gintmsk;
	uint32_t grxfsiz, gnptxfsiz, hptxfsiz, gi2cctl, gpvndctl, ggpio, guid;
	uint32_t hcfg, hfir, haintmsk, pcgcctl;
	uint32_t hprt0; /* the RW bits and the change bits */
	struct Channel {
		uint32_t hcchar, hcsplt, hcint, hcintmsk, hctsiz, hcdma;
	} hc[DWC2_NUM_CHANNELS_MAX];
};

static void stop(Dwc2 *d, const char *access, uint64_t offset, unsigned size,
                 const char *why)
{
	machine_stop(d->machine, MACHINE_STOP_ERROR,
	             "dwc2: %u-byte %s offset 0x%03" PRIx64 " %s", size, access,
	             offset, why);
}

static uint32_t haint(const Dwc2 *d)
{
	uint32_t bits = 0;

	for (unsigned n = 0; n < d->num_channels; n++)
		if (d->hc[n].hcint & d->hc[n].hcintmsk)
			bits |= 1u << n;
	return bits;
}

static uint32_t gintsts(const Dwc2 *d)
{
	uint32_t v = d->gintsts;

	if (!(d->gusbcfg & GUSBCFG_FORCEDEVMODE))
		v |= GINTSTS_CURMODE_HOST;
	if (d->hprt0 & HPRT0_CHANGES)
		v |= GINTSTS_PRTINT;
	if (haint(d) & d->haintmsk)
		v |= GINTSTS_HCHINT;
	return v;
}

static void update_irq(Dwc2 *d)
{
	irq_set(d->irq, (d->gahbcfg & GAHBCFG_GLBLINTRMSK) &&
	                    (gintsts(d) & d->gintmsk) != 0);
}

/* The port: nothing attached, so never connected, never enabled. */
static uint32_t hprt0(const Dwc2 *d)
{
	return d->hprt0;
}

static void halt_channels(Dwc2 *d)
{
	for (unsigned n = 0; n < d->num_channels; n++)
		d->hc[n].hcchar &= ~(HCCHAR_CHENA | HCCHAR_CHDIS);
}

static uint64_t channel_read(Dwc2 *d, unsigned n, unsigned reg)
{
	struct Channel *c = &d->hc[n];

	switch (reg) {
	case HCCHAR:
		return c->hcchar;
	case HCSPLT:
		return c->hcsplt;
	case HCINT:
		return c->hcint;
	case HCINTMSK:
		return c->hcintmsk;
	case HCTSIZ:
		return c->hctsiz;
	default: /* HCDMA */
		return c->hcdma;
	}
}

static void channel_write(Dwc2 *d, uint64_t offset, unsigned n, unsigned reg,
                          uint32_t v)
{
	struct Channel *c = &d->hc[n];

	switch (reg) {
	case HCCHAR:
		if ((v & HCCHAR_CHENA) && !(v & HCCHAR_CHDIS)) {
			stop(d, "write to", offset, 4,
			     "is not emulated: a transfer, with no device on the "
			     "port");
			return;
		}
		if (v & HCCHAR_CHENA) {
			/* A halt of a channel that is idle ends at once. */
			v &= ~(HCCHAR_CHENA | HCCHAR_CHDIS);
			c->hcint |= HCINT_CHHLTD;
		}
		c->hcchar = v;
		break;
	case HCSPLT:
		c->hcsplt = v;
		break;
	case HCINT:
		c->hcint &= ~v;
		break;
	case HCINTMSK:
		c->hcintmsk = v;
		break;
	case HCTSIZ:
		c->hctsiz = v;
		break;
	default: /* HCDMA */
		c->hcdma = v;
		break;
	}
}

/* The number of the channel at offset, or -1; *reg its register. */
static int channel_of(const Dwc2 *d, uint64_t offset, unsigned *reg)
{
	uint64_t n = (offset - HC_BASE) / 0x20;

	if (offset < HC_BASE || n >= d->num_channels || offset % 0x20 > HCDMA)
		return -1;
	*reg = (unsigned)(offset % 0x20);
	return (int)n;
}

static uint64_t dwc2_read(void *ctx, uint64_t offset, unsigned size)
{
	Dwc2 *d = ctx;
	unsigned reg;
	int n;

	if (size != 4 || offset % 4 != 0) {
		stop(d, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	n = channel_of(d, offset, &reg);
	if (n >= 0)
		return channel_read(d, (unsigned)n, reg);
	switch (offset) {
	case GOTGCTL:
		return d->gotgctl & ~GOTGCTL_CONIDSTS;
	case GOTGINT:
		return 0;
	case GAHBCFG:
		return d->gahbcfg;
	case GUSBCFG:
		return d->gusbcfg;
	case GRSTCTL:
		return (d->grstctl & GRSTCTL_TXFNUM) | GRSTCTL_AHBIDLE;
	case GINTSTS:
		return gintsts(d);
	case GINTMSK:
		return d->gintmsk;
	case GRXFSIZ:
		return d->grxfsiz;
	case GNPTXFSIZ:
		return d->gnptxfsiz;
	case GNPTXSTS:
		/* Empty: the queue's 8 spaces and the FIFO's whole depth. */
		return 8u << 16 | (d->gnptxfsiz >> 16);
	case GI2CCTL:
		return d->gi2cctl;
	case GPVNDCTL:
		return d->gpvndctl;
	case GGPIO:
		return d->ggpio;
	case GUID:
		return d->guid;
	case GSNPSID:
		return d->config.snpsid;
	case GHWCFG1:
		return d->config.hwcfg1;
	case GHWCFG2:
		return d->config.hwcfg2;
	case GHWCFG3:
		return d->config.hwcfg3;
	case GHWCFG4:
		return d->config.hwcfg4;
	case HPTXFSIZ:
		return d->hptxfsiz;
	case HCFG:
		return d->hcfg;
	case HFIR:
		return d->hfir;
	case HFNUM:
		return 0; /* no frames while the port is disabled */
	case HPTXSTS:
		return 8u << 16 | (d->hptxfsiz >> 16);
	case HAINT:
		return haint(d);
	case HAINTMSK:
		return d->haintmsk;
	case HPRT0:
		return hprt0(d);
	case PCGCCTL:
		return d->pcgcctl;
	default:
		stop(d, "read of", offset, size,
		     "is not emulated (device mode, the FIFOs, or no register)");
		return 0;
	}
}

static void reset_registers(Dwc2 *d)
{
	d->gotgctl = 0;
	d->gahbcfg = 0;
	d->gusbcfg = 0;
	d->grstctl = 0;
	d->gintsts = 0;
	d->gintmsk = 0;
	/* The FIFO sizes at reset are the largest that GHWCFG3 allows. */
	d->grxfsiz = d->config.hwcfg3 >> 16;
	d->gnptxfsiz = (d->config.hwcfg3 >> 16) << 16 | (d->config.hwcfg3 >> 16);
	d->hptxfsiz = d->gnptxfsiz;
	d->gi2cctl = d->gpvndctl = d->ggpio = 0;
	d->guid = d->config.guid;
	d->hcfg = d->hfir = d->haintmsk = d->pcgcctl = 0;
	d->hprt0 = 0;
	memset(d->hc, 0, sizeof(d->hc));
	update_irq(d);
}

static void dwc2_write(void *ctx, uint64_t offset, uint64_t value,
                       unsigned size)
{
	Dwc2 *d = ctx;
	uint32_t v = (uint32_t)value;
	unsigned reg;
	int n;

	if (size != 4 || offset % 4 != 0) {
		stop(d, "write to", offset, size, "is not a 32-bit register write");
		return;
	}
	n = channel_of(d, offset, &reg);
	if (n >= 0) {
		channel_write(d, offset, (unsigned)n, reg, v);
		update_irq(d);
		return;
	}
	switch (offset) {
	case GOTGCTL:
		d->gotgctl = v;
		break;
	case GOTGINT:
		break; /* no OTG events to clear */
	case GAHBCFG:
		d->gahbcfg = v;
		break;
	case GUSBCFG:
		if (v & GUSBCFG_FORCEDEVMODE) {
			stop(d, "write to", offset, size, "is not emulated: device mode");
			return;
		}
		d->gusbcfg = v;
		break;
	case GRSTCTL:
		/* The resets and the flushes end at once. */
		if (v & 1) { /* CSFTRST: the core's state, not its settings */
			halt_channels(d);
			d->gintsts = 0;
		}
		d->grstctl = v & ~GRSTCTL_SELF_CLEARING;
		break;
	case GINTSTS:
		d->gintsts &= ~(v & GINTSTS_W1C);
		break;
	case GINTMSK:
		d->gintmsk = v;
		break;
	case GRXFSIZ:
		d->grxfsiz = v & 0xffff;
		break;
	case GNPTXFSIZ:
		d->gnptxfsiz = v;
		break;
	case GI2CCTL:
		d->gi2cctl = v;
		break;
	case GPVNDCTL:
		d->gpvndctl = v;
		break;
	case GGPIO:
		d->ggpio = v;
		break;
	case GUID:
		d->guid = v;
		break;
	case HPTXFSIZ:
		d->hptxfsiz = v;
		break;
	case HCFG:
		d->hcfg = v;
		break;
	case HFIR:
		d->hfir = v & 0xffff;
		break;
	case HAINTMSK:
		d->haintmsk = v & ((1u << d->num_channels) - 1);
		break;
	case HPRT0:
		/* Writing 1 clears the change bits; ENA cannot be set. */
		d->hprt0 = (d->hprt0 & HPRT0_CHANGES & ~v) | (v & HPRT0_RW);
		break;
	case PCGCCTL:
		d->pcgcctl = v;
		break;
	default:
		stop(d, "write to", offset, size,
		     "is not emulated (device mode, the FIFOs, or no register)");
		return;
	}
	update_irq(d);
}

static const BusMmioOps dwc2_mmio_ops = {
	.read = dwc2_read,
	.write = dwc2_write,
};

void dwc2_reset(Dwc2 *d)
{
	reset_registers(d);
}

Dwc2 *dwc2_create(Machine *m, const Dwc2Config *config, IrqLine *irq)
{
	Dwc2 *d = calloc(1, sizeof(*d));

	if (d == NULL)
		return NULL;
	d->machine = m;
	d->config = *config;
	d->irq = irq;
	/* GHWCFG2 bits 17:14: the number of host channels, less 1. */
	d->num_channels = ((config->hwcfg2 >> 14) & 0xf) + 1;
	reset_registers(d);
	return d;
}

void dwc2_destroy(Dwc2 *d)
{
	free(d);
}

BusStatus dwc2_map(Dwc2 *d, Bus *bus, const char *name, uint64_t base)
{
	return bus_map_mmio(bus, name, base, DWC2_MMIO_SIZE, &dwc2_mmio_ops, d);
}
