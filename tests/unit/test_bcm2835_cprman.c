/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835's clock manager
 * (src/hw/misc/bcm2835_cprman.c), as Linux's clk-bcm2835.c uses it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/bus.h"
#include "core/machine.h"
#include "hw/misc/bcm2835_cprman.h"
#include "test.h"
#include "util/macros.h"

#define BASE 0x3f101000
#define PW 0x5a000000u

enum {
	VPUCTL = 0x008,
	VPUDIV = 0x00c,
	GP0CTL = 0x070,
	GP0DIV = 0x074,
	GP2CTL = 0x080,
	GP2DIV = 0x084,
	TCNTCTL = 0x0c0,
	UARTCTL = 0x0f0,
	UARTDIV = 0x0f4,
	OSCCOUNT = 0x100,
	PLLA = 0x104,
	PLLC = 0x108,
	PLLD = 0x10c,
	PLLH = 0x110,
	LOCK = 0x114,
	DSI0HSCK = 0x120,
	PLLB = 0x170,
	EMMCCTL = 0x1c0,
	EMMCDIV = 0x1c4,
	PLLA_ANA1 = 0x1014,
	PLLC_ANA1 = 0x1034,
	PLLD_ANA0 = 0x1050,
	PLLD_ANA1 = 0x1054,
	PLLA_CTRL = 0x1100,
	PLLC_CTRL = 0x1120,
	PLLD_CTRL = 0x1140,
	PLLH_CTRL = 0x1160,
	XOSC_CTRL = 0x1190,
	PLLB_CTRL = 0x11e0,
	PLLA_FRAC = 0x1200,
	PLLC_FRAC = 0x1220,
	PLLD_FRAC = 0x1240,
	PLLB_FRAC = 0x12e0,
	PLLA_CORE = 0x1400,
	PLLA_PER = 0x1500,
	PLLC_CORE0 = 0x1620,
	PLLC_PER = 0x1520,
	PLLD_CORE = 0x1440,
	PLLD_PER = 0x1540,
	PLLB_ARM = 0x13e0,
	PLLH_PIX = 0x1560,
};

enum {
	ENAB = 1u << 4,
	KILL = 1u << 5,
	GATE = 1u << 6,
	BUSY = 1u << 7,
	MASH1 = 1u << 9,
	ANARST = 1u << 8,
	PWRDN = 1u << 16,
	PRST_DISABLE = 1u << 17,
	DISABLE = 1u << 8,
	FLOCKA = 1u << 8,
	FLOCKB = 1u << 9,
	FLOCKC = 1u << 10,
	FLOCKD = 1u << 11,
	FLOCKH = 1u << 12,
};

typedef struct Setup {
	Machine *m;
	Bcm2835Cprman *c;
} Setup;

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->c = bcm2835_cprman_create(s->m);
	CHECK(s->c != NULL);
	CHECK_EQ(bcm2835_cprman_map(s->c, s->m->bus, "cprman", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_cprman_destroy(s->c);
	machine_destroy(s->m);
}

static uint64_t rd(Setup *s, uint64_t offset)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, BASE + offset, 4, &value), BUS_OK);
	return value;
}

static void wr(Setup *s, uint64_t offset, uint64_t value)
{
	CHECK_EQ(bus_write(s->m->bus, BASE + offset, 4, value), BUS_OK);
	if (s->m->stop != MACHINE_STOP_NONE) {
		fprintf(stderr, "stopped: %s\n", s->m->stop_message);
		test_failures++;
		s->m->stop = MACHINE_STOP_NONE;
	}
}

/* Read, change and write back, as clk-bcm2835.c does. */
static void rmw(Setup *s, uint64_t offset, uint32_t clear, uint32_t set)
{
	wr(s, offset, PW | ((rd(s, offset) & ~clear) | set));
}

/*
 * What the firmware leaves, as a Raspberry Pi 3 Model B shows it:
 * PLLC and PLLD at 2000000024 Hz, the UART at 47999625 Hz.
 */
static void test_boot_state(void)
{
	static const struct {
		uint16_t offset;
		uint32_t value;
	} regs[] = {
		{LOCK, FLOCKB | FLOCKC | FLOCKD},
		{PLLA, ANARST},
		{PLLA_CTRL, PWRDN},
		{PLLA_PER, DISABLE},
		{PLLB, 0},
		{PLLB_CTRL, PRST_DISABLE | 1u << 12 | 62},
		{PLLB_FRAC, 1u << 19},
		{PLLB_ARM, 2},
		{PLLC_CTRL, PRST_DISABLE | 1u << 12 | 52},
		{PLLC_FRAC, 87382},
		{PLLC_ANA1, 1u << 14},
		{PLLC_CORE0, 2},
		{PLLC_PER, DISABLE},
		{PLLD, 0},
		{PLLD_CTRL, PRST_DISABLE | 1u << 12 | 52},
		{PLLD_FRAC, 87382},
		{PLLD_ANA0, 0},
		{PLLD_ANA1, 1u << 14},
		{PLLD_CORE, 4},
		{PLLD_PER, 4},
		{PLLH, ANARST},
		{PLLH_CTRL, PWRDN},
		{PLLH_PIX, DISABLE},
		{XOSC_CTRL, 0x95},
		{VPUCTL, BUSY | ENAB | 5},
		{VPUDIV, 4u << 12},
		{0x028, MASH1 | BUSY | ENAB | 5}, /* H264 */
		{0x02c, 0x2800},                  /* 2.5 */
		{0x030, MASH1 | BUSY | ENAB | 5}, /* ISP */
		{0x034, 0x2800},
		{0x1a8, BUSY | ENAB | 6},           /* SDRAM */
		{0x1ac, 3u << 12},                  /* from PLLD_CORE */
		{UARTCTL, MASH1 | BUSY | ENAB | 6}, /* PLLD_PER */
		{UARTDIV, 10u << 12 | 1707},        /* 10.4167 */
		{EMMCCTL, MASH1 | BUSY | ENAB | 6},
		{EMMCDIV, 0x2800},
		{0x078, BUSY | ENAB | 6},          /* GP1 */
		{0x07c, 20u << 12},                /* 25 MHz */
		{GP2CTL, MASH1 | BUSY | ENAB | 1}, /* the crystal */
		{GP2DIV, 585u << 12 | 3840},       /* 32768 Hz */
		{GP0CTL, 0},
		{0x0e0, BUSY | ENAB | 1}, /* TSENS, from the crystal */
		{0x0e4, 10u << 12},       /* 1.92 MHz */
		{0x0e8, 0},               /* TIMER */
		{DSI0HSCK, 0},
	};
	Setup s;

	setup(&s);
	for (unsigned i = 0; i < ARRAY_SIZE(regs); i++)
		CHECK_EQ(rd(&s, regs[i].offset), regs[i].value);
	CHECK_EQ(bcm2835_cprman_rate(s.c, BCM2835_CPRMAN_UART), 47999625);
	CHECK_EQ(bcm2835_cprman_rate(s.c, BCM2835_CPRMAN_EMMC), 200000002);
	CHECK_EQ(bcm2835_cprman_rate(s.c, BCM2835_CPRMAN_VPU), 250000003);
	teardown(&s);
}

/*
 * The sequences of clk-bcm2835.c: a PLL comes up and locks, a channel
 * and a clock turn on and off. None of them changes a watched rate.
 */
static void test_linux_sequences(void)
{
	Setup s;

	setup(&s);
	/* bcm2835_pll_on, for a running PLL and for PLLA. */
	rmw(&s, PLLD_CTRL, PWRDN, 0);
	rmw(&s, PLLD, ANARST, 0);
	CHECK(rd(&s, LOCK) & FLOCKD);
	rmw(&s, PLLD_CTRL, 0, PRST_DISABLE);
	rmw(&s, PLLA_CTRL, PWRDN, 0);
	CHECK_EQ(rd(&s, LOCK) & FLOCKA, 0); /* still in reset */
	rmw(&s, PLLA, ANARST, 0);
	CHECK_EQ(rd(&s, LOCK), FLOCKA | FLOCKB | FLOCKC | FLOCKD);
	rmw(&s, PLLA_CTRL, 0, PRST_DISABLE);
	rmw(&s, PLLH, ANARST, 0);
	CHECK_EQ(rd(&s, LOCK) & FLOCKH, 0); /* still powered down */
	rmw(&s, PLLH_CTRL, PWRDN, 0);
	CHECK(rd(&s, LOCK) & FLOCKH);

	/* bcm2835_pll_divider_on and _off. */
	rmw(&s, PLLA_CORE, DISABLE, 0);
	rmw(&s, PLLA, 1u << 5, 0);
	CHECK_EQ(rd(&s, PLLA_CORE), 0);
	rmw(&s, PLLD_PER, DISABLE, 0);
	rmw(&s, PLLD, 1u << 7, 0);
	CHECK_EQ(rd(&s, PLLD_PER), 4);
	rmw(&s, PLLD, 0, 1u << 5); /* PLLD_CORE: only SDRAM runs from it */
	rmw(&s, PLLD_CORE, 0, DISABLE);
	CHECK_EQ(rd(&s, PLLD_CORE), DISABLE | 4);

	/* bcm2835_clock_on and _off; and the UART's, which changes no rate. */
	wr(&s, GP0DIV, PW | 20u << 12);
	rmw(&s, GP0CTL, 0, ENAB | GATE | 6);
	CHECK_EQ(rd(&s, GP0CTL), BUSY | GATE | ENAB | 6);
	CHECK_EQ(rd(&s, GP0DIV), 20u << 12);
	rmw(&s, GP0CTL, ENAB, 0);
	CHECK_EQ(rd(&s, GP0CTL), GATE | 6);
	rmw(&s, UARTCTL, 0, ENAB | GATE);
	CHECK_EQ(rd(&s, UARTCTL), MASH1 | BUSY | GATE | ENAB | 6);
	rmw(&s, EMMCCTL, 0, ENAB | GATE);
	rmw(&s, VPUCTL, 0, ENAB | GATE);

	/* The probe's write of DSI0HSCK; and bcm2835_pll_off for PLLA. */
	wr(&s, DSI0HSCK, PW | 1);
	CHECK_EQ(rd(&s, DSI0HSCK), 1);
	wr(&s, PLLA, PW | ANARST);
	rmw(&s, PLLA_CTRL, 0, PWRDN);
	CHECK_EQ(rd(&s, LOCK), FLOCKB | FLOCKC | FLOCKD | FLOCKH);

	/* A reset brings the firmware's state back. */
	bcm2835_cprman_reset(s.c);
	CHECK_EQ(rd(&s, LOCK), FLOCKB | FLOCKC | FLOCKD);
	CHECK_EQ(rd(&s, GP0CTL), 0);
	CHECK_EQ(rd(&s, DSI0HSCK), 0);
	CHECK_EQ(rd(&s, PLLD_CORE), 4);
	teardown(&s);
}

/* The bits that registers keep; the password; BUSY and KILL. */
static void test_fields(void)
{
	Setup s;

	setup(&s);
	wr(&s, GP0CTL, 0x11); /* no password: nothing */
	wr(&s, GP0CTL, 0x5b000011);
	CHECK_EQ(rd(&s, GP0CTL), 0);
	wr(&s, GP0CTL, PW | 0xffffff);
	CHECK_EQ(rd(&s, GP0CTL), 0x77f); /* KILL: not busy */
	wr(&s, GP0CTL, PW | ENAB);
	CHECK_EQ(rd(&s, GP0CTL), BUSY | ENAB);
	wr(&s, GP0CTL, PW | KILL | ENAB);
	CHECK_EQ(rd(&s, GP0CTL), KILL | ENAB);
	wr(&s, GP0DIV, PW | 0xffffff);
	CHECK_EQ(rd(&s, GP0DIV), 0xffffff);
	wr(&s, PLLA_FRAC, PW | 0xffffff);
	CHECK_EQ(rd(&s, PLLA_FRAC), 0xffffff);
	wr(&s, PLLA_ANA1, PW | 0x123456);
	CHECK_EQ(rd(&s, PLLA_ANA1), 0x123456);
	wr(&s, PLLH_PIX, PW | 0xabcdef);
	CHECK_EQ(rd(&s, PLLH_PIX), 0xabcdef);
	wr(&s, XOSC_CTRL, 0x12);
	CHECK_EQ(rd(&s, XOSC_CTRL), 0x95);
	wr(&s, DSI0HSCK, PW | 0xffffff);
	CHECK_EQ(rd(&s, DSI0HSCK), 0xffffff);
	teardown(&s);
}

/*
 * Writes that leave the watched rates as they are go on: bits that a
 * generator does not have, DIVF without MASH, other PLLs and channels.
 */
static void test_same_rates(void)
{
	Setup s;

	setup(&s);
	wr(&s, EMMCDIV, PW | 0x280f);           /* 8 bits of DIVF: 0x80 */
	wr(&s, EMMCDIV, PW | 0xf02800);         /* 4 bits of DIVI */
	wr(&s, EMMCDIV, PW | 0x10000 | 0x2800); /* bit 16: not DIVI */
	wr(&s, VPUDIV, PW | 4u << 12 | 0xfff);  /* no MASH: DIVF is not used */
	wr(&s, VPUDIV, PW | 0xf000000);         /* above the password: nothing */
	wr(&s, VPUDIV, PW | 4u << 12);
	wr(&s, VPUCTL, PW | MASH1 | ENAB | 5); /* DIVF is 0 */
	wr(&s, VPUDIV, PW | 4u << 12 | 0xf);   /* 8 bits of DIVF: bits 11:4 */
	wr(&s, VPUCTL, PW | ENAB | 5);
	wr(&s, UARTDIV, PW | 0xc00000 | 10u << 12 | 1707); /* 10 bits of DIVI */
	wr(&s, UARTCTL, PW | MASH1 | ENAB | 6 | 0x800);    /* bit 11 */
	wr(&s, PLLD_CTRL, PW | PRST_DISABLE | 1u << 12 | 52 | 0x800);
	wr(&s, PLLD_FRAC, PW | 0xf00000 | 87382);
	wr(&s, PLLD_ANA1, PW | 0xffffff); /* the other bits too */
	wr(&s, PLLD_ANA1, PW | 1u << 14);
	wr(&s, PLLD_PER, PW | 0xfe04); /* bits 15:9 */
	wr(&s, PLLD, PW | 0x7f);       /* every HOLD but PLLD_PER's */
	wr(&s, PLLC, PW | 0xfd);       /* every HOLD but PLLC_CORE0's */
	wr(&s, PLLC_PER, PW | 3);
	wr(&s, PLLB_ARM, PW | 1);
	wr(&s, PLLB_CTRL, PW | PWRDN);
	wr(&s, PLLA_PER, PW | 1);
	wr(&s, VPUCTL, PW | MASH1 | ENAB | 5);
	bus_write(s.m->bus, BASE + VPUDIV, 4,
	          PW | 4u << 12 | 0x10); /* DIVF bit 4 */
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message, "rate of the VPU") != NULL);
	CHECK_EQ(bcm2835_cprman_rate(s.c, BCM2835_CPRMAN_UART), 47999625);
	CHECK_EQ(bcm2835_cprman_rate(s.c, BCM2835_CPRMAN_EMMC), 200000002);
	CHECK_EQ(bcm2835_cprman_rate(s.c, BCM2835_CPRMAN_VPU), 250000003);
	teardown(&s);
}

/* The registers of clk-bcm2835.c, at their offsets. */
static const uint16_t controls[] = {
	0x008, 0x028, 0x030, 0x040, 0x048, 0x058, 0x060, 0x068, 0x070,
	0x078, 0x080, 0x088, 0x090, 0x098, 0x0a0, 0x0a8, 0x0b0, 0x0c8,
	0x0e0, 0x0e8, 0x0f0, 0x158, 0x160, 0x168, 0x1a8, 0x1b8, 0x1c0,
};
static const uint16_t plain[] = {
	0x104,  0x108,  0x10c,  0x110,  0x170,  0x120,  0x1190, 0x1100, 0x1120,
	0x1140, 0x1160, 0x11e0, 0x1200, 0x1220, 0x1240, 0x1260, 0x12e0, 0x1300,
	0x1400, 0x1500, 0x1600, 0x13e0, 0x1320, 0x1420, 0x1520, 0x1620, 0x1340,
	0x1440, 0x1540, 0x1640, 0x1360, 0x1460, 0x1560,
};
static const uint16_t ana[] = {0x1010, 0x1030, 0x1050, 0x1070, 0x10f0};

/* What a known offset is: 'c' control, 'p' plain, 'n' counter, 'l' lock. */
static char kind_of(uint64_t offset)
{
	if (offset == 0x020)
		return 'c'; /* PERI_IMAGE, without a divisor */
	if (offset == LOCK)
		return 'l';
	if (offset == TCNTCTL || offset == 0x0c4 || offset == OSCCOUNT)
		return 'n';
	for (unsigned i = 0; i < ARRAY_SIZE(controls); i++) {
		if (offset == controls[i])
			return 'c';
		if (offset == controls[i] + 4u)
			return 'p';
	}
	for (unsigned i = 0; i < ARRAY_SIZE(plain); i++)
		if (offset == plain[i])
			return 'p';
	for (unsigned i = 0; i < ARRAY_SIZE(ana); i++)
		if (offset >= ana[i] && offset < ana[i] + 16u)
			return 'p';
	return 0;
}

/* Every offset: a register, the frequency counter, or a stop. */
static void test_register_map(void)
{
	Setup s;
	uint64_t value = 0;
	unsigned known = 0;

	setup(&s);
	for (uint64_t offset = 0; offset < BCM2835_CPRMAN_MMIO_SIZE; offset += 4) {
		char kind = kind_of(offset);

		value = 0;
		CHECK_EQ(bus_read(s.m->bus, BASE + offset, 4, &value), BUS_OK);
		if (kind == 'c' || kind == 'p' || kind == 'l') {
			known++;
			if (s.m->stop != MACHINE_STOP_NONE)
				fprintf(stderr, "0x%04x stops\n", (unsigned)offset);
			CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
		} else {
			CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
			CHECK(strstr(s.m->stop_message,
			             kind == 'n' ? "frequency counter" : "not emulated"));
			CHECK_EQ(value, 0);
		}
		s.m->stop = MACHINE_STOP_NONE;
	}
	CHECK_EQ(known, 2 * 27 + 1 + 1 + 33 + 20);

	/* What the registers that no watched clock uses keep. */
	for (unsigned i = 0; i < ARRAY_SIZE(controls); i++) {
		if (controls[i] == VPUCTL || controls[i] == UARTCTL ||
		    controls[i] == EMMCCTL)
			continue;
		wr(&s, controls[i], PW | 0xffffff);
		CHECK_EQ(rd(&s, controls[i]), 0x77f);
		wr(&s, controls[i] + 4u, PW | 0xffffff);
		CHECK_EQ(rd(&s, controls[i] + 4u), 0xffffff);
	}
	wr(&s, 0x020, PW | 0xffffff);
	CHECK_EQ(rd(&s, 0x020), 0x77f);
	for (unsigned i = 0; i < ARRAY_SIZE(ana); i++) {
		if (ana[i] == 0x1030 || ana[i] == 0x1050)
			continue; /* PLLC and PLLD: the VPU, UART and EMMC */
		for (unsigned j = 0; j < 16; j += 4) {
			wr(&s, ana[i] + j, PW | 0xffffff);
			CHECK_EQ(rd(&s, ana[i] + j), 0xffffff);
		}
	}

	/* An access that stops does nothing: its value stays out. */
	bus_write(s.m->bus, BASE + 0x1d0, 4, PW | 0x123);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	s.m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(bus_read(s.m->bus, BASE + 0x1d0, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	s.m->stop = MACHINE_STOP_NONE;
	bus_write(s.m->bus, BASE + OSCCOUNT, 4, PW | 0x123);
	s.m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(bus_read(s.m->bus, BASE + OSCCOUNT, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	s.m->stop = MACHINE_STOP_NONE;
	bus_write(s.m->bus, BASE + DSI0HSCK, 8, PW | 0x123);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	s.m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(rd(&s, DSI0HSCK), 0);
	teardown(&s);
}

/*
 * Other sources that give a watched clock the same rate: the change goes
 * on, and holding or disabling the new source's channel then stops the
 * machine.
 */
static void test_other_sources(void)
{
	static const struct {
		uint16_t pll, ctrl, frac, ana1, channel;
		uint32_t ctrl_value, frac_value, ana1_value, channel_div, hold, load;
		uint16_t ctl;
		uint32_t ctl_value;
		const char *clock;
	} paths[] = {
		/* PLLH with its pre-divider and PDIV 2: 2000000024 Hz. */
		{PLLH, PLLH_CTRL, 0x1260, 0x1074, 0x1360, 2u << 12 | 104, 174764,
	     1u << 11, 4, 0, 7, UARTCTL, MASH1 | ENAB | 7, "UART"},
		{PLLH, PLLH_CTRL, 0x1260, 0x1074, 0x1360, 2u << 12 | 104, 174764,
	     1u << 11, 2, 0, 7, VPUCTL, ENAB | 7, "VPU"},
		{PLLA, PLLA_CTRL, PLLA_FRAC, PLLA_ANA1, PLLA_PER, 1u << 12 | 52, 87382,
	     1u << 14, 4, 1u << 7, 0x55, UARTCTL, MASH1 | ENAB | 4, "UART"},
		{PLLA, PLLA_CTRL, PLLA_FRAC, PLLA_ANA1, PLLA_CORE, 1u << 12 | 52, 87382,
	     1u << 14, 2, 1u << 5, 0x55, VPUCTL, ENAB | 4, "VPU"},
		{PLLA, PLLA_CTRL, PLLA_FRAC, PLLA_ANA1, PLLA_PER, 1u << 12 | 52, 87382,
	     1u << 14, 4, 1u << 7, 0x55, EMMCCTL, MASH1 | ENAB | 4, "EMMC"},
	};
	static const struct {
		uint16_t channel;
		uint32_t div, hold;
		uint16_t pll, ctl;
		uint32_t ctl_value;
		const char *clock;
	} same_pll[] = {
		{PLLC_PER, 4, 1u << 7, PLLC, UARTCTL, MASH1 | ENAB | 5, "UART"},
		{PLLD_CORE, 2, 1u << 5, PLLD, VPUCTL, ENAB | 6, "VPU"},
		{0x1420, 2, 1u << 3, PLLC, VPUCTL, ENAB | 8, "VPU"}, /* PLLC_CORE1 */
		{0x1320, 2, 1u << 5, PLLC, VPUCTL, ENAB | 9, "VPU"}, /* PLLC_CORE2 */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(paths); i++) {
		Setup s;

		setup(&s);
		wr(&s, paths[i].frac, PW | paths[i].frac_value);
		wr(&s, paths[i].ana1, PW | paths[i].ana1_value);
		wr(&s, paths[i].channel, PW | paths[i].channel_div);
		wr(&s, paths[i].pll, PW | paths[i].load);
		wr(&s, paths[i].ctrl, PW | paths[i].ctrl_value);
		wr(&s, paths[i].ctl, PW | paths[i].ctl_value);
		CHECK_EQ(rd(&s, paths[i].ctl), BUSY | paths[i].ctl_value);
		if (paths[i].hold != 0)
			bus_write(s.m->bus, BASE + paths[i].pll, 4, PW | paths[i].hold);
		else
			bus_write(s.m->bus, BASE + paths[i].channel, 4,
			          PW | DISABLE | paths[i].channel_div);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, paths[i].clock) != NULL);
		teardown(&s);
	}
	for (unsigned i = 0; i < ARRAY_SIZE(same_pll); i++) {
		Setup s;

		setup(&s);
		wr(&s, same_pll[i].channel, PW | same_pll[i].div);
		wr(&s, same_pll[i].ctl, PW | same_pll[i].ctl_value);
		bus_write(s.m->bus, BASE + same_pll[i].pll, 4, PW | same_pll[i].hold);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, same_pll[i].clock) != NULL);
		teardown(&s);
	}
}

/* What the model lacks, or nothing describes, stops it, and does nothing. */
static void test_stops(void)
{
	static const struct {
		bool write;
		uint64_t offset;
		unsigned size;
		uint32_t value;
		const char *message;
	} stops[] = {
		{false, UARTCTL, 1, 0,
	     "1-byte read from offset 0x00f0 is not a 32-bit"},
		{true, UARTDIV, 2, PW, "2-byte write to offset 0x00f4 is not a 32-bit"},
		{false, 0x0f2, 4, 0, "offset 0x00f2 is not a 32-bit register access"},
		{false, 0x000, 4, 0,
	     "offset 0x0000 is not emulated: no register of clk-bcm2835 is there"},
		{true, 0x1d0, 4, PW, "offset 0x01d0 is not emulated"},  /* EMMC2 */
		{false, 0x038, 4, 0, "offset 0x0038 is not emulated"},  /* V3D */
		{false, 0x14e0, 4, 0, "offset 0x14e0 is not emulated"}, /* PLLB_SP0 */
		{false, 0x1ffc, 4, 0, "offset 0x1ffc is not emulated"},
		{false, OSCCOUNT, 4, 0,
	     "offset 0x0100 is not implemented yet (the frequency counter)"},
		{true, TCNTCTL, 4, PW | KILL, "offset 0x00c0 is not implemented yet"},
		{false, 0x0c4, 4, 0, "offset 0x00c4 is not implemented yet"},
		{true, LOCK, 4, PW, "4-byte write to offset 0x0114 is not emulated"},
		{true, UARTDIV, 4, PW | 10u << 12 | 1706,
	     "4-byte write to offset 0x00f4 is not implemented yet: it changes the "
	     "rate of the UART"},
		{true, UARTCTL, 4, PW | MASH1 | 6,
	     "offset 0x00f0 is not implemented yet: it changes the rate of the "
	     "UART"},
		{true, UARTCTL, 4, PW | MASH1 | KILL | ENAB | 6, "rate of the UART"},
		{true, UARTCTL, 4, PW | ENAB | 6, "rate of the UART"}, /* no MASH */
		{true, UARTCTL, 4, PW | MASH1 | ENAB | 1, "rate of the UART"},
		{true, UARTCTL, 4, PW | MASH1 | ENAB | 5, "rate of the UART"},
		{true, UARTCTL, 4, PW | MASH1 | ENAB | 0xe, "rate of the UART"},
		{true, UARTDIV, 4, PW | 1707, "rate of the UART"}, /* DIVI 0 */
		{true, UARTDIV, 4, PW | 11u << 12 | 1707, "rate of the UART"},
		{true, UARTDIV, 4, PW | 0x200000 | 10u << 12 | 1707,
	     "UART"},                                            /* bit 21 */
		{true, EMMCDIV, 4, PW | 0xa800, "rate of the EMMC"}, /* DIVI bit 15 */
		{true, VPUDIV, 4, PW | 0x804000, "rate of the VPU"}, /* DIVI bit 23 */
		{true, EMMCDIV, 4, PW | 0x2810, "rate of the EMMC"},
		{true, EMMCDIV, 4, PW | 0x3800, "rate of the EMMC"},
		{true, EMMCCTL, 4, PW | ENAB | 6, "rate of the EMMC"},
		{true, VPUDIV, 4, PW | 5u << 12, "rate of the VPU"},
		{true, VPUDIV, 4, PW | 0x800000, "rate of the VPU"}, /* DIVI 2048 */
		{true, VPUCTL, 4, PW | ENAB | 6, "rate of the VPU"}, /* PLLD_CORE */
		{true, VPUCTL, 4, PW | ENAB | 8, "rate of the VPU"}, /* PLLC_CORE1 */
		{true, PLLD_CTRL, 4, PW | PRST_DISABLE | 1u << 12 | 53, "UART"},
		{true, PLLD_CTRL, 4, PW | PRST_DISABLE | 2u << 12 | 52, "UART"},
		{true, PLLD_CTRL, 4, PW | PRST_DISABLE | 52, "UART"}, /* PDIV 0 */
		{true, PLLD_CTRL, 4, PW | PWRDN | PRST_DISABLE | 1u << 12 | 52, "UART"},
		{true, PLLD_FRAC, 4, PW | 87383, "rate of the UART"},
		{true, PLLD_ANA1, 4, PW, "rate of the UART"},
		{true, PLLD, 4, PW | ANARST, "rate of the UART"},
		{true, PLLD, 4, PW | 1u << 7, "rate of the UART"}, /* HOLDPER */
		{true, PLLD_PER, 4, PW | DISABLE | 4, "rate of the UART"},
		{true, PLLD_PER, 4, PW | 5, "rate of the UART"},
		{true, PLLD_PER, 4, PW, "rate of the UART"}, /* 256 */
		{true, PLLC_ANA1, 4, PW, "rate of the VPU"},
		{true, PLLC, 4, PW | 1u << 1, "rate of the VPU"}, /* HOLDCORE0 */
		{true, PLLC_CORE0, 4, PW | 1, "rate of the VPU"},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(stops); i++) {
		Setup s;
		uint64_t value = 0;

		setup(&s);
		if (stops[i].write)
			bus_write(s.m->bus, BASE + stops[i].offset, stops[i].size,
			          stops[i].value);
		else
			bus_read(s.m->bus, BASE + stops[i].offset, stops[i].size, &value);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		if (strstr(s.m->stop_message, stops[i].message) == NULL) {
			fprintf(stderr, "stop %u: %s\n", i, s.m->stop_message);
			test_failures++;
		}
		CHECK(strstr(s.m->stop_message, "bcm2835-cprman: ") != NULL);
		/* The access did nothing else. */
		CHECK_EQ(value, 0);
		CHECK_EQ(bcm2835_cprman_rate(s.c, BCM2835_CPRMAN_UART), 47999625);
		CHECK_EQ(bcm2835_cprman_rate(s.c, BCM2835_CPRMAN_EMMC), 200000002);
		CHECK_EQ(bcm2835_cprman_rate(s.c, BCM2835_CPRMAN_VPU), 250000003);
		CHECK_EQ(rd(&s, UARTCTL), MASH1 | BUSY | ENAB | 6);
		teardown(&s);
	}
}

int main(void)
{
	RUN_TEST(test_boot_state);
	RUN_TEST(test_linux_sequences);
	RUN_TEST(test_fields);
	RUN_TEST(test_same_rates);
	RUN_TEST(test_register_map);
	RUN_TEST(test_other_sources);
	RUN_TEST(test_stops);
	return test_summary();
}
