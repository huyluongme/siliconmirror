/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* A USB 2.0 hub; see usb_hub.h. Section numbers are USB 2.0's. */

#include "hw/usb/usb_hub.h"

#include <stdlib.h>
#include <string.h>

#include "core/event.h"
#include "util/byteorder.h"

/* Hub class requests (Table 11-16) and the hub descriptor's type. */
enum {
	GET_STATUS = 0,
	CLEAR_FEATURE = 1,
	SET_FEATURE = 3,
	GET_DESCRIPTOR = 6,
	CLEAR_TT_BUFFER = 8,
	RESET_TT = 9,
	GET_TT_STATE = 10,
	STOP_TT = 11,
};
#define DESC_HUB 0x29

/* Feature selectors (Table 11-17). */
enum {
	PORT_CONNECTION = 0,
	PORT_ENABLE = 1,
	PORT_SUSPEND = 2,
	PORT_OVER_CURRENT = 3,
	PORT_RESET = 4,
	PORT_POWER = 8,
	PORT_LOW_SPEED = 9,
	C_PORT_CONNECTION = 16,
	C_PORT_ENABLE = 17,
	C_PORT_SUSPEND = 18,
	C_PORT_OVER_CURRENT = 19,
	C_PORT_RESET = 20,
	PORT_TEST = 21,
	PORT_INDICATOR = 22,
	C_HUB_LOCAL_POWER = 0,
	C_HUB_OVER_CURRENT = 1,
};

/* wPortStatus (Table 11-21) and wPortChange (Table 11-22). */
#define STAT_CONNECTION (1u << 0)
#define STAT_ENABLE (1u << 1)
#define STAT_RESET (1u << 4)
#define STAT_POWER (1u << 8)
#define STAT_HIGH_SPEED (1u << 10)
#define CHANGE_CONNECTION (1u << 0)
#define CHANGE_ENABLE (1u << 1)
#define CHANGE_RESET (1u << 4)

#define TDRST_NS 10000000 /* a port reset, 10 ms at least (7.1.7.5) */

typedef struct Port {
	UsbHub *hub;
	UsbDevice *dev;
	uint16_t status, change;
	Event reset_done;
} Port;

struct UsbHub {
	Machine *machine;
	UsbHubConfig config;
	UsbDevice dev;
	uint8_t device_desc[18];
	uint8_t config_desc[9 + 2 * (9 + 7)];
	UsbDeviceClass cls;
	Port ports[USB_HUB_MAX_PORTS + 1]; /* 1 to ports */
};

static void port_reset_done(void *ctx)
{
	Port *p = ctx;

	p->status &= ~STAT_RESET;
	if (p->dev != NULL && (p->status & STAT_CONNECTION)) {
		usb_device_reset(p->dev);
		p->status |= STAT_ENABLE | STAT_HIGH_SPEED;
	}
	p->change |= CHANGE_RESET;
}

/* Power on or off: a device shows as connected while there is power. */
static void port_power(Port *p, bool on)
{
	if (on && !(p->status & STAT_POWER)) {
		p->status |= STAT_POWER;
		if (p->dev != NULL) {
			p->status |= STAT_CONNECTION;
			p->change |= CHANGE_CONNECTION;
		}
	} else if (!on && (p->status & STAT_POWER)) {
		event_cancel(&p->hub->machine->events, &p->reset_done);
		if (p->status & STAT_CONNECTION)
			p->change |= CHANGE_CONNECTION;
		p->status = 0;
	}
}

static bool port_feature(UsbHub *h, bool set, unsigned feature, unsigned port)
{
	Port *p;

	if (port < 1 || port > h->config.ports)
		return false;
	p = &h->ports[port];
	switch (feature) {
	case PORT_ENABLE: /* only a reset enables a port */
		if (set)
			return false;
		p->status &= ~STAT_ENABLE;
		return true;
	case PORT_RESET:
		if (!set)
			return false;
		if (p->status & STAT_POWER) {
			p->status = (p->status | STAT_RESET) & ~STAT_ENABLE;
			event_schedule(&h->machine->events, &p->reset_done,
			               machine_now(h->machine) + TDRST_NS);
		}
		return true;
	case PORT_POWER:
		port_power(p, set);
		return true;
	case C_PORT_CONNECTION:
	case C_PORT_ENABLE:
	case C_PORT_RESET:
		if (set)
			return false;
		p->change &= ~(1u << (feature - C_PORT_CONNECTION));
		return true;
	case C_PORT_SUSPEND:
	case C_PORT_OVER_CURRENT:
		return !set; /* never set */
	default:
		return false; /* suspend, test modes, indicators */
	}
}

static bool hub_request(UsbDevice *d, const UsbSetup *s, uint8_t *data,
                        size_t *len)
{
	UsbHub *h = d->ctx;
	unsigned recipient = s->bm_request_type & USB_RECIP_MASK;
	bool in = s->bm_request_type & USB_DIR_IN;

	if ((s->bm_request_type & USB_TYPE_MASK) != USB_TYPE_CLASS ||
	    d->config == 0)
		return false;
	switch (s->b_request) {
	case GET_STATUS:
		if (!in)
			return false;
		memset(data, 0, 4);
		*len = 4;
		if (recipient == USB_RECIP_DEVICE)
			return true; /* local power good, no over-current */
		if (recipient != USB_RECIP_OTHER || s->w_index < 1 ||
		    s->w_index > h->config.ports)
			return false;
		store_le16(data, h->ports[s->w_index].status);
		store_le16(data + 2, h->ports[s->w_index].change);
		return true;
	case CLEAR_FEATURE:
	case SET_FEATURE:
		if (in)
			return false;
		if (recipient == USB_RECIP_DEVICE)
			return s->b_request == CLEAR_FEATURE &&
			       (s->w_value == C_HUB_LOCAL_POWER ||
			        s->w_value == C_HUB_OVER_CURRENT);
		return recipient == USB_RECIP_OTHER &&
		       port_feature(h, s->b_request == SET_FEATURE, s->w_value,
		                    s->w_index & 0xff);
	case GET_DESCRIPTOR: {
		unsigned n = h->config.ports;

		if (!in || (s->w_value >> 8) != DESC_HUB)
			return false;
		data[0] = 9; /* 7 bytes, then 1 for each bitmap of 8 ports */
		data[1] = DESC_HUB;
		data[2] = (uint8_t)n;
		store_le16(data + 3, h->config.characteristics);
		data[5] = h->config.power_on_time;
		data[6] = h->config.controller_current;
		data[7] = h->config.removable;
		data[8] = 0xff; /* PortPwrCtrlMask: all 1, as 11.23.2.1 asks */
		*len = 9;
		return true;
	}
	case CLEAR_TT_BUFFER:
	case RESET_TT:
	case STOP_TT:
		return !in; /* no transaction translator holds state */
	default:
		return false;
	}
}

/* The status change endpoint: a bitmap of the ports with changes. */
static UsbResult hub_in(UsbDevice *d, unsigned ep, uint8_t *buf, size_t max,
                        size_t *len)
{
	UsbHub *h = d->ctx;
	unsigned bits = 0;

	if (ep != 1 || max < 1)
		return USB_STALL;
	for (unsigned n = 1; n <= h->config.ports; n++)
		if (h->ports[n].change)
			bits |= 1u << n;
	if (bits == 0)
		return USB_NAK;
	buf[0] = (uint8_t)bits;
	*len = 1;
	return USB_ACK;
}

static UsbDevice *hub_downstream(UsbDevice *d, unsigned address)
{
	UsbHub *h = d->ctx;

	for (unsigned n = 1; n <= h->config.ports; n++) {
		Port *p = &h->ports[n];
		UsbDevice *found;

		if (p->dev == NULL || !(p->status & STAT_ENABLE))
			continue;
		found = usb_device_find(p->dev, address);
		if (found != NULL)
			return found;
	}
	return NULL;
}

/* A bus reset of the hub: its ports lose power (11.11). */
static void hub_reset(UsbDevice *d)
{
	UsbHub *h = d->ctx;

	for (unsigned n = 1; n <= h->config.ports; n++) {
		event_cancel(&h->machine->events, &h->ports[n].reset_done);
		h->ports[n].status = h->ports[n].change = 0;
	}
}

/* The descriptors of 11.23.1, from the configuration. */
static void make_descriptors(UsbHub *h)
{
	const UsbHubConfig *c = &h->config;
	uint8_t *d = h->device_desc, *cfg = h->config_desc;
	unsigned alternates = c->multi_tt ? 2 : 1;

	d[0] = 18;
	d[1] = 1;
	store_le16(d + 2, 0x0200); /* bcdUSB */
	d[4] = 9;                  /* hub class */
	d[5] = 0;
	d[6] = c->multi_tt ? 2 : 1; /* high speed, with one TT or more */
	d[7] = 64;                  /* bMaxPacketSize0 */
	store_le16(d + 8, c->vendor);
	store_le16(d + 10, c->product);
	store_le16(d + 12, c->release);
	d[14] = d[15] = d[16] = 0; /* no strings */
	d[17] = 1;                 /* one configuration */

	cfg[0] = 9;
	cfg[1] = 2;
	store_le16(cfg + 2, (uint16_t)(9 + alternates * (9 + 7)));
	cfg[4] = 1; /* one interface */
	cfg[5] = 1; /* bConfigurationValue */
	cfg[6] = 0;
	cfg[7] = 0x80 | (c->self_powered ? 0x40 : 0);
	cfg[8] = c->max_power;
	for (unsigned alt = 0; alt < alternates; alt++) {
		uint8_t *i = cfg + 9 + alt * 16, *e = i + 9;

		i[0] = 9;
		i[1] = 4;
		i[2] = 0; /* interface 0 */
		i[3] = (uint8_t)alt;
		i[4] = 1; /* one endpoint */
		i[5] = 9; /* hub class */
		i[6] = 0;
		i[7] = (uint8_t)(alt == 0 ? (c->multi_tt ? 1 : 0) : 2);
		i[8] = 0;
		e[0] = 7;
		e[1] = 5;
		e[2] = 0x81;          /* endpoint 1 IN */
		e[3] = 3;             /* interrupt */
		store_le16(e + 4, 1); /* a bit for the hub, up to 7 ports */
		e[6] = 12;            /* every 2^11 microframes */
	}
	h->cls = (UsbDeviceClass){
		.device = h->device_desc,
		.config = h->config_desc,
		.alternates = alternates,
		.request = hub_request,
		.in = hub_in,
		.downstream = hub_downstream,
		.reset = hub_reset,
	};
}

UsbHub *usb_hub_create(Machine *m, const UsbHubConfig *config)
{
	UsbHub *h;

	if (config->ports < 1 || config->ports > USB_HUB_MAX_PORTS)
		return NULL;
	h = calloc(1, sizeof(*h));
	if (h == NULL)
		return NULL;
	h->machine = m;
	h->config = *config;
	for (unsigned n = 1; n <= config->ports; n++) {
		h->ports[n].hub = h;
		event_init(&h->ports[n].reset_done, port_reset_done, &h->ports[n]);
	}
	make_descriptors(h);
	usb_device_init(&h->dev, &h->cls, h, USB_SPEED_HIGH);
	return h;
}

void usb_hub_destroy(UsbHub *h)
{
	for (unsigned n = 1; n <= h->config.ports; n++)
		event_cancel(&h->machine->events, &h->ports[n].reset_done);
	free(h);
}

UsbDevice *usb_hub_device(UsbHub *h)
{
	return &h->dev;
}

void usb_hub_attach(UsbHub *h, unsigned port, UsbDevice *dev)
{
	if (port >= 1 && port <= h->config.ports)
		h->ports[port].dev = dev;
}
