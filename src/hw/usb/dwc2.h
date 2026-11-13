/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A Synopsys DesignWare USB 2.0 Hi-Speed On-The-Go controller (DWC2), as
 * a host with one root port.
 *
 * Synopsys does not publish the controller's databook. Its registers
 * behave here as the drivers use them: Linux's drivers/usb/dwc2 (hw.h,
 * core.c, hcd.c) and U-Boot's drivers/usb/host/dwc2.c. The identity and
 * hardware configuration registers hold the values of the board's
 * controller, which the board gives.
 *
 * What is modelled: the core's resets, host mode (the ID pin of an
 * A-device, as on boards whose port is a host only), the FIFO size
 * registers, the root port (power, connection, a reset that software
 * ends, enable; a high-speed device), the frame number, and the host
 * channels with buffer DMA: a transfer goes packet by packet to the
 * device, and ends with the transfer done, a NAK, a STALL, a babble or a
 * transaction error, as the drivers expect, after the time its packets
 * take at 480 Mb/s. Device mode, the data FIFOs (slave mode), split
 * transactions and isochronous endpoints stop the machine.
 */

#ifndef HW_USB_DWC2_H
#define HW_USB_DWC2_H

#include <stdint.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/usb/usb.h"

#define DWC2_MMIO_SIZE 0x10000
#define DWC2_NUM_CHANNELS_MAX 16

/* The read-only identity of a controller. */
typedef struct Dwc2Config {
	uint32_t guid; /* its reset value; software may change it */
	uint32_t snpsid;
	uint32_t hwcfg1, hwcfg2, hwcfg3, hwcfg4;
	uint32_t dma_mask; /* the bits of a DMA address that reach memory */
} Dwc2Config;

typedef struct Dwc2 Dwc2;

/* A controller; irq is its interrupt line. NULL when memory runs out. */
Dwc2 *dwc2_create(Machine *m, const Dwc2Config *config, IrqLine *irq);
void dwc2_destroy(Dwc2 *dwc2);
void dwc2_reset(Dwc2 *dwc2);
BusStatus dwc2_map(Dwc2 *dwc2, Bus *bus, const char *name, uint64_t base);

/* Attach dev to the root port, for good. */
void dwc2_attach(Dwc2 *dwc2, UsbDevice *dev);

#endif /* HW_USB_DWC2_H */
