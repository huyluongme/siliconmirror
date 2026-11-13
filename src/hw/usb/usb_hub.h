/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A USB 2.0 high-speed hub, from chapter 11 of the USB 2.0
 * specification: the hub descriptor (11.23.2.1), the hub class requests
 * (11.24.2), the port states that the host sees through wPortStatus and
 * wPortChange (11.24.2.7), and the status change endpoint (11.12.4).
 *
 * A port is powered off until the host powers it. A device on a powered
 * port shows as connected; a port reset of 10 ms (TDRST, 7.1.7.5) resets
 * the device and enables the port. Transactions to a device go through
 * the enabled ports. Only high-speed devices are attached: the
 * transaction translators that full- and low-speed devices need are not
 * modelled, nor are suspend, over-current and the port indicators.
 */

#ifndef HW_USB_USB_HUB_H
#define HW_USB_USB_HUB_H

#include <stdbool.h>
#include <stdint.h>

#include "core/machine.h"
#include "hw/usb/usb.h"

#define USB_HUB_MAX_PORTS 7

/* What a hub's descriptors say. */
typedef struct UsbHubConfig {
	uint16_t vendor, product, release;
	unsigned ports;             /* 1 to USB_HUB_MAX_PORTS */
	uint8_t removable;          /* DeviceRemovable: bit n for port n */
	uint16_t characteristics;   /* wHubCharacteristics */
	uint8_t power_on_time;      /* bPwrOn2PwrGood, in 2 ms */
	uint8_t controller_current; /* bHubContrCurrent, in mA */
	uint8_t max_power;          /* bMaxPower, in 2 mA */
	bool self_powered;
	bool multi_tt; /* one transaction translator a port */
} UsbHubConfig;

typedef struct UsbHub UsbHub;

/* A hub that takes the time from m. NULL when memory runs out. */
UsbHub *usb_hub_create(Machine *m, const UsbHubConfig *config);
void usb_hub_destroy(UsbHub *hub);

/* The hub, as a device on its upstream port. */
UsbDevice *usb_hub_device(UsbHub *hub);

/* Attach dev to port, 1 to the number of ports, for good. */
void usb_hub_attach(UsbHub *hub, unsigned port, UsbDevice *dev);

#endif /* HW_USB_USB_HUB_H */
