/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the USB 2.0 hub (src/hw/usb/usb_hub.c): chapter 11 of
 * the USB 2.0 specification, with the LAN9514's configuration.
 */

#include <stdint.h>
#include <string.h>

#include "core/event.h"
#include "core/machine.h"
#include "hw/usb/usb_hub.h"
#include "test.h"

#define MS 1000000

static const UsbHubConfig lan9514 = {
	.vendor = 0x0424,
	.product = 0x9514,
	.release = 0x0200,
	.ports = 5,
	.removable = 0x02,
	.characteristics = 0x000d,
	.power_on_time = 0x32,
	.controller_current = 2,
	.max_power = 1,
	.self_powered = true,
	.multi_tt = true,
};

/* A device for port 1, which only counts its resets. */
static const uint8_t leaf_device[18] = {18, 1, 0,    2, 0xff, 0, 0, 64, 0x24,
                                        4,  0, 0xec, 0, 1,    0, 0, 0,  1};
static const uint8_t leaf_config[9] = {9, 2, 9, 0, 0, 1, 0, 0xc0, 1};
static unsigned leaf_resets;

static void leaf_reset(UsbDevice *d)
{
	(void)d;
	leaf_resets++;
}

static const UsbDeviceClass leaf = {
	.device = leaf_device,
	.config = leaf_config,
	.alternates = 1,
	.reset = leaf_reset,
};

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
	size_t len, done = 0;
	UsbResult r;

	CHECK_EQ(usb_device_setup(d, setup), USB_ACK);
	if (type & 0x80) {
		while (done < length) {
			r = usb_device_in(d, 0, data + done, 64, &len);
			if (r != USB_ACK)
				return r;
			done += len;
			if (len < 64)
				break;
		}
		*got = done;
		return usb_device_out(d, 0, NULL, 0);
	}
	return usb_device_in(d, 0, NULL, 0, &len);
}

static uint32_t port_status(UsbDevice *d, unsigned port)
{
	uint8_t buf[4];
	size_t got;

	CHECK_EQ(control(d, 0xa3, 0, 0, (uint16_t)port, 4, buf, &got), USB_ACK);
	CHECK_EQ(got, 4);
	return (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 |
	       (uint32_t)buf[3] << 24;
}

/* The descriptors that 11.23 asks for, with the LAN9514's values. */
static void test_descriptors(void)
{
	static const uint8_t device[18] = {18,   1,    0x00, 0x02, 9,    0,
	                                   2,    64,   0x24, 0x04, 0x14, 0x95,
	                                   0x00, 0x02, 0,    0,    0,    1};
	static const uint8_t config[41] = {
		9, 2, 41, 0, 1, 1, 0,    0xc0, 1,    9, 4,  0,      0,  1,
		9, 0, 1,  0, 7, 5, 0x81, 3,    1,    0, 12, /**/ 9, 4,  0,
		1, 1, 9,  0, 2, 0, 7,    5,    0x81, 3, 1,  0,      12,
	};
	static const uint8_t hub[9] = {9, 0x29, 5, 0x0d, 0, 0x32, 2, 0x02, 0xff};
	Machine *m = machine_create();
	UsbHub *h = usb_hub_create(m, &lan9514);
	UsbDevice *d = usb_hub_device(h);
	uint8_t buf[64];
	size_t got;

	CHECK(h != NULL);
	CHECK_EQ(d->speed, USB_SPEED_HIGH);
	CHECK_EQ(control(d, 0x80, 6, 0x0100, 0, 18, buf, &got), USB_ACK);
	CHECK(got == 18 && memcmp(buf, device, 18) == 0);
	CHECK_EQ(control(d, 0x80, 6, 0x0200, 0, 64, buf, &got), USB_ACK);
	CHECK(got == 41 && memcmp(buf, config, 41) == 0);
	/* Class requests wait for the configuration. */
	CHECK_EQ(control(d, 0xa0, 6, 0x2900, 0, 9, buf, &got), USB_STALL);
	CHECK_EQ(control(d, 0, 9, 1, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(d, 0xa0, 6, 0x2900, 0, 64, buf, &got), USB_ACK);
	CHECK(got == 9 && memcmp(buf, hub, 9) == 0);
	CHECK_EQ(control(d, 0xa0, 6, 0x2a00, 0, 64, buf, &got), USB_STALL);
	CHECK_EQ(control(d, 0xa0, 0, 0, 0, 4, buf, &got), USB_ACK);
	CHECK(got == 4 && memcmp(buf, "\0\0\0\0", 4) == 0);
	CHECK_EQ(control(d, 0x20, 1, 0, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(d, 0x20, 3, 0, 0, 0, NULL, &got), USB_STALL);
	/* Transaction translators: nothing kept, so nothing to clear. */
	CHECK_EQ(control(d, 0x23, 8, 0, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(d, 0x23, 9, 0, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(d, 0xa3, 10, 0, 1, 4, buf, &got), USB_STALL);
	CHECK_EQ(control(d, 0x40, 1, 0, 0, 0, NULL, &got), USB_STALL);
	CHECK(usb_hub_create(m, &(UsbHubConfig){.ports = 8}) == NULL);
	CHECK(usb_hub_create(m, &(UsbHubConfig){.ports = 0}) == NULL);
	usb_hub_destroy(h);
	machine_destroy(m);
}

/*
 * A port: power shows the device; a reset of 10 ms enables the port and
 * resets the device; the change bits until the host clears them; the
 * status change endpoint; the routing to the device.
 */
static void test_ports(void)
{
	Machine *m = machine_create();
	UsbHub *h = usb_hub_create(m, &lan9514);
	UsbDevice *d = usb_hub_device(h), eth;
	uint8_t buf[8];
	size_t got, len;
	uint64_t t;

	usb_device_init(&eth, &leaf, NULL, USB_SPEED_HIGH);
	usb_hub_attach(h, 1, &eth);
	usb_hub_attach(h, 9, &eth); /* no such port: ignored */
	CHECK_EQ(control(d, 0, 5, 3, 0, 0, NULL, &got), USB_ACK); /* address */
	CHECK_EQ(control(d, 0, 9, 1, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(port_status(d, 1), 0); /* no power */
	CHECK_EQ(usb_device_in(d, 1, buf, 1, &len), USB_NAK);
	CHECK_EQ(control(d, 0x23, 3, 8, 1, 0, NULL, &got), USB_ACK); /* power */
	CHECK_EQ(control(d, 0x23, 3, 8, 2, 0, NULL, &got), USB_ACK);
	CHECK_EQ(port_status(d, 1), 0x00010101); /* connected; C_CONNECTION */
	CHECK_EQ(port_status(d, 2), 0x00000100); /* nothing on port 2 */
	CHECK_EQ(usb_device_in(d, 1, buf, 1, &len), USB_ACK);
	CHECK(len == 1 && buf[0] == 0x02);
	CHECK_EQ(usb_device_in(d, 1, buf, 0, &len), USB_STALL);
	CHECK_EQ(usb_device_in(d, 2, buf, 1, &len), USB_STALL);
	CHECK_EQ(control(d, 0x23, 1, 16, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(port_status(d, 1), 0x00000101);
	CHECK_EQ(usb_device_in(d, 1, buf, 1, &len), USB_NAK);
	CHECK(usb_device_find(d, 0) == NULL); /* not enabled yet */

	/* Reset: 10 ms, then enabled, high speed, the device reset. */
	leaf_resets = 0;
	t = m->events.now;
	CHECK_EQ(control(d, 0x23, 3, 4, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(port_status(d, 1), 0x00000111);
	event_queue_run_until(&m->events, t + 10 * MS - 1);
	CHECK_EQ(port_status(d, 1), 0x00000111);
	event_queue_run_until(&m->events, t + 10 * MS);
	CHECK_EQ(port_status(d, 1), 0x00100503);
	CHECK_EQ(leaf_resets, 1);
	CHECK(usb_device_find(d, 0) == &eth);
	CHECK(usb_device_find(d, 5) == NULL);
	CHECK_EQ(control(d, 0x23, 1, 20, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(port_status(d, 1), 0x00000503);
	CHECK_EQ(control(d, 0x23, 1, 17, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(d, 0x23, 1, 18, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(d, 0x23, 1, 19, 1, 0, NULL, &got), USB_ACK);
	/* Not set by the host: enable, the change bits; port numbers. */
	CHECK_EQ(control(d, 0x23, 3, 1, 1, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(d, 0x23, 3, 16, 1, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(d, 0x23, 3, 19, 1, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(d, 0x23, 1, 4, 1, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(d, 0x23, 3, 2, 1, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(d, 0x23, 3, 8, 6, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(d, 0x23, 3, 8, 0, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(d, 0xa3, 0, 0, 6, 4, buf, &got), USB_STALL);
	CHECK_EQ(control(d, 0xa2, 0, 0, 1, 4, buf, &got), USB_STALL);
	CHECK_EQ(control(d, 0x23, 0, 0, 1, 0, NULL, &got), USB_STALL);

	/* Disabled by the host: no route. */
	CHECK_EQ(control(d, 0x23, 1, 1, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(port_status(d, 1), 0x00000501);
	CHECK(usb_device_find(d, 0) == NULL);
	/* Power off: disconnected, and a change; a reset without power. */
	CHECK_EQ(control(d, 0x23, 1, 8, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(port_status(d, 1), 0x00010000);
	CHECK_EQ(control(d, 0x23, 3, 4, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(port_status(d, 1), 0x00010000);

	/* A reset of the hub ends a port reset and powers the ports off. */
	CHECK_EQ(control(d, 0x23, 3, 8, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(d, 0x23, 3, 4, 1, 0, NULL, &got), USB_ACK);
	usb_device_reset(d);
	event_queue_run_until(&m->events, m->events.now + 20 * MS);
	CHECK_EQ(d->address, 0);
	CHECK_EQ(control(d, 0, 9, 1, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(port_status(d, 1), 0);
	/* Destroyed in the middle of a port reset. */
	CHECK_EQ(control(d, 0x23, 3, 8, 1, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(d, 0x23, 3, 4, 1, 0, NULL, &got), USB_ACK);
	usb_hub_destroy(h);
	event_queue_run_until(&m->events, m->events.now + 20 * MS);
	machine_destroy(m);
}

int main(void)
{
	RUN_TEST(test_descriptors);
	RUN_TEST(test_ports);
	return test_summary();
}
