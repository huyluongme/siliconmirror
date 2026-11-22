/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */
/*
 * The HVS with no display attached: what Linux's vc4 driver does to it
 * at bind (vc4_hvs_hw_init, the filter kernels, vc4_hvs_cob_init) is
 * kept; the rest stops the machine.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/bus.h"
#include "core/machine.h"
#include "hw/display/bcm2835_hvs.h"
#include "test.h"

#define BASE 0x3f400000

enum {
	DISPCTRL = 0x00,
	DISPSTAT = 0x04,
	DISPID = 0x08,
	DISPECTRL = 0x0c,
	DISPPROF = 0x10,
	DISPDITHER = 0x14,
	DISPEOLN = 0x18,
	DISPLIST0 = 0x20,
	DISPCTRL0 = 0x40,
	DISPBKGND0 = 0x44,
	DISPSTAT0 = 0x48,
	DISPBASE0 = 0x4c,
	DISPBASE1 = 0x5c,
	DISPBASE2 = 0x6c,
	GAMADDR = 0x78,
	DLIST = 0x2000,
	DLIST_END = 0x6000,
};

typedef struct Setup {
	Machine *m;
	Bcm2835Hvs *hvs;
} Setup;

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->hvs = bcm2835_hvs_create(s->m);
	CHECK(s->hvs != NULL);
	CHECK_EQ(bcm2835_hvs_map(s->hvs, s->m->bus, "hvs", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_hvs_destroy(s->hvs);
	machine_destroy(s->m);
}

static uint32_t rd(Setup *s, uint64_t offset)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, BASE + offset, 4, &value), BUS_OK);
	return (uint32_t)value;
}

static void wr(Setup *s, uint64_t offset, uint32_t value)
{
	CHECK_EQ(bus_write(s->m->bus, BASE + offset, 4, value), BUS_OK);
}

static void check_stop(Setup *s, const char *message)
{
	CHECK_EQ(s->m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s->m->stop_message, "bcm2835-hvs: ") != NULL);
	if (strstr(s->m->stop_message, message) == NULL)
		fprintf(stderr, "got \"%s\"\n", s->m->stop_message);
	CHECK(strstr(s->m->stop_message, message) != NULL);
	s->m->stop = MACHINE_STOP_NONE;
}

/* The bind of Linux's vc4 driver, as Raspberry Pi OS's kernel did it. */
static void test_linux_bind(void)
{
	static const uint32_t kept[] = {DISPCTRL,  DISPECTRL, DISPDITHER, DISPEOLN,
	                                DISPBASE0, DISPBASE1, DISPBASE2};
	Setup s;
	uint32_t v;

	setup(&s);
	for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
		CHECK_EQ(rd(&s, kept[i]), 0);
	CHECK_EQ(rd(&s, DISPSTAT), 0);
	CHECK_EQ(rd(&s, DLIST), 0);
	CHECK_EQ(rd(&s, DLIST_END - 4), 0);

	/* vc4_hvs_hw_init */
	wr(&s, DISPCTRL, rd(&s, DISPCTRL) | 0x80000000u);
	wr(&s, DISPECTRL, rd(&s, DISPECTRL) & ~0x80000000u);
	v = rd(&s, DISPCTRL) & ~(3u << 18);
	wr(&s, DISPCTRL, v | 3u << 18);
	wr(&s, DISPEOLN, (rd(&s, DISPEOLN) & ~(3u << 30)) | 3u << 30);
	wr(&s, DISPDITHER, (rd(&s, DISPDITHER) & ~(3u << 30)) | 3u << 30);
	v = rd(&s, DISPCTRL);
	CHECK_EQ(v, 0x800c0000u);
	wr(&s, DISPCTRL, v | 0x2a000000u | 0x0000e000u);
	CHECK_EQ(rd(&s, DISPCTRL), 0xaa0ce000u);
	CHECK_EQ(rd(&s, DISPECTRL), 0);
	CHECK_EQ(rd(&s, DISPEOLN), 0xc0000000u);
	CHECK_EQ(rd(&s, DISPDITHER), 0xc0000000u);
	/* The two filter kernels, 11 words each, after the first 32. */
	for (uint32_t i = 0; i < 22; i++)
		wr(&s, DLIST + 0x80 + 4 * i, 0x07ebfc00u + i);
	for (uint32_t i = 0; i < 22; i++)
		CHECK_EQ(rd(&s, DLIST + 0x80 + 4 * i), 0x07ebfc00u + i);
	CHECK_EQ(rd(&s, DLIST + 0x7c), 0);
	CHECK_EQ(rd(&s, DLIST + 0xd8), 0);
	/* vc4_hvs_cob_init */
	wr(&s, DISPBASE2, 0x17ff0000u);
	wr(&s, DISPBASE1, 0x2fff1800u);
	wr(&s, DISPBASE0, 0x50ff3000u);
	CHECK_EQ(rd(&s, DISPBASE0), 0x50ff3000u);
	CHECK_EQ(rd(&s, DISPBASE1), 0x2fff1800u);
	CHECK_EQ(rd(&s, DISPBASE2), 0x17ff0000u);
	/* The interrupt handler's acknowledgement: nothing to clear. */
	wr(&s, DISPSTAT, 0xffffffffu);
	CHECK_EQ(rd(&s, DISPSTAT), 0);
	/* The ends of the display list memory. */
	wr(&s, DLIST, 0x12345678u);
	wr(&s, DLIST_END - 4, 0x9abcdef0u);
	CHECK_EQ(rd(&s, DLIST), 0x12345678u);
	CHECK_EQ(rd(&s, DLIST_END - 4), 0x9abcdef0u);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);

	/* A reset clears them all. */
	bcm2835_hvs_reset(s.hvs);
	for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
		CHECK_EQ(rd(&s, kept[i]), 0);
	CHECK_EQ(rd(&s, DLIST), 0);
	CHECK_EQ(rd(&s, DLIST + 0x80), 0);
	CHECK_EQ(rd(&s, DLIST_END - 4), 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* What a running channel or another driver would use stops the machine. */
static void test_stops(void)
{
	static const uint32_t others[] = {
		DISPID, DISPPROF, DISPLIST0, DISPCTRL0, DISPBKGND0, DISPSTAT0,
		0x50,   0x60,     GAMADDR,   0x7c,      0x80,       DLIST - 4,
	};
	Setup s;
	uint64_t v;

	setup(&s);
	for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
		CHECK_EQ(rd(&s, others[i]), 0);
		check_stop(&s, "is not emulated: no display is attached");
		wr(&s, others[i], 1);
		check_stop(&s, "is not emulated: no display is attached");
	}
	CHECK_EQ(rd(&s, 0x80), 0);
	check_stop(&s, "4-byte read of offset 0x0080 is not emulated");
	wr(&s, DISPID, 1);
	check_stop(&s, "4-byte write to offset 0x0008 is not emulated");
	CHECK_EQ(bus_read(s.m->bus, BASE + DISPCTRL, 2, &v), BUS_OK);
	check_stop(&s, "2-byte read of offset 0x0000 is not a 32-bit");
	CHECK_EQ(bus_read(s.m->bus, BASE + DLIST + 2, 4, &v), BUS_OK);
	check_stop(&s, "read of offset 0x2002 is not a 32-bit");
	CHECK_EQ(bus_write(s.m->bus, BASE + DLIST, 1, 5), BUS_OK);
	check_stop(&s, "1-byte write to offset 0x2000 is not a 32-bit");
	CHECK_EQ(bus_write(s.m->bus, BASE + DISPCTRL + 1, 4, 5), BUS_OK);
	check_stop(&s, "write to offset 0x0001 is not a 32-bit");
	/* Nothing was written. */
	CHECK_EQ(rd(&s, DISPCTRL), 0);
	CHECK_EQ(rd(&s, DLIST), 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_linux_bind);
	RUN_TEST(test_stops);
	return test_summary();
}
