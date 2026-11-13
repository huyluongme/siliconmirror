/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The Ethernet controller of the LAN9500, LAN9512 and LAN9514: a USB 2.0
 * high-speed device with a 10/100 MAC and PHY (Linux's and U-Boot's
 * smsc95xx drivers). From the LAN950x datasheet (DS00001875D): the USB
 * descriptors and endpoints (5.3.1), the vendor commands (5.3.3), the RX
 * and TX paths (5.4), address filtering (5.5.3, 5.5.4), the EEPROM
 * controller (5.7.4), the resets (5.13), the registers (7.3, 7.4) and the
 * PHY's registers (7.5). The LAN9500A's extra features are not there.
 *
 * There is no EEPROM, as on the boards that hold the chip: the
 * descriptors and registers keep their defaults, the MAC address is
 * FF:FF:FF:FF:FF:FF, and the EEPROM's data line reads low, so reads give
 * 0 and writes time out.
 *
 * The cable: with none plugged in the link stays down. A cable goes to a
 * partner that negotiates 10 or 100 Mb/s, half or full duplex; frames
 * cross it whole, with their FCS, at once.
 *
 * Not modelled, and the machine stops when software uses them: the soft
 * reset (SRST, a USB disconnect), the data port, the debug registers, the
 * wake-up frame filter and wake-up detection, the checksum offload
 * engines, pause frames on demand, the PHY's loopback and collision test.
 * GPIO pins are inputs that read 1 (pulled up) and never interrupt.
 */

#ifndef HW_NET_LAN95XX_H
#define HW_NET_LAN95XX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/machine.h"
#include "hw/usb/usb.h"

/* What a chip's descriptors and identity registers hold. */
typedef struct Lan95xxConfig {
	uint16_t vendor, product, release;
	uint8_t device_protocol;    /* bDeviceProtocol */
	uint16_t chip_id, chip_rev; /* ID_REV */
	uint16_t phy_id1, phy_id2;  /* PHY registers 2 and 3 */
	uint8_t max_power;          /* bMaxPower, in 2 mA */
	uint8_t interval;           /* the interrupt endpoint's bInterval */
	bool self_powered, remote_wakeup;
} Lan95xxConfig;

typedef struct Lan95xx Lan95xx;

/* What gets the frames sent on the cable, FCS included. */
typedef void Lan95xxSend(void *ctx, const uint8_t *frame, size_t len);

/* A controller that takes the time from m. NULL when memory runs out. */
Lan95xx *lan95xx_create(Machine *m, const Lan95xxConfig *config);
void lan95xx_destroy(Lan95xx *e);

/* A power-on reset of the whole chip. */
void lan95xx_reset(Lan95xx *e);

/* The controller, as a device on its upstream port. */
UsbDevice *lan95xx_device(Lan95xx *e);

/* Plug in a cable to a partner that gets the frames; NULL unplugs it. */
void lan95xx_plug(Lan95xx *e, Lan95xxSend *send, void *ctx);

/* A frame from the cable, FCS included. False when the controller drops it. */
bool lan95xx_receive(Lan95xx *e, const uint8_t *frame, size_t len);

#endif /* HW_NET_LAN95XX_H */
