/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the DWC2 USB controller as a host (src/hw/usb/dwc2.c),
 * with nothing on its port, as Linux's and U-Boot's drivers use it.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/usb/dwc2.h"
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
	GUID = 0x03c,
	GSNPSID = 0x040,
	GHWCFG1 = 0x044,
	GHWCFG2 = 0x048,
	GHWCFG3 = 0x04c,
	GHWCFG4 = 0x050,
	HPTXFSIZ = 0x100,
	HCFG = 0x400,
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
	wr(&s, GSNPSID, 0); /* read-only: stops */
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	s.m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(rd(&s, GINTSTS), 1); /* CURMODE: host */
	CHECK_EQ(rd(&s, GOTGCTL), 0); /* ID: an A-device */
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
		{true, HCCHAR0, 4, CHENA, "a transfer, with no device"},
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

int main(void)
{
	RUN_TEST(test_identity);
	RUN_TEST(test_resets);
	RUN_TEST(test_port);
	RUN_TEST(test_channels);
	RUN_TEST(test_stops);
	return test_summary();
}
