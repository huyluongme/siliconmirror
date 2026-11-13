/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * USB devices as a host controller sees them: transactions to an address
 * and an endpoint, from the USB 2.0 specification. A device answers each
 * transaction at once: ACK with data, NAK when it has nothing, STALL.
 *
 * The device framework of chapter 9 is common to every device: the
 * states (Default, Address, Configured, 9.1), the three stages of a
 * control transfer on endpoint 0 (8.5.3), and the standard requests
 * (9.4): GET_STATUS, CLEAR_FEATURE, SET_FEATURE (ENDPOINT_HALT, and
 * DEVICE_REMOTE_WAKEUP when the configuration has it), SET_ADDRESS,
 * GET_DESCRIPTOR (device, configuration, device qualifier), GET_CONFIGURATION,
 * SET_CONFIGURATION, GET_INTERFACE, SET_INTERFACE. A device brings its
 * descriptors and its class or vendor requests; a request that neither knows
 * gets a STALL, as 9.2.7 asks.
 */

#ifndef HW_USB_USB_H
#define HW_USB_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum UsbSpeed {
	USB_SPEED_HIGH,
	USB_SPEED_FULL,
	USB_SPEED_LOW
} UsbSpeed;

/* The handshake of a transaction, or no answer at all. */
typedef enum UsbResult {
	USB_ACK,
	USB_NAK,
	USB_STALL,
	USB_NO_RESPONSE,
} UsbResult;

/* A SETUP packet's fields (9.3). */
typedef struct UsbSetup {
	uint8_t bm_request_type, b_request;
	uint16_t w_value, w_index, w_length;
} UsbSetup;

#define USB_DIR_IN 0x80
#define USB_TYPE_MASK 0x60
#define USB_TYPE_STANDARD 0x00
#define USB_TYPE_CLASS 0x20
#define USB_TYPE_VENDOR 0x40
#define USB_RECIP_MASK 0x1f
#define USB_RECIP_DEVICE 0
#define USB_RECIP_INTERFACE 1
#define USB_RECIP_ENDPOINT 2
#define USB_RECIP_OTHER 3

#define USB_CONTROL_MAX 1024 /* the longest data stage a device takes */
#define USB_ENDPOINTS 16

typedef struct UsbDevice UsbDevice;

typedef struct UsbDeviceClass {
	const uint8_t *device; /* the device descriptor, 18 bytes */
	const uint8_t *config; /* the configuration, wTotalLength bytes */
	unsigned alternates;   /* alternate settings of interface 0 */
	/*
	 * A class or vendor request. An IN request writes up to *len bytes
	 * to data and sets *len; an OUT request gets its *len bytes there.
	 * Returns false for a STALL.
	 */
	bool (*request)(UsbDevice *d, const UsbSetup *s, uint8_t *data,
	                size_t *len);
	/* A transaction on endpoint ep, 1 to 15, IN or OUT. */
	UsbResult (*in)(UsbDevice *d, unsigned ep, uint8_t *buf, size_t max,
	                size_t *len);
	UsbResult (*out)(UsbDevice *d, unsigned ep, const uint8_t *buf, size_t len);
	/* The device behind a hub's ports with address, or NULL. */
	UsbDevice *(*downstream)(UsbDevice *d, unsigned address);
	/* A bus reset, or the configuration that SET_CONFIGURATION chose. */
	void (*reset)(UsbDevice *d);
	void (*configured)(UsbDevice *d, unsigned config);
	/* True while the device NAKs every transaction but SETUP. */
	bool (*busy)(UsbDevice *d);
} UsbDeviceClass;

typedef enum UsbControlStage {
	USB_CONTROL_IDLE,
	USB_CONTROL_DATA_IN,
	USB_CONTROL_DATA_OUT,
	USB_CONTROL_STATUS_IN,  /* a zero-length IN ends it */
	USB_CONTROL_STATUS_OUT, /* a zero-length OUT ends it */
	USB_CONTROL_STALLED,
} UsbControlStage;

struct UsbDevice {
	const UsbDeviceClass *cls;
	void *ctx;
	UsbSpeed speed;

	uint8_t address;
	uint8_t config;                /* 0: not configured */
	uint8_t alternate;             /* of interface 0 */
	bool remote_wakeup;            /* DEVICE_REMOTE_WAKEUP (9.4.5) */
	bool halted[USB_ENDPOINTS][2]; /* [ep][1 for IN] */

	/* The control transfer in progress on endpoint 0. */
	UsbControlStage stage;
	UsbSetup setup;
	uint8_t data[USB_CONTROL_MAX];
	size_t len, pos;
	bool pending;    /* an IN request, carried out at its first IN */
	int new_address; /* SET_ADDRESS, after its status stage; or -1 */
};

/* A device of cls, in the Default state. */
void usb_device_init(UsbDevice *d, const UsbDeviceClass *cls, void *ctx,
                     UsbSpeed speed);

/* A bus reset: the Default state, address 0, no configuration. */
void usb_device_reset(UsbDevice *d);

/* The transactions of the host, to this device. */
UsbResult usb_device_setup(UsbDevice *d, const uint8_t packet[8]);
UsbResult usb_device_in(UsbDevice *d, unsigned ep, uint8_t *buf, size_t max,
                        size_t *len);
UsbResult usb_device_out(UsbDevice *d, unsigned ep, const uint8_t *buf,
                         size_t len);

/* The device with address at d or behind it, or NULL. */
UsbDevice *usb_device_find(UsbDevice *d, unsigned address);

/* wMaxPacketSize of endpoint 0, from the device descriptor. */
unsigned usb_device_max_packet0(const UsbDevice *d);

#endif /* HW_USB_USB_H */
