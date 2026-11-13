/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the USB device framework (src/hw/usb/usb.c): chapter 9
 * of the USB 2.0 specification, on a device of the tests.
 */

#include <stdint.h>
#include <string.h>

#include "hw/usb/usb.h"
#include "test.h"

static const uint8_t device_desc[18] = {
	18,   1,    0x00, 0x02, 0xff, 0, 0, 64, 0x34,
	0x12, 0x78, 0x56, 0x00, 0x01, 0, 0, 0,  1,
};

/* One configuration, value 3, self-powered; an interface, 2 settings. */
static const uint8_t config_desc[25] = {
	9, 2,    25, 0, 1, 3, 0, 0xc0, 1, 9,    4,    0, 0,
	1, 0xff, 0,  0, 0, 7, 5, 0x81, 2, 0x00, 0x02, 0,
};

/* The device of the tests: endpoint 1 IN gives "ok"; a vendor request. */
static unsigned configured_with = 99;
static uint8_t vendor_out[4];

static bool fake_request(UsbDevice *d, const UsbSetup *s, uint8_t *data,
                         size_t *len)
{
	(void)d;
	if ((s->bm_request_type & USB_TYPE_MASK) != USB_TYPE_VENDOR)
		return false;
	if (s->b_request == 0xa1 && (s->bm_request_type & USB_DIR_IN)) {
		memcpy(data, "\x11\x22\x33\x44", 4);
		*len = 4;
		return true;
	}
	if (s->b_request == 0xa0 && *len == 4) {
		memcpy(vendor_out, data, 4);
		return true;
	}
	return false;
}

static UsbResult fake_in(UsbDevice *d, unsigned ep, uint8_t *buf, size_t max,
                         size_t *len)
{
	(void)d;
	(void)max;
	if (ep != 1)
		return USB_STALL;
	memcpy(buf, "ok", 2);
	*len = 2;
	return USB_ACK;
}

static void fake_configured(UsbDevice *d, unsigned config)
{
	(void)d;
	configured_with = config;
}

static const UsbDeviceClass fake = {
	.device = device_desc,
	.config = config_desc,
	.alternates = 2,
	.request = fake_request,
	.in = fake_in,
	.configured = fake_configured,
};

/* A control transfer: SETUP, the data stage, the status stage. */
static UsbResult control(UsbDevice *d, uint8_t type, uint8_t request,
                         uint16_t value, uint16_t index, uint16_t length,
                         uint8_t *data, size_t *got)
{
	uint8_t setup[8] = {type,
	                    request,
	                    (uint8_t)value,
	                    (uint8_t)(value >> 8),
	                    (uint8_t)index,
	                    (uint8_t)(index >> 8),
	                    (uint8_t)length,
	                    (uint8_t)(length >> 8)};
	UsbResult r;
	size_t len, done = 0;

	CHECK_EQ(usb_device_setup(d, setup), USB_ACK);
	if (type & USB_DIR_IN) {
		while (done < length) {
			r = usb_device_in(d, 0, data + done, 8, &len);
			if (r != USB_ACK)
				return r;
			done += len;
			if (len < 8)
				break;
		}
		*got = done;
		return usb_device_out(d, 0, NULL, 0);
	}
	if (length != 0) {
		r = usb_device_out(d, 0, data, length);
		if (r != USB_ACK)
			return r;
	}
	return usb_device_in(d, 0, NULL, 0, &len);
}

/* Enumeration: descriptors, an address after the status stage, a config. */
static void test_enumeration(void)
{
	UsbDevice d;
	uint8_t buf[64];
	size_t got;

	usb_device_init(&d, &fake, NULL, USB_SPEED_HIGH);
	CHECK_EQ(usb_device_max_packet0(&d), 64);
	CHECK_EQ(control(&d, 0x80, 6, 0x0100, 0, 64, buf, &got), USB_ACK);
	CHECK_EQ(got, 18);
	CHECK(memcmp(buf, device_desc, 18) == 0);
	CHECK_EQ(control(&d, 0x80, 6, 0x0100, 0, 8, buf, &got), USB_ACK);
	CHECK_EQ(got, 8);
	/* SET_ADDRESS takes effect after the status stage. */
	uint8_t set_address[8] = {0, 5, 7, 0, 0, 0, 0, 0};
	size_t len;

	CHECK_EQ(usb_device_setup(&d, set_address), USB_ACK);
	CHECK_EQ(d.address, 0);
	CHECK(usb_device_find(&d, 0) == &d);
	CHECK_EQ(usb_device_in(&d, 0, buf, 8, &len), USB_ACK);
	CHECK_EQ(len, 0);
	CHECK_EQ(d.address, 7);
	CHECK(usb_device_find(&d, 7) == &d);
	CHECK(usb_device_find(&d, 0) == NULL);
	CHECK_EQ(control(&d, 0, 5, 128, 0, 0, NULL, &got), USB_STALL);

	CHECK_EQ(control(&d, 0x80, 6, 0x0200, 0, 255, buf, &got), USB_ACK);
	CHECK_EQ(got, 25);
	CHECK(memcmp(buf, config_desc, 25) == 0);
	CHECK_EQ(control(&d, 0x80, 6, 0x0201, 0, 255, buf, &got), USB_STALL);
	CHECK_EQ(control(&d, 0x80, 6, 0x0300, 0, 255, buf, &got), USB_STALL);
	/* The device qualifier of a high-speed device. */
	CHECK_EQ(control(&d, 0x80, 6, 0x0600, 0, 10, buf, &got), USB_ACK);
	CHECK(memcmp(buf, "\x0a\x06\x00\x02\xff\x00\x00\x40\x01\x00", 10) == 0);

	/* Endpoints other than 0 wait for a configuration. */
	CHECK_EQ(usb_device_in(&d, 1, buf, 512, &len), USB_STALL);
	CHECK_EQ(control(&d, 0, 9, 2, 0, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(&d, 0, 9, 3, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(configured_with, 3);
	CHECK_EQ(control(&d, 0x80, 8, 0, 0, 1, buf, &got), USB_ACK);
	CHECK_EQ(buf[0], 3);
	CHECK_EQ(usb_device_in(&d, 1, buf, 512, &len), USB_ACK);
	CHECK(len == 2 && memcmp(buf, "ok", 2) == 0);
	CHECK_EQ(usb_device_in(&d, 16, buf, 512, &len), USB_STALL);
	CHECK_EQ(usb_device_out(&d, 1, buf, 2), USB_STALL); /* no out() */

	/* A bus reset: address 0, not configured. */
	usb_device_reset(&d);
	CHECK_EQ(d.address, 0);
	CHECK_EQ(d.config, 0);
	CHECK_EQ(usb_device_in(&d, 1, buf, 512, &len), USB_STALL);
}

/* GET_STATUS, the halt feature and the alternate settings. */
static void test_status_and_features(void)
{
	UsbDevice d;
	uint8_t buf[64];
	size_t got, len;

	usb_device_init(&d, &fake, NULL, USB_SPEED_FULL);
	CHECK_EQ(control(&d, 0x80, 0, 0, 0, 2, buf, &got), USB_ACK);
	CHECK(got == 2 && buf[0] == 1 && buf[1] == 0); /* self-powered */
	CHECK_EQ(control(&d, 0x81, 0, 0, 0, 2, buf, &got), USB_STALL);
	CHECK_EQ(control(&d, 0x80, 6, 0x0600, 0, 10, buf, &got), USB_STALL);
	CHECK_EQ(control(&d, 0, 9, 3, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(&d, 0x81, 0, 0, 0, 2, buf, &got), USB_ACK);
	CHECK_EQ(control(&d, 0x81, 0, 0, 1, 2, buf, &got), USB_STALL);
	/* ENDPOINT_HALT on endpoint 1 IN. */
	CHECK_EQ(control(&d, 0x02, 3, 0, 0x81, 0, NULL, &got), USB_ACK);
	CHECK_EQ(usb_device_in(&d, 1, buf, 512, &len), USB_STALL);
	CHECK_EQ(control(&d, 0x82, 0, 0, 0x81, 2, buf, &got), USB_ACK);
	CHECK_EQ(buf[0], 1);
	CHECK_EQ(control(&d, 0x02, 1, 0, 0x81, 0, NULL, &got), USB_ACK);
	CHECK_EQ(usb_device_in(&d, 1, buf, 512, &len), USB_ACK);
	CHECK_EQ(control(&d, 0x02, 3, 1, 0x81, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(&d, 0x02, 3, 0, 0x181, 0, NULL, &got), USB_STALL);
	/* Alternate settings 0 and 1. */
	CHECK_EQ(control(&d, 0x01, 11, 1, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(&d, 0x81, 10, 0, 0, 1, buf, &got), USB_ACK);
	CHECK_EQ(buf[0], 1);
	CHECK_EQ(control(&d, 0x01, 11, 2, 0, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(&d, 0x01, 11, 0, 1, 0, NULL, &got), USB_STALL);
	/* A request nobody knows. */
	CHECK_EQ(control(&d, 0x80, 42, 0, 0, 2, buf, &got), USB_STALL);
}

/* Class and vendor requests go to the device, with their data stage. */
static void test_vendor_requests(void)
{
	UsbDevice d;
	uint8_t buf[64] = {0};
	size_t got;

	usb_device_init(&d, &fake, NULL, USB_SPEED_HIGH);
	CHECK_EQ(control(&d, 0xc0, 0xa1, 0, 0x100, 4, buf, &got), USB_ACK);
	CHECK(got == 4 && memcmp(buf, "\x11\x22\x33\x44", 4) == 0);
	/* Shorter than asked: what the host asks for. */
	CHECK_EQ(control(&d, 0xc0, 0xa1, 0, 0x100, 2, buf, &got), USB_ACK);
	CHECK_EQ(got, 2);
	memcpy(buf, "abcd", 4);
	CHECK_EQ(control(&d, 0x40, 0xa0, 0, 0x100, 4, buf, &got), USB_ACK);
	CHECK(memcmp(vendor_out, "abcd", 4) == 0);
	CHECK_EQ(control(&d, 0x40, 0xa0, 0, 0x100, 3, buf, &got), USB_STALL);
	/* Too much data in the data stage. */
	uint8_t setup[8] = {0x40, 0xa0, 0, 0, 0, 1, 4, 0};

	CHECK_EQ(usb_device_setup(&d, setup), USB_ACK);
	CHECK_EQ(usb_device_out(&d, 0, buf, 8), USB_STALL);
	/* Longer than the device takes: a STALL. */
	uint8_t big[8] = {0x40, 0xa0, 0, 0, 0, 1, 0x01, 0x10};

	CHECK_EQ(usb_device_setup(&d, big), USB_ACK);
	CHECK_EQ(usb_device_out(&d, 0, buf, 8), USB_STALL);
	/* An IN with no transfer, and a new SETUP during one. */
	size_t len;

	CHECK_EQ(usb_device_in(&d, 0, buf, 8, &len), USB_STALL);
	CHECK_EQ(control(&d, 0xc0, 0xa1, 0, 0x100, 4, buf, &got), USB_ACK);
}

int main(void)
{
	RUN_TEST(test_enumeration);
	RUN_TEST(test_status_and_features);
	RUN_TEST(test_vendor_requests);
	return test_summary();
}
