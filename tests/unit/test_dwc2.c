/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the DWC2 USB controller as a host (src/hw/usb/dwc2.c),
 * with nothing on its port, as Linux's and U-Boot's drivers use it.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/event.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/usb/dwc2.h"
#include "hw/usb/usb_hub.h"
#include "test.h"
#include "util/macros.h"

#define BASE 0x3f980000

enum {
	GOTGCTL = 0x000,
	GAHBCFG = 0x008,
	GUSBCFG = 0x00c,
	GRSTCTL = 0x010,
	GINTSTS = 0x014,
	GINTMSK = 0x018,
	GRXFSIZ = 0x024,
	GNPTXFSIZ = 0x028,
	GNPTXSTS = 0x02c,
	GOTGINT = 0x004,
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
	HPTXSTS = 0x410,
	HFNUM = 0x408,
	HAINT = 0x414,
	HAINTMSK = 0x418,
	HPRT0 = 0x440,
	HCCHAR0 = 0x500,
	HCINT0 = 0x508,
	HCINTMSK0 = 0x50c,
	PCGCCTL = 0xe00,
};

#define CHENA (1u << 31)
#define CHDIS (1u << 30)
#define CHHLTD (1u << 1)

/* The values of the Raspberry Pi's controller. */
static const Dwc2Config config = {
	.guid = 0x2708a000,
	.snpsid = 0x4f54280a,
	.hwcfg1 = 0,
	.hwcfg2 = 0x228ddd50,
	.hwcfg3 = 0x0ff000e8,
	.hwcfg4 = 0x1ff00020,
	.dma_mask = 0x3fffffff,
};

typedef struct Setup {
	Machine *m;
	IrqLine irq;
	Dwc2 *d;
} Setup;

static void setup(Setup *s)
{
	memset(s, 0, sizeof(*s));
	s->m = machine_create();
	CHECK(s->m != NULL);
	irq_init(&s->irq, NULL, NULL, 0);
	s->d = dwc2_create(s->m, &config, &s->irq);
	CHECK(s->d != NULL);
	CHECK_EQ(dwc2_map(s->d, s->m->bus, "usb", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	dwc2_destroy(s->d);
	machine_destroy(s->m);
}

static uint32_t rd(Setup *s, uint64_t offset)
{
	uint64_t v = 0;

	CHECK_EQ(bus_read(s->m->bus, BASE + offset, 4, &v), BUS_OK);
	return (uint32_t)v;
}

static void wr(Setup *s, uint64_t offset, uint32_t v)
{
	CHECK_EQ(bus_write(s->m->bus, BASE + offset, 4, v), BUS_OK);
}

/* The identity, host mode, and the FIFO sizes at reset. */
static void test_identity(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, GSNPSID), 0x4f54280a);
	CHECK_EQ(rd(&s, GHWCFG1), 0);
	CHECK_EQ(rd(&s, GHWCFG2), 0x228ddd50);
	CHECK_EQ(rd(&s, GHWCFG3), 0x0ff000e8);
	CHECK_EQ(rd(&s, GHWCFG4), 0x1ff00020);
	CHECK_EQ(rd(&s, GUID), 0x2708a000);
	wr(&s, GUID, 0x1234);
	CHECK_EQ(rd(&s, GUID), 0x1234);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	wr(&s, GSNPSID, 0); /* read-only: stops */
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	s.m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(rd(&s, GINTSTS), 1); /* CURMODE: host */
	CHECK_EQ(rd(&s, GOTGCTL), 0); /* ID: an A-device */
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	wr(&s, GOTGCTL, 1u << 16);
	CHECK_EQ(rd(&s, GOTGCTL), 0);
	CHECK_EQ(rd(&s, GRSTCTL), 1u << 31); /* AHB idle */
	CHECK_EQ(rd(&s, GRXFSIZ), 0x0ff0);
	CHECK_EQ(rd(&s, GNPTXFSIZ), 0x0ff00ff0);
	CHECK_EQ(rd(&s, HPTXFSIZ), 0x0ff00ff0);
	wr(&s, GRXFSIZ, 0x12345678);
	CHECK_EQ(rd(&s, GRXFSIZ), 0x5678);
	wr(&s, GNPTXFSIZ, 0x01000306);
	CHECK_EQ(rd(&s, GNPTXSTS), 0x00080100); /* empty: all its space */
	CHECK_EQ(rd(&s, HFNUM), 0);
	CHECK(!irq_level(&s.irq));
	teardown(&s);
}

/* The resets and flushes end at once; the soft reset keeps settings. */
static void test_resets(void)
{
	Setup s;

	setup(&s);
	wr(&s, GUSBCFG, 0x1400);
	wr(&s, GRSTCTL, 0x10 << 6 | 0x37);
	CHECK_EQ(rd(&s, GRSTCTL), 1u << 31 | 0x10 << 6);
	CHECK_EQ(rd(&s, GUSBCFG), 0x1400);
	/* Board reset: everything back. */
	wr(&s, HCFG, 1);
	wr(&s, PCGCCTL, 1);
	dwc2_reset(s.d);
	CHECK_EQ(rd(&s, GUSBCFG), 0);
	CHECK_EQ(rd(&s, HCFG), 0);
	CHECK_EQ(rd(&s, PCGCCTL), 0);
	CHECK_EQ(rd(&s, GRSTCTL), 1u << 31);
	teardown(&s);
}

/* The root port with nothing attached: power only. */
static void test_port(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, HPRT0), 0);
	wr(&s, HPRT0, 1u << 12); /* power */
	CHECK_EQ(rd(&s, HPRT0), 1u << 12);
	wr(&s, HPRT0, 1u << 12 | 1u << 8); /* reset */
	CHECK_EQ(rd(&s, HPRT0), 1u << 12 | 1u << 8);
	wr(&s, HPRT0, 1u << 12 | 0x2f); /* RO and W1C bits: nothing */
	CHECK_EQ(rd(&s, HPRT0), 1u << 12);
	CHECK_EQ(rd(&s, GINTSTS), 1);
	teardown(&s);
}

/* Channels: an idle one halts at once; its interrupt reaches the line. */
static void test_channels(void)
{
	Setup s;

	setup(&s);
	for (unsigned n = 0; n < 8; n++) {
		wr(&s, HCCHAR0 + 0x20 * n, CHENA | CHDIS | 0x200);
		CHECK_EQ(rd(&s, HCCHAR0 + 0x20 * n), 0x200);
		CHECK_EQ(rd(&s, HCINT0 + 0x20 * n), CHHLTD);
	}
	CHECK_EQ(rd(&s, HAINT), 0); /* not enabled */
	wr(&s, HCINTMSK0 + 0x20 * 3, CHHLTD);
	CHECK_EQ(rd(&s, HAINT), 1u << 3);
	CHECK_EQ(rd(&s, GINTSTS), 1); /* HAINTMSK off */
	wr(&s, HAINTMSK, 0xffffffff);
	CHECK_EQ(rd(&s, HAINTMSK), 0xff);
	CHECK_EQ(rd(&s, GINTSTS), 1u << 25 | 1);
	wr(&s, GINTMSK, 1u << 25);
	CHECK(!irq_level(&s.irq));
	wr(&s, GAHBCFG, 1);
	CHECK(irq_level(&s.irq));
	wr(&s, HCINT0 + 0x20 * 3, CHHLTD);
	CHECK_EQ(rd(&s, HAINT), 0);
	CHECK(!irq_level(&s.irq));
	/* A soft reset halts the channels too. */
	wr(&s, HCCHAR0, 0x100);
	wr(&s, GRSTCTL, 1);
	CHECK_EQ(rd(&s, HCCHAR0), 0x100);
	teardown(&s);
}

/* Each register keeps what the model keeps of it, and no other changes. */
static void test_register_values(void)
{
	static const struct {
		uint64_t offset;
		uint32_t value, read;
	} regs[] = {
		{GOTGCTL, 0x00000003, 0x00000003},
		{GOTGINT, 0xffffffff, 0},
		{GAHBCFG, 0x00000021, 0x00000021},
		{GUSBCFG, 0x00001407, 0x00001407},
		{GRSTCTL, 0x1fu << 6, 0x1fu << 6 | 1u << 31},
		{GINTSTS, 0xffffffff, 1},
		{GINTMSK, 0x0f000003, 0x0f000003},
		{GRXFSIZ, 0x12345679, 0x00005679},
		{GNPTXFSIZ, 0x01000306, 0x01000306},
		{GI2CCTL, 0x00000011, 0x00000011},
		{GPVNDCTL, 0x00000022, 0x00000022},
		{GGPIO, 0x00330000, 0x00330000},
		{GUID, 0x00001234, 0x00001234},
		{HPTXFSIZ, 0x02000400, 0x02000400},
		{HCFG, 0x00000001, 0x00000001},
		{HFIR, 0x0001ea61, 0x0000ea61},
		{HAINTMSK, 0xffffffff, 0x000000ff},
		{PCGCCTL, 0x00000001, 0x00000001},
		{HCCHAR0 + 0xa0, 0x00d8a200, 0x00d8a200}, /* channel 5 */
		{HCCHAR0 + 0xa4, 0x80000123, 0x80000123},
		{HCINT0 + 0xa0, 0xffffffff, 0},
		{HCINTMSK0 + 0xa0, 0x000007ff, 0x000007ff},
		{HCCHAR0 + 0xb0, 0x80080040, 0x80080040},
		{HCCHAR0 + 0xb4, 0x12345678, 0x12345678},
	};
	const size_t n = ARRAY_SIZE(regs);
	uint32_t base[ARRAY_SIZE(regs)], reset[ARRAY_SIZE(regs)];
	Setup s;

	setup(&s);
	for (size_t i = 0; i < n; i++) {
		uint64_t o = regs[i].offset;

		reset[i] = rd(&s, o);
		/* All 0 at reset but these (GRXFSIZ from GHWCFG3). */
		CHECK_EQ(reset[i], o == GRSTCTL     ? 1u << 31
		                   : o == GINTSTS   ? 1
		                   : o == GRXFSIZ   ? 0x0ff0
		                   : o == GNPTXFSIZ ? 0x0ff00ff0
		                   : o == GUID      ? 0x2708a000
		                   : o == HPTXFSIZ  ? 0x0ff00ff0
		                                    : 0);
	}
	for (size_t i = 0; i < n; i++)
		wr(&s, regs[i].offset, 0);
	for (size_t i = 0; i < n; i++)
		base[i] = rd(&s, regs[i].offset);
	for (size_t i = 0; i < n; i++) {
		wr(&s, regs[i].offset, regs[i].value);
		for (size_t j = 0; j < n; j++)
			CHECK_EQ(rd(&s, regs[j].offset), i == j ? regs[j].read : base[j]);
		wr(&s, regs[i].offset, 0);
	}
	CHECK_EQ(rd(&s, HPTXSTS), 8u << 16); /* the queue: 8 spaces */
	wr(&s, HPTXFSIZ, 0x02000400);
	CHECK_EQ(rd(&s, HPTXSTS), 8u << 16 | 0x0200);
	/* A board reset brings back every value of the reset. */
	for (size_t i = 0; i < n; i++)
		wr(&s, regs[i].offset, regs[i].value);
	dwc2_reset(s.d);
	for (size_t i = 0; i < n; i++)
		CHECK_EQ(rd(&s, regs[i].offset), reset[i]);
	/* A halt of an idle channel sets CHHLTD; HCINT clears what is written. */
	wr(&s, HCCHAR0 + 0x20, CHENA | CHDIS);
	CHECK_EQ(rd(&s, HCINT0 + 0x20), CHHLTD);
	wr(&s, HCINT0 + 0x20, 1u << 4);
	CHECK_EQ(rd(&s, HCINT0 + 0x20), CHHLTD);
	wr(&s, HCCHAR0 + 0x24, CHHLTD); /* HCSPLT, which leaves HCINT */
	CHECK_EQ(rd(&s, HCINT0 + 0x20), CHHLTD);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* What is not modelled stops the machine. */
static void test_stops(void)
{
	static const struct {
		bool write;
		uint64_t offset;
		unsigned size;
		uint32_t value;
		const char *message;
	} stops[] = {
		{true, HCCHAR0, 4, CHENA, "a transfer with the port disabled"},
		{true, GUSBCFG, 4, 1u << 30, "device mode"},
		{false, 0x800, 4, 0, "is not emulated (device mode"},
		{false, 0x1000, 4, 0, "is not emulated (device mode"},
		{false, HCCHAR0 + 0x20 * 8, 4, 0, "is not emulated (device mode"},
		{false, HCCHAR0 + 0x18, 4, 0, "is not emulated (device mode"},
		{false, GSNPSID, 2, 0, "is not a 32-bit register read"},
		{true, HPRT0, 1, 0, "is not a 32-bit register write"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(stops); i++) {
		Setup s;
		uint64_t v;

		setup(&s);
		if (stops[i].write)
			bus_write(s.m->bus, BASE + stops[i].offset, stops[i].size,
			          stops[i].value);
		else
			bus_read(s.m->bus, BASE + stops[i].offset, stops[i].size, &v);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, "dwc2: ") != NULL);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		teardown(&s);
	}

	Setup s;
	uint64_t v;

	setup(&s);
	CHECK_EQ(bus_read(s.m->bus, BASE + 0x10000, 4, &v), BUS_ERR_UNMAPPED);
	teardown(&s);
}

#define HCSPLT0 0x504
#define HCTSIZ0 0x510
#define HCDMA0 0x514
#define RAM 0x100000
#define ALIAS 0xc0000000u /* the uncached bus alias of the RAM */

#define XFERCOMPL (1u << 0)
#define STALLED (1u << 3)
#define NAKED (1u << 4)
#define ACKED (1u << 5)
#define XACTERR (1u << 7)
#define BBLERR (1u << 8)
#define PID_DATA1 (2u << 29)
#define PID_SETUP (3u << 29)

static const UsbHubConfig hub_config = {
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

/* A transfer on channel 0; returns HCINT once it is done. */
static uint32_t transfer(Setup *s, uint32_t hcchar, uint32_t size,
                         uint32_t packets, uint32_t pid, uint64_t *ns)
{
	uint64_t t;

	wr(s, HCTSIZ0, pid | packets << 19 | size);
	wr(s, HCDMA0, ALIAS | RAM);
	wr(s, HCINT0, 0x3fff);
	t = s->m->events.now;
	wr(s, HCCHAR0, CHENA | hcchar);
	CHECK(rd(s, HCCHAR0) & CHENA);
	for (int i = 0; i < 100000 && (rd(s, HCCHAR0) & CHENA); i++)
		event_queue_run_until(&s->m->events, s->m->events.now + 1);
	if (ns != NULL)
		*ns = s->m->events.now - t;
	return rd(s, HCINT0);
}

/* HCCHAR for a device address, an endpoint, IN or not, a type, a size. */
static uint32_t hcchar(unsigned addr, unsigned ep, bool in, unsigned type,
                       unsigned mps)
{
	return addr << 22 | type << 18 | (in ? 1u << 15 : 0) | ep << 11 | mps;
}

/*
 * The hub on the root port: power, a reset that software ends, then
 * control transfers through channel 0 with DMA, as U-Boot does them.
 */
static void test_transfers(void)
{
	static const uint8_t get_device[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
	Setup s;
	UsbHub *hub;
	uint8_t buf[18];
	uint64_t ns;

	setup(&s);
	CHECK_EQ(bus_map_ram(s.m->bus, "ram", 0, 0x200000), BUS_OK);
	hub = usb_hub_create(s.m, &hub_config);
	dwc2_attach(s.d, usb_hub_device(hub));
	CHECK_EQ(rd(&s, HPRT0), 0); /* no power, nothing seen */
	wr(&s, HPRT0, 1u << 12);
	CHECK_EQ(rd(&s, HPRT0), 1u << 12 | 3); /* CONNSTS, CONNDET */
	CHECK_EQ(rd(&s, GINTSTS), 1u << 24 | 1);
	wr(&s, HPRT0, 1u << 12 | 2);
	CHECK_EQ(rd(&s, HPRT0), 1u << 12 | 1);
	CHECK_EQ(rd(&s, HFNUM), 0);
	wr(&s, HPRT0, 1u << 12 | 1u << 8); /* reset */
	CHECK_EQ(rd(&s, HPRT0), 1u << 12 | 1u << 8 | 1);
	event_queue_run_until(&s.m->events, s.m->events.now + 50000000);
	wr(&s, HPRT0, 1u << 12);
	CHECK_EQ(rd(&s, HPRT0), 1u << 12 | 0xd); /* ENA, ENCHNG, high speed */
	wr(&s, HPRT0, 1u << 12 | 8);
	CHECK_EQ(rd(&s, HPRT0), 1u << 12 | 5);
	/* Microframes of 125 us from the enable; the time left, at 60 MHz. */
	event_queue_run_until(&s.m->events, s.m->events.now + 3 * 125000 + 25000);
	CHECK_EQ(rd(&s, HFNUM), 6000u << 16 | 3);

	/* SETUP: GET_DESCRIPTOR(device), 8 bytes, then the data, 18. */
	CHECK_EQ(bus_write_block(s.m->bus, RAM, get_device, 8), BUS_OK);
	CHECK_EQ(transfer(&s, hcchar(0, 0, false, 0, 64), 8, 1, PID_SETUP, &ns),
	         XFERCOMPL | 2 | ACKED);
	CHECK_EQ(ns, 400);                    /* 24 bytes at 480 Mb/s */
	CHECK_EQ(rd(&s, HCTSIZ0), PID_DATA1); /* nothing left; DATA1 next */
	CHECK_EQ(rd(&s, HCDMA0), ALIAS | (RAM + 8));
	CHECK_EQ(transfer(&s, hcchar(0, 0, true, 0, 64), 64, 1, PID_DATA1, &ns),
	         XFERCOMPL | 2 | ACKED);
	CHECK_EQ(rd(&s, HCTSIZ0), 46); /* a short packet: 46 not sent */
	CHECK_EQ(bus_read_block(s.m->bus, RAM, buf, 18), BUS_OK);
	CHECK_EQ(buf[0], 18);
	CHECK(buf[8] == 0x24 && buf[9] == 0x04 && buf[10] == 0x14);
	CHECK_EQ(transfer(&s, hcchar(0, 0, false, 0, 64), 0, 1, PID_DATA1, &ns),
	         XFERCOMPL | 2 | ACKED);
	/* Two packets of 8: the data toggles back to DATA1. */
	CHECK_EQ(bus_write_block(s.m->bus, RAM, get_device, 8), BUS_OK);
	transfer(&s, hcchar(0, 0, false, 0, 8), 8, 1, PID_SETUP, NULL);
	CHECK_EQ(transfer(&s, hcchar(0, 0, true, 0, 8), 16, 2, PID_DATA1, &ns),
	         XFERCOMPL | 2 | ACKED);
	CHECK_EQ(rd(&s, HCTSIZ0), PID_DATA1);
	CHECK_EQ(ns, 2 * 400);
	/* More than the transfer has room for: babble. */
	CHECK_EQ(transfer(&s, hcchar(0, 0, true, 0, 8), 1, 1, PID_DATA1, NULL),
	         BBLERR | 2);

	/* STALL: endpoint 1 before a configuration; NAK once configured. */
	CHECK_EQ(transfer(&s, hcchar(0, 1, true, 3, 1), 1, 1, 0, NULL),
	         STALLED | 2);
	CHECK_EQ(bus_write_block(s.m->bus, RAM, "\0\x09\x01\0\0\0\0\0", 8), BUS_OK);
	transfer(&s, hcchar(0, 0, false, 0, 64), 8, 1, PID_SETUP, NULL);
	transfer(&s, hcchar(0, 0, true, 0, 64), 0, 1, PID_DATA1, NULL);
	CHECK_EQ(transfer(&s, hcchar(0, 1, true, 3, 1), 1, 1, 0, &ns), NAKED | 2);
	CHECK_EQ(ns, 267);
	CHECK_EQ(rd(&s, HCTSIZ0), 1u << 19 | 1); /* nothing moved */
	/* No device at address 5: no answer. */
	CHECK_EQ(transfer(&s, hcchar(5, 0, true, 0, 64), 8, 1, PID_DATA1, &ns),
	         XACTERR | 2);
	CHECK_EQ(ns, 3 * 267);
	/* A SETUP that is not 8 bytes gets no answer either. */
	CHECK_EQ(transfer(&s, hcchar(0, 0, false, 0, 64), 7, 1, PID_SETUP, NULL),
	         XACTERR | 2);

	/* A halt while the transfer runs ends it at once. */
	wr(&s, HCTSIZ0, PID_DATA1 | 1u << 19 | 8);
	wr(&s, HCINT0, 0x3fff);
	wr(&s, HCCHAR0, CHENA | hcchar(5, 0, true, 0, 64));
	wr(&s, HCCHAR0, CHENA | CHDIS | hcchar(5, 0, true, 0, 64));
	CHECK_EQ(rd(&s, HCCHAR0) & CHENA, 0);
	CHECK_EQ(rd(&s, HCINT0), XACTERR | 2);
	/* Another write to it while it runs stops the machine. */
	wr(&s, HCCHAR0, CHENA | hcchar(5, 0, true, 0, 64));
	wr(&s, HCCHAR0, hcchar(5, 0, true, 0, 64));
	CHECK(strstr(s.m->stop_message, "a change to a channel that runs"));
	s.m->stop = MACHINE_STOP_NONE;
	/* A reset of the board ends it too. */
	dwc2_reset(s.d);
	event_queue_run_until(&s.m->events, s.m->events.now + 100000);
	CHECK_EQ(rd(&s, HCINT0), 0);
	CHECK_EQ(rd(&s, HPRT0), 0);

	/* What is not modelled: splits, isochronous, a disabled port. */
	wr(&s, HPRT0, 1u << 12);
	wr(&s, HCTSIZ0, 1u << 19);
	wr(&s, HCCHAR0, CHENA | hcchar(0, 0, true, 0, 64));
	CHECK(strstr(s.m->stop_message, "with the port disabled"));
	s.m->stop = MACHINE_STOP_NONE;
	wr(&s, HPRT0, 1u << 12 | 1u << 8);
	wr(&s, HPRT0, 1u << 12);
	wr(&s, HCSPLT0, 1u << 31);
	wr(&s, HCCHAR0, CHENA | hcchar(0, 0, true, 0, 64));
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	s.m->stop = MACHINE_STOP_NONE;
	wr(&s, HCSPLT0, 0);
	wr(&s, HCCHAR0, CHENA | hcchar(0, 1, true, 1, 64));
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	s.m->stop = MACHINE_STOP_NONE;
	wr(&s, HCTSIZ0, 0);
	wr(&s, HCCHAR0, CHENA | hcchar(0, 0, true, 0, 64));
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	s.m->stop = MACHINE_STOP_NONE;
	/* Power off disables the port; writing ENA does too. */
	wr(&s, HPRT0, 0);
	CHECK_EQ(rd(&s, HPRT0) & 0x1005, 0); /* PWR, CONNSTS, ENA */
	wr(&s, HPRT0, 1u << 12 | 1u << 8);
	wr(&s, HPRT0, 1u << 12);
	wr(&s, HPRT0, 1u << 12 | 4);
	CHECK_EQ(rd(&s, HPRT0) & 4, 0);
	/* Destroyed while a channel runs. */
	wr(&s, HPRT0, 1u << 12 | 1u << 8);
	wr(&s, HPRT0, 1u << 12);
	wr(&s, HCTSIZ0, PID_DATA1 | 1u << 19 | 8);
	wr(&s, HCCHAR0, CHENA | hcchar(5, 0, true, 0, 64));
	dwc2_destroy(s.d);
	event_queue_run_until(&s.m->events, s.m->events.now + 100000);
	usb_hub_destroy(hub);
	machine_destroy(s.m);
}

/* A device with two bulk endpoints: 1 IN, from fake_in; 2 OUT, to fake_out. */
static uint8_t fake_in_data[700], fake_out_data[2048];
static size_t fake_in_pos, fake_out_len;

static UsbResult fake_in(UsbDevice *d, unsigned ep, uint8_t *buf, size_t max,
                         size_t *len)
{
	size_t n = sizeof(fake_in_data) - fake_in_pos;

	(void)d;
	if (ep != 1)
		return USB_STALL;
	n = n < max ? n : max;
	memcpy(buf, fake_in_data + fake_in_pos, n);
	fake_in_pos += n;
	*len = n;
	return USB_ACK;
}

static UsbResult fake_out(UsbDevice *d, unsigned ep, const uint8_t *buf,
                          size_t len)
{
	(void)d;
	if (ep != 2 || fake_out_len + len > sizeof(fake_out_data))
		return USB_STALL;
	memcpy(fake_out_data + fake_out_len, buf, len);
	fake_out_len += len;
	return USB_ACK;
}

static const uint8_t fake_device[18] = {
	18, 1, 0, 2, 0xff, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 0, 0, 0, 1};
static const uint8_t fake_config[32] = {
	9, 2, 32, 0, 1,    1, 0, 0x80, 50, 9, 4, 0,    0, 2, 0xff, 0,
	0, 0, 7,  5, 0x81, 2, 0, 2,    0,  7, 5, 0x02, 2, 0, 2,    0,
};
static const UsbDeviceClass fake_class = {
	.device = fake_device,
	.config = fake_config,
	.alternates = 1,
	.in = fake_in,
	.out = fake_out,
};

/* The root port powered, reset, and enabled; its change bits clear. */
static void port_up(Setup *s)
{
	wr(s, HPRT0, 1u << 12);
	wr(s, HPRT0, 1u << 12 | 1u << 8);
	event_queue_run_until(&s->m->events, s->m->events.now + 50000000);
	wr(s, HPRT0, 1u << 12);
	wr(s, HPRT0, 1u << 12 | 0xa);
	CHECK_EQ(rd(s, HPRT0), 1u << 12 | 5);
}

/* A transfer on channel n from dma; returns HCINT once it is done. */
static uint32_t xfer(Setup *s, unsigned n, uint32_t hcchar, uint32_t hctsiz,
                     uint32_t dma)
{
	uint64_t at = 0x20 * n;

	wr(s, HCTSIZ0 + at, hctsiz);
	wr(s, HCDMA0 + at, dma);
	wr(s, HCINT0 + at, 0x3fff);
	wr(s, HCCHAR0 + at, CHENA | hcchar);
	for (int i = 0; i < 100000 && (rd(s, HCCHAR0 + at) & CHENA); i++)
		event_queue_run_until(&s->m->events, s->m->events.now + 1);
	return rd(s, HCINT0 + at);
}

/* Bulk transfers of several packets, both ways, at address 5. */
static void test_bulk(void)
{
	const uint32_t done = XFERCOMPL | ACKED | CHHLTD;
	Setup s;
	UsbDevice dev;
	uint8_t out[1100], in[1024];

	setup(&s);
	CHECK_EQ(bus_map_ram(s.m->bus, "ram", 0, 0x200000), BUS_OK);
	usb_device_init(&dev, &fake_class, NULL, USB_SPEED_HIGH);
	dwc2_attach(s.d, &dev);
	port_up(&s);
	dev.address = 5; /* as SET_ADDRESS and SET_CONFIGURATION leave it */
	dev.config = 1;
	for (size_t i = 0; i < sizeof(out); i++)
		out[i] = (uint8_t)(i * 7 + 1);
	for (size_t i = 0; i < sizeof(fake_in_data); i++)
		fake_in_data[i] = (uint8_t)(i * 3 + 2);
	CHECK_EQ(bus_write_block(s.m->bus, RAM + 0xff, out, sizeof(out)), BUS_OK);

	/* OUT: 512, 512 and 76 bytes; DOPNG and the alias bits stay. */
	fake_out_len = 0;
	CHECK_EQ(xfer(&s, 1, hcchar(5, 2, false, 2, 512),
	              1u << 31 | 3u << 19 | 1100, ALIAS | (RAM + 0xff)),
	         done);
	CHECK(fake_out_len == 1100 && memcmp(fake_out_data, out, 1100) == 0);
	CHECK_EQ(rd(&s, HCTSIZ0 + 0x20), 1u << 31 | PID_DATA1);
	CHECK_EQ(rd(&s, HCDMA0 + 0x20), ALIAS | (RAM + 0xff + 1100));
	/* IN: 512, then 188 short of 512; 324 bytes not used. */
	fake_in_pos = 0;
	CHECK_EQ(xfer(&s, 0, hcchar(5, 1, true, 2, 512), 2u << 19 | 1024, RAM),
	         done);
	CHECK_EQ(bus_read_block(s.m->bus, RAM, in, 700), BUS_OK);
	CHECK(memcmp(in, fake_in_data, 700) == 0);
	CHECK_EQ(rd(&s, HCTSIZ0), 324);
	/* Packets of 1024 bytes, the most there are. */
	fake_in_pos = 0;
	CHECK_EQ(xfer(&s, 0, hcchar(5, 1, true, 2, 1024), 1u << 19 | 1024, RAM),
	         done);
	CHECK_EQ(rd(&s, HCTSIZ0), PID_DATA1 | 324);
	/* A channel's interrupt reaches the line when the transfer ends. */
	wr(&s, GAHBCFG, 1);
	wr(&s, GINTMSK, 1u << 25);
	wr(&s, HAINTMSK, 1);
	wr(&s, HCINTMSK0, XFERCOMPL);
	fake_in_pos = 0;
	CHECK_EQ(xfer(&s, 0, hcchar(5, 1, true, 2, 512), 2u << 19 | 1024, RAM),
	         done);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(rd(&s, HAINT), 1);
	/* A board reset drops it. */
	dwc2_reset(s.d);
	CHECK(!irq_level(&s.irq));
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);

	/* Packets of 0 or of 1025 bytes stop the machine. */
	for (unsigned mps = 0; mps <= 1025; mps += 1025) {
		setup(&s);
		usb_device_init(&dev, &fake_class, NULL, USB_SPEED_HIGH);
		dwc2_attach(s.d, &dev);
		port_up(&s);
		wr(&s, HCTSIZ0, 1u << 19 | 8);
		wr(&s, HCCHAR0, CHENA | hcchar(0, 1, true, 2, mps));
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		teardown(&s);
	}
}

/*
 * A running transfer: a soft reset or a board reset halts it, and its
 * end does not come after.
 */
static void test_halts(void)
{
	Setup s;
	UsbDevice dev;

	for (int board = 0; board < 2; board++) {
		setup(&s);
		CHECK_EQ(bus_map_ram(s.m->bus, "ram", 0, 0x200000), BUS_OK);
		usb_device_init(&dev, &fake_class, NULL, USB_SPEED_HIGH);
		dwc2_attach(s.d, &dev);
		port_up(&s);
		dev.config = 1;
		fake_in_pos = 0;
		wr(&s, HCTSIZ0 + 0x20, 2u << 19 | 1024);
		wr(&s, HCDMA0 + 0x20, RAM);
		wr(&s, HCCHAR0 + 0x20, CHENA | hcchar(0, 1, true, 2, 512));
		CHECK(rd(&s, HCCHAR0 + 0x20) & CHENA);
		if (board)
			dwc2_reset(s.d);
		else
			wr(&s, GRSTCTL, 1);
		CHECK_EQ(rd(&s, HCCHAR0 + 0x20) & (CHENA | CHDIS), 0);
		wr(&s, HCCHAR0 + 0x20, 0x200); /* not running: no stop */
		event_queue_run_until(&s.m->events, s.m->events.now + 1000000);
		CHECK_EQ(rd(&s, HCINT0 + 0x20) & XFERCOMPL, 0);
		CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
		teardown(&s);
	}
}

/* The root port: power, connect, reset, and the microframes. */
static void test_root_port(void)
{
	Setup s;
	UsbDevice dev;

	setup(&s);
	usb_device_init(&dev, &fake_class, NULL, USB_SPEED_HIGH);
	dwc2_attach(s.d, &dev);
	wr(&s, HPRT0, 0); /* without power, no connection to see */
	CHECK_EQ(rd(&s, HPRT0), 0);
	port_up(&s);
	/* HFNUM: the frame number and the time left, from the enable. */
	event_queue_run_until(&s.m->events, s.m->events.now + 100 * 125000);
	CHECK_EQ(rd(&s, HFNUM), 7500u << 16 | 100);
	/* A reset disables the port; its end resets the device. */
	dev.address = 5;
	wr(&s, HPRT0, 1u << 12 | 1u << 8);
	CHECK_EQ(rd(&s, HPRT0) & 4, 0);
	wr(&s, HPRT0, 1u << 12);
	CHECK_EQ(dev.address, 0);
	CHECK_EQ(rd(&s, HPRT0), 1u << 12 | 0xd);
	/* ENCHNG alone raises PRTINT; writing it 1 clears it. */
	CHECK_EQ(rd(&s, GINTSTS), 1u << 24 | 1);
	wr(&s, HPRT0, 1u << 12 | 8);
	CHECK_EQ(rd(&s, GINTSTS), 1);
	/* A reset that ends without power does not enable the port. */
	wr(&s, HPRT0, 1u << 12 | 1u << 8);
	CHECK_EQ(rd(&s, PCGCCTL), 0); /* HPRT0's writes reach no other */
	wr(&s, HPRT0, 0);
	CHECK_EQ(rd(&s, HPRT0) & 4, 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_identity);
	RUN_TEST(test_resets);
	RUN_TEST(test_port);
	RUN_TEST(test_channels);
	RUN_TEST(test_register_values);
	RUN_TEST(test_stops);
	RUN_TEST(test_transfers);
	RUN_TEST(test_bulk);
	RUN_TEST(test_halts);
	RUN_TEST(test_root_port);
	return test_summary();
}
