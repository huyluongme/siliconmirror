/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The DWC2 USB controller, as a host; see dwc2.h. */

#include "hw/usb/dwc2.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/event.h"
#include "util/muldiv.h"

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

#define GRSTCTL_TXFNUM (0x1fu << 6)
#define GRSTCTL_AHBIDLE (1u << 31)

#define GINTSTS_CURMODE_HOST (1u << 0)
#define GINTSTS_PRTINT (1u << 24)
#define GINTSTS_HCHINT (1u << 25)

#define HPRT0_CONNSTS (1u << 0)
#define HPRT0_CONNDET (1u << 1)
#define HPRT0_ENA (1u << 2)
#define HPRT0_ENCHNG (1u << 3)
#define HPRT0_OVRCURRCHNG (1u << 5)
#define HPRT0_CHANGES (HPRT0_CONNDET | HPRT0_ENCHNG | HPRT0_OVRCURRCHNG)
#define HPRT0_RST (1u << 8)
#define HPRT0_PWR (1u << 12)
#define HPRT0_RW 0x1f1c0u /* RES, SUSP, RST, PWR and TSTCTL */

#define HCCHAR_CHDIS (1u << 30)
#define HCCHAR_CHENA (1u << 31)
#define HCSPLT_SPLTENA (1u << 31)

/* HCINT */
#define XFERCOMPL (1u << 0)
#define HCINT_CHHLTD (1u << 1)
#define STALL (1u << 3)
#define NAK (1u << 4)
#define ACK (1u << 5)
#define XACTERR (1u << 7)
#define BBLERR (1u << 8)

/* HCTSIZ's PID field. */
enum { PID_DATA0 = 0, PID_DATA1 = 2, PID_SETUP = 3 };

/*
 * Bus times, at 480 Mb/s: a byte takes 50/3 ns. A transaction adds its
 * token, handshake and gaps, which smirror counts as 16 bytes; a device
 * that does not answer costs three tries of a 16-byte timeout each.
 */
#define OVERHEAD_BYTES 16
#define MICROFRAME_NS 125000

struct Dwc2 {
	Machine *machine;
	Dwc2Config config;
	IrqLine *irq;
	unsigned num_channels;

	uint32_t gotgctl, gahbcfg, gusbcfg, grstctl, gintmsk;
	uint32_t grxfsiz, gnptxfsiz, hptxfsiz, gi2cctl, gpvndctl, ggpio, guid;
	uint32_t hcfg, hfir, haintmsk, pcgcctl;
	uint32_t hprt0;      /* the RW bits and the change bits */
	UsbDevice *port_dev; /* on the root port, or NULL */
	bool port_enabled;
	uint64_t frames_from; /* when the port started sending frames */
	struct Channel {
		Dwc2 *dwc2;
		uint32_t hcchar, hcsplt, hcint, hcintmsk, hctsiz, hcdma;
		bool busy;
		uint32_t pending; /* HCINT bits at the end of the transfer */
		Event done;
	} hc[DWC2_NUM_CHANNELS_MAX];
	uint8_t packet[1024]; /* the largest packet, high-speed bulk's 512 */
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

/*
 * The core's own events, such as SOF, are not raised; the port's and
 * the channels' come from their registers.
 */
static uint32_t gintsts(const Dwc2 *d)
{
	uint32_t v = 0;

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

/* The root port: connected while powered with a device; high speed. */
static uint32_t hprt0(const Dwc2 *d)
{
	uint32_t v = d->hprt0;

	if ((v & HPRT0_PWR) && d->port_dev != NULL)
		v |= HPRT0_CONNSTS;
	if (d->port_enabled)
		v |= HPRT0_ENA; /* PRTSPD 0: high speed */
	return v;
}

/* HPRT0: power, then a reset that ends when software ends it. */
static void write_hprt0(Dwc2 *d, uint32_t v)
{
	uint32_t old = d->hprt0;

	d->hprt0 = (old & HPRT0_CHANGES & ~v) | (v & HPRT0_RW);
	if (v & HPRT0_ENA) /* writing 1 disables the port */
		d->port_enabled = false;
	if ((v & HPRT0_PWR) && !(old & HPRT0_PWR) && d->port_dev != NULL)
		d->hprt0 |= HPRT0_CONNDET;
	if (!(v & HPRT0_PWR))
		d->port_enabled = false;
	if (v & HPRT0_RST)
		d->port_enabled = false;
	if ((old & HPRT0_RST) && !(v & HPRT0_RST) && (v & HPRT0_PWR) &&
	    d->port_dev != NULL) {
		usb_device_reset(d->port_dev);
		d->port_enabled = true;
		d->frames_from = machine_now(d->machine);
		d->hprt0 |= HPRT0_ENCHNG;
	}
}

/* HFNUM: the microframe number and the time left in it, in 60 MHz clocks. */
static uint32_t hfnum(const Dwc2 *d)
{
	uint64_t t;

	if (!d->port_enabled)
		return 0;
	t = machine_now(d->machine) - d->frames_from;
	return (uint32_t)((MICROFRAME_NS - t % MICROFRAME_NS) * 60 / 1000) << 16 |
	       (uint32_t)(t / MICROFRAME_NS & 0x3fff);
}

static void channel_done(void *ctx)
{
	struct Channel *c = ctx;

	c->busy = false;
	c->hcchar &= ~(HCCHAR_CHENA | HCCHAR_CHDIS);
	c->hcint |= c->pending;
	update_irq(c->dwc2);
}

static void halt_channels(Dwc2 *d)
{
	for (unsigned n = 0; n < d->num_channels; n++) {
		event_cancel(&d->machine->events, &d->hc[n].done);
		d->hc[n].busy = false;
		d->hc[n].hcchar &= ~(HCCHAR_CHENA | HCCHAR_CHDIS);
	}
}

static uint64_t bytes_ns(uint64_t bytes)
{
	return mul_div_ceil(bytes, 50, 3);
}

/*
 * The packets of a channel's transfer, as the core does them with DMA:
 * HCTSIZ's packets of HCCHAR's maximum size, each to the device, until
 * the last, a short IN packet, a NAK, a STALL or no answer. The device
 * acts at once; HCINT says so when the bus time has passed.
 */
static void start_transfer(Dwc2 *d, uint64_t offset, struct Channel *c)
{
	uint32_t ch = c->hcchar, ts = c->hctsiz;
	unsigned mps = ch & 0x7ff, ep = ch >> 11 & 0xf, in = ch >> 15 & 1;
	unsigned type = ch >> 18 & 3, address = ch >> 22 & 0x7f;
	uint32_t size = ts & 0x7ffff, packets = ts >> 19 & 0x3ff;
	unsigned pid = ts >> 29 & 3;
	uint64_t dma = c->hcdma & d->config.dma_mask, ns = 0;
	uint32_t result = 0;
	UsbDevice *dev;

	if (!d->port_enabled || (c->hcsplt & HCSPLT_SPLTENA) || type == 1 ||
	    mps == 0 || mps > sizeof(d->packet) || packets == 0) {
		stop(d, "write to", offset, 4,
		     "is not emulated: a transfer with the port disabled, a "
		     "split, an isochronous endpoint or no packets");
		return;
	}
	dev = usb_device_find(d->port_dev, address);
	while (result == 0) {
		size_t len = 0;
		UsbResult r;

		if (dev == NULL) {
			r = USB_NO_RESPONSE;
		} else if (!in) {
			len = size < mps ? size : mps;
			bus_read_block(d->machine->bus, dma, d->packet, len);
			if (pid == PID_SETUP && type == 0)
				r = len == 8 ? usb_device_setup(dev, d->packet)
				             : USB_NO_RESPONSE;
			else
				r = usb_device_out(dev, ep, d->packet, len);
		} else {
			r = usb_device_in(dev, ep, d->packet, mps, &len);
			if (r == USB_ACK && len > size) {
				result = BBLERR | HCINT_CHHLTD;
				break;
			}
			if (r == USB_ACK)
				bus_write_block(d->machine->bus, dma, d->packet, len);
		}
		ns += bytes_ns(len + OVERHEAD_BYTES);
		switch (r) {
		case USB_NAK:
			result = NAK | HCINT_CHHLTD;
			continue;
		case USB_STALL:
			result = STALL | HCINT_CHHLTD;
			continue;
		case USB_NO_RESPONSE:
			ns += 2 * bytes_ns(OVERHEAD_BYTES);
			result = XACTERR | HCINT_CHHLTD;
			continue;
		case USB_ACK:
			break;
		}
		dma += len;
		size -= (uint32_t)len;
		packets--;
		pid = pid == PID_DATA1 ? PID_DATA0 : PID_DATA1;
		if (packets == 0 || (in && len < mps))
			result = XFERCOMPL | ACK | HCINT_CHHLTD;
	}
	c->hctsiz = (ts & (1u << 31)) | pid << 29 | packets << 19 | size;
	c->hcdma = (c->hcdma & ~d->config.dma_mask) | (uint32_t)dma;
	c->pending = result;
	c->busy = true;
	event_schedule(&d->machine->events, &c->done, machine_now(d->machine) + ns);
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
		if (c->busy) {
			if ((v & (HCCHAR_CHENA | HCCHAR_CHDIS)) !=
			    (HCCHAR_CHENA | HCCHAR_CHDIS)) {
				stop(d, "write to", offset, 4,
				     "is not emulated: a change to a channel that runs");
				return;
			}
			/* A halt: the transfer ends now, as far as it went. */
			event_cancel(&d->machine->events, &c->done);
			channel_done(c);
			return;
		}
		if ((v & HCCHAR_CHENA) && !(v & HCCHAR_CHDIS)) {
			c->hcchar = v;
			start_transfer(d, offset, c);
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
		return d->grstctl | GRSTCTL_AHBIDLE;
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
		return hfnum(d);
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
	d->gintmsk = 0;
	/* The FIFO sizes at reset are the largest that GHWCFG3 allows. */
	d->grxfsiz = d->config.hwcfg3 >> 16;
	d->gnptxfsiz = (d->config.hwcfg3 >> 16) << 16 | (d->config.hwcfg3 >> 16);
	d->hptxfsiz = d->gnptxfsiz;
	d->gi2cctl = d->gpvndctl = d->ggpio = 0;
	d->guid = d->config.guid;
	d->hcfg = d->hfir = d->haintmsk = d->pcgcctl = 0;
	d->hprt0 = 0;
	d->port_enabled = false;
	halt_channels(d);
	for (unsigned n = 0; n < DWC2_NUM_CHANNELS_MAX; n++) {
		struct Channel *c = &d->hc[n];

		c->hcchar = c->hcsplt = c->hcint = c->hcintmsk = 0;
		c->hctsiz = c->hcdma = c->pending = 0;
	}
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
		if (v & 1) /* CSFTRST: the core's state, not its settings */
			halt_channels(d);
		d->grstctl = v & GRSTCTL_TXFNUM;
		break;
	case GINTSTS:
		break; /* no event of its own to clear */
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
		write_hprt0(d, v);
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
	for (unsigned n = 0; n < DWC2_NUM_CHANNELS_MAX; n++) {
		d->hc[n].dwc2 = d;
		event_init(&d->hc[n].done, channel_done, &d->hc[n]);
	}
	reset_registers(d);
	return d;
}

void dwc2_destroy(Dwc2 *d)
{
	halt_channels(d);
	free(d);
}

void dwc2_attach(Dwc2 *d, UsbDevice *dev)
{
	d->port_dev = dev;
}

BusStatus dwc2_map(Dwc2 *d, Bus *bus, const char *name, uint64_t base)
{
	return bus_map_mmio(bus, name, base, DWC2_MMIO_SIZE, &dwc2_mmio_ops, d);
}
