/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835's MPHI (src/hw/misc/bcm2835_mphi.c), as the
 * Raspberry Pi's dwc_otg driver uses it to raise its "soft" interrupt.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/misc/bcm2835_mphi.h"
#include "test.h"

#define BASE 0x3f006000

enum { OUTDDA = 0x28, OUTDDB = 0x2c, CTRL = 0x4c, INTSTAT = 0x50 };

typedef struct Setup {
	Machine *m;
	Bcm2835Mphi *mphi;
	IrqLine irq;
	bool level;
} Setup;

static void on_irq(void *ctx, unsigned n, bool level)
{
	Setup *s = ctx;

	(void)n;
	s->level = level;
}

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->level = false;
	irq_init(&s->irq, on_irq, s, 0);
	s->mphi = bcm2835_mphi_create(s->m, &s->irq);
	CHECK(s->mphi != NULL);
	CHECK_EQ(bcm2835_mphi_map(s->mphi, s->m->bus, "mphi", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_mphi_destroy(s->mphi);
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
}

static void check_stop(Setup *s, const char *message)
{
	CHECK_EQ(s->m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s->m->stop_message, "bcm2835-mphi: ") != NULL);
	CHECK(strstr(s->m->stop_message, message) != NULL);
	s->m->stop = MACHINE_STOP_NONE;
}

/*
 * dwc_otg: enable the block (hcd_init_fiq); the fast handler sends a
 * dummy buffer to raise the interrupt (dwc_otg_fiq_fsm); the main
 * handler clears it (dwc_otg_hcd_handle_intr).
 */
static void test_kick(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, CTRL), 0);
	CHECK_EQ(rd(&s, INTSTAT), 0);
	wr(&s, CTRL, 1u << 31);
	CHECK_EQ(rd(&s, CTRL), 1u << 31);
	wr(&s, OUTDDA, 0xc7490000u);
	CHECK_EQ(rd(&s, OUTDDA), 0xc7490000u);
	CHECK(!s.level);
	wr(&s, OUTDDB, 1u << 29);
	CHECK(s.level);
	CHECK_EQ(rd(&s, INTSTAT), (1u << 29) | (1u << 16));
	wr(&s, INTSTAT, 1u << 16);
	CHECK(!s.level);
	CHECK_EQ(rd(&s, INTSTAT), 1u << 29);

	/* The other path: clear the send first, then send again. */
	wr(&s, INTSTAT, 1u << 29);
	CHECK_EQ(rd(&s, INTSTAT), 0);
	CHECK(!s.level);
	wr(&s, OUTDDB, 1u << 29);
	CHECK(s.level);
	wr(&s, INTSTAT, (1u << 29) | (1u << 16));
	CHECK_EQ(rd(&s, INTSTAT), 0);
	CHECK(!s.level);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/*
 * After 50 kicks, dwc_otg resets the block: bit 16 of CTRL, until bit 17
 * says it is done, then enables it again. A reset of the board disables
 * it and drops the interrupt.
 */
static void test_reset(void)
{
	Setup s;

	setup(&s);
	wr(&s, CTRL, 1u << 31);
	wr(&s, CTRL, (1u << 31) | (1u << 16));
	CHECK_EQ(rd(&s, CTRL), (1u << 31) | (1u << 17) | (1u << 16));
	wr(&s, CTRL, 1u << 31);
	CHECK_EQ(rd(&s, CTRL), 1u << 31);
	wr(&s, CTRL, 1u << 16);
	CHECK_EQ(rd(&s, CTRL), (1u << 17) | (1u << 16));
	wr(&s, CTRL, 1u << 31);

	wr(&s, OUTDDA, 0x1234);
	wr(&s, OUTDDB, 1u << 29);
	CHECK(s.level);
	bcm2835_mphi_reset(s.mphi);
	CHECK(!s.level);
	CHECK_EQ(rd(&s, CTRL), 0);
	CHECK_EQ(rd(&s, INTSTAT), 0);
	CHECK_EQ(rd(&s, OUTDDA), 0);
	teardown(&s);
}

/* What dwc_otg does not do stops the machine. */
static void test_stops(void)
{
	Setup s;
	uint64_t value = 0x5a;

	setup(&s);
	wr(&s, OUTDDB, 1u << 29);
	check_stop(&s, "4-byte write to offset 0x02c is not emulated: a send "
	               "while the block is disabled");
	CHECK(!s.level);
	CHECK_EQ(rd(&s, INTSTAT), 0);
	wr(&s, CTRL, 1u << 31);
	wr(&s, OUTDDB, (1u << 29) | 1);
	check_stop(&s, "write to offset 0x02c is not emulated: only a send, "
	               "bit 29");
	wr(&s, OUTDDB, 0);
	check_stop(&s, "only a send, bit 29");
	CHECK(!s.level);
	wr(&s, CTRL, (1u << 31) | (1u << 17));
	check_stop(&s, "write to offset 0x04c is not emulated: bits other than "
	               "enable and reset");
	wr(&s, CTRL, 1);
	check_stop(&s, "bits other than enable and reset");
	CHECK_EQ(rd(&s, CTRL), 1u << 31);
	wr(&s, OUTDDB, 1u << 29);
	wr(&s, INTSTAT, (1u << 16) | (1u << 30));
	check_stop(&s, "write to offset 0x050 is not emulated: bits other than "
	               "16 and 29");
	wr(&s, INTSTAT, 1);
	check_stop(&s, "bits other than 16 and 29");
	CHECK(s.level);
	CHECK_EQ(rd(&s, OUTDDB), 0);
	check_stop(&s, "read of offset 0x02c is not emulated: dwc_otg does not "
	               "read it");
	CHECK_EQ(rd(&s, 0x1f0), 0);
	check_stop(&s, "read of offset 0x1f0 is not emulated");
	wr(&s, 0x1f0, 1);
	check_stop(&s, "write to offset 0x1f0 is not emulated: dwc_otg does not "
	               "write it");
	CHECK_EQ(bus_read(s.m->bus, BASE + CTRL, 2, &value), BUS_OK);
	check_stop(&s, "2-byte read of offset 0x04c is not a 32-bit register");
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_read(s.m->bus, BASE + 0x29, 4, &value), BUS_OK);
	check_stop(&s, "read of offset 0x029 is not a 32-bit register");
	CHECK_EQ(bus_write(s.m->bus, BASE + CTRL, 1, 0), BUS_OK);
	check_stop(&s, "1-byte write to offset 0x04c is not a 32-bit register");
	CHECK_EQ(bus_write(s.m->bus, BASE + 0x4e, 4, 0), BUS_OK);
	check_stop(&s, "write to offset 0x04e is not a 32-bit register");
	CHECK_EQ(rd(&s, CTRL), 1u << 31);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_kick);
	RUN_TEST(test_reset);
	RUN_TEST(test_stops);
	return test_summary();
}
