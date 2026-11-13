/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The USB device framework; see usb.h. Section numbers are USB 2.0's. */

#include "hw/usb/usb.h"

#include <string.h>

#include "util/byteorder.h"

/* Standard requests (Table 9-4) and descriptor types (Table 9-5). */
enum {
	GET_STATUS = 0,
	CLEAR_FEATURE = 1,
	SET_FEATURE = 3,
	SET_ADDRESS = 5,
	GET_DESCRIPTOR = 6,
	GET_CONFIGURATION = 8,
	SET_CONFIGURATION = 9,
	GET_INTERFACE = 10,
	SET_INTERFACE = 11,
};
enum { DESC_DEVICE = 1, DESC_CONFIG = 2, DESC_QUALIFIER = 6 };
enum { ENDPOINT_HALT = 0 };

void usb_device_init(UsbDevice *d, const UsbDeviceClass *cls, void *ctx,
                     UsbSpeed speed)
{
	memset(d, 0, sizeof(*d));
	d->cls = cls;
	d->ctx = ctx;
	d->speed = speed;
	usb_device_reset(d);
}

void usb_device_reset(UsbDevice *d)
{
	d->address = 0;
	d->config = 0;
	d->alternate = 0;
	memset(d->halted, 0, sizeof(d->halted));
	d->stage = USB_CONTROL_IDLE;
	d->new_address = -1;
	if (d->cls->reset != NULL)
		d->cls->reset(d);
}

unsigned usb_device_max_packet0(const UsbDevice *d)
{
	return d->cls->device[7];
}

static unsigned config_value(const UsbDevice *d)
{
	return d->cls->config[5];
}

/* The endpoint of a request's wIndex: number and direction, if valid. */
static bool endpoint_of(const UsbSetup *s, unsigned *ep, unsigned *in)
{
	*ep = s->w_index & 0xf;
	*in = (s->w_index & USB_DIR_IN) != 0;
	return (s->w_index & 0xff70) == 0;
}

/* A standard request; data and *len as for UsbDeviceClass.request. */
static bool standard(UsbDevice *d, const UsbSetup *s, uint8_t *data,
                     size_t *len)
{
	unsigned recipient = s->bm_request_type & USB_RECIP_MASK, ep, in;

	switch (s->b_request) {
	case GET_STATUS:
		memset(data, 0, 2);
		*len = 2;
		if (recipient == USB_RECIP_DEVICE) {
			data[0] = (d->cls->config[7] & 0x40) ? 1 : 0; /* self-powered */
			return true;
		}
		if (recipient == USB_RECIP_INTERFACE)
			return d->config != 0 && s->w_index == 0;
		if (recipient == USB_RECIP_ENDPOINT && endpoint_of(s, &ep, &in)) {
			data[0] = d->halted[ep][in];
			return ep == 0 || d->config != 0;
		}
		return false;
	case CLEAR_FEATURE:
	case SET_FEATURE:
		if (recipient != USB_RECIP_ENDPOINT || s->w_value != ENDPOINT_HALT ||
		    !endpoint_of(s, &ep, &in) || (ep != 0 && d->config == 0))
			return false;
		d->halted[ep][in] = s->b_request == SET_FEATURE;
		return true;
	case SET_ADDRESS:
		if (s->w_value > 127 || s->w_index != 0 || s->w_length != 0)
			return false;
		d->new_address = s->w_value; /* after the status stage */
		return true;
	case GET_DESCRIPTOR:
		switch (s->w_value >> 8) {
		case DESC_DEVICE:
			memcpy(data, d->cls->device, 18);
			*len = 18;
			return true;
		case DESC_CONFIG:
			if ((s->w_value & 0xff) != 0)
				return false;
			*len = load_le16(d->cls->config + 2);
			memcpy(data, d->cls->config, *len);
			return true;
		case DESC_QUALIFIER:
			/* 9.6.2: the device's fields at the other speed. */
			if (d->speed != USB_SPEED_HIGH)
				return false;
			data[0] = 10;
			data[1] = DESC_QUALIFIER;
			memcpy(data + 2, d->cls->device + 2, 6); /* bcdUSB to MPS0 */
			data[8] = d->cls->device[17];            /* configurations */
			data[9] = 0;
			*len = 10;
			return true;
		default:
			return false; /* no strings, no other-speed configuration */
		}
	case GET_CONFIGURATION:
		data[0] = d->config;
		*len = 1;
		return true;
	case SET_CONFIGURATION:
		if (s->w_value != 0 && s->w_value != config_value(d))
			return false;
		d->config = (uint8_t)s->w_value;
		d->alternate = 0;
		memset(d->halted, 0, sizeof(d->halted));
		if (d->cls->configured != NULL)
			d->cls->configured(d, d->config);
		return true;
	case GET_INTERFACE:
		if (d->config == 0 || s->w_index != 0)
			return false;
		data[0] = d->alternate;
		*len = 1;
		return true;
	case SET_INTERFACE:
		if (d->config == 0 || s->w_index != 0 ||
		    s->w_value >= d->cls->alternates)
			return false;
		d->alternate = (uint8_t)s->w_value;
		return true;
	default:
		return false;
	}
}

/* Carry out the request of the SETUP, its data in d->data. */
static bool carry_out(UsbDevice *d)
{
	const UsbSetup *s = &d->setup;
	size_t len = d->len;
	bool ok;

	if ((s->bm_request_type & USB_TYPE_MASK) == USB_TYPE_STANDARD)
		ok = standard(d, s, d->data, &len);
	else
		ok = d->cls->request != NULL && d->cls->request(d, s, d->data, &len);
	if (s->bm_request_type & USB_DIR_IN)
		d->len = len < s->w_length ? len : s->w_length;
	return ok;
}

UsbResult usb_device_setup(UsbDevice *d, const uint8_t packet[8])
{
	UsbSetup *s = &d->setup;

	/* A SETUP is always taken, and ends a transfer in progress (8.5.3). */
	s->bm_request_type = packet[0];
	s->b_request = packet[1];
	s->w_value = load_le16(packet + 2);
	s->w_index = load_le16(packet + 4);
	s->w_length = load_le16(packet + 6);
	d->new_address = -1;
	d->pos = 0;
	d->len = s->w_length < USB_CONTROL_MAX ? s->w_length : USB_CONTROL_MAX;
	if (s->w_length > USB_CONTROL_MAX && !(s->bm_request_type & USB_DIR_IN)) {
		d->stage = USB_CONTROL_STALLED;
		return USB_ACK;
	}
	if (!(s->bm_request_type & USB_DIR_IN) && s->w_length != 0) {
		d->len = 0;
		d->stage = USB_CONTROL_DATA_OUT;
		return USB_ACK;
	}
	if (!carry_out(d))
		d->stage = USB_CONTROL_STALLED;
	else if (s->bm_request_type & USB_DIR_IN && s->w_length != 0)
		d->stage = USB_CONTROL_DATA_IN;
	else
		d->stage = USB_CONTROL_STATUS_IN;
	return USB_ACK;
}

static UsbResult control_in(UsbDevice *d, uint8_t *buf, size_t max, size_t *len)
{
	size_t n;

	*len = 0;
	switch (d->stage) {
	case USB_CONTROL_DATA_IN:
		n = d->len - d->pos < max ? d->len - d->pos : max;
		memcpy(buf, d->data + d->pos, n);
		d->pos += n;
		*len = n;
		return USB_ACK;
	case USB_CONTROL_STATUS_IN:
		if (d->new_address >= 0)
			d->address = (uint8_t)d->new_address;
		d->new_address = -1;
		d->stage = USB_CONTROL_IDLE;
		return USB_ACK;
	default:
		return USB_STALL;
	}
}

static UsbResult control_out(UsbDevice *d, const uint8_t *buf, size_t len)
{
	switch (d->stage) {
	case USB_CONTROL_DATA_OUT:
		if (len > d->setup.w_length - d->len) {
			d->stage = USB_CONTROL_STALLED;
			return USB_STALL;
		}
		memcpy(d->data + d->len, buf, len);
		d->len += len;
		if (d->len == d->setup.w_length)
			d->stage =
				carry_out(d) ? USB_CONTROL_STATUS_IN : USB_CONTROL_STALLED;
		return USB_ACK;
	case USB_CONTROL_DATA_IN:
	case USB_CONTROL_STATUS_OUT:
		/* The status stage of an IN request: a zero-length OUT. */
		d->stage = USB_CONTROL_IDLE;
		return len == 0 ? USB_ACK : USB_STALL;
	default:
		return USB_STALL;
	}
}

UsbResult usb_device_in(UsbDevice *d, unsigned ep, uint8_t *buf, size_t max,
                        size_t *len)
{
	*len = 0;
	if (ep == 0)
		return control_in(d, buf, max, len);
	if (ep >= USB_ENDPOINTS || d->config == 0 || d->halted[ep][1] ||
	    d->cls->in == NULL)
		return USB_STALL;
	return d->cls->in(d, ep, buf, max, len);
}

UsbResult usb_device_out(UsbDevice *d, unsigned ep, const uint8_t *buf,
                         size_t len)
{
	if (ep == 0)
		return control_out(d, buf, len);
	if (ep >= USB_ENDPOINTS || d->config == 0 || d->halted[ep][0] ||
	    d->cls->out == NULL)
		return USB_STALL;
	return d->cls->out(d, ep, buf, len);
}

UsbDevice *usb_device_find(UsbDevice *d, unsigned address)
{
	if (d == NULL)
		return NULL;
	if (d->address == address)
		return d;
	return d->cls->downstream != NULL ? d->cls->downstream(d, address) : NULL;
}
