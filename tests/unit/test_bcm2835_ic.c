/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the ARM interrupt controller of the BCM2835
 * (src/hw/intc/bcm2835_ic.c). Section numbers refer to chapter 7 of
 * Broadcom's "BCM2835 ARM Peripherals".
 */

#include <stdint.h>
#include <string.h>

#include "hw/intc/bcm2835_ic.h"
#include "test.h"
#include "util/macros.h"

#define BASE 0x3f00b200
#define BASIC_PENDING (BASE + 0x00)
#define PENDING1 (BASE + 0x04)
#define PENDING2 (BASE + 0x08)
#define FIQ_CONTROL (BASE + 0x0c)
#define ENABLE1 (BASE + 0x10)
#define ENABLE2 (BASE + 0x14)
#define ENABLE_BASIC (BASE + 0x18)
#define DISABLE1 (BASE + 0x1c)
#define DISABLE2 (BASE + 0x20)
#define DISABLE_BASIC (BASE + 0x24)

#define FIQ_ENABLE 0x80

static Machine *m;
static Bcm2835Ic *ic;
static IrqLine irq_out, fiq_out;

static void setup(void)
{
	m = machine_create();
	ic = bcm2835_ic_create(m);
	CHECK(m != NULL && ic != NULL);
	CHECK_EQ(bcm2835_ic_map(ic, m->bus, "intc", BASE), BUS_OK);
	irq_init(&irq_out, NULL, NULL, 0);
	irq_init(&fiq_out, NULL, NULL, 0);
	bcm2835_ic_connect(ic, &irq_out, &fiq_out);
}

static void teardown(void)
{
	bcm2835_ic_destroy(ic);
	machine_destroy(m);
}

static uint32_t rd(uint64_t address)
{
	uint64_t value = 0x5555;

	CHECK_EQ(bus_read(m->bus, address, 4, &value), BUS_OK);
	return (uint32_t)value;
}

static void wr(uint64_t address, uint32_t value)
{
	CHECK_EQ(bus_write(m->bus, address, 4, value), BUS_OK);
}

static void gpu(unsigned n, bool level)
{
	irq_set(bcm2835_ic_gpu_input(ic, n), level);
}

static void arm(unsigned n, bool level)
{
	irq_set(bcm2835_ic_arm_input(ic, n), level);
}

/* Section 7.5: every register resets to 0. */
static void test_reset(void)
{
	setup();
	for (uint64_t a = BASIC_PENDING; a <= DISABLE_BASIC; a += 4)
		CHECK_EQ(rd(a), 0);
	CHECK(!irq_level(&irq_out));
	CHECK(!irq_level(&fiq_out));

	/* After setting the enables, a reset clears them; inputs are wires. */
	wr(ENABLE1, 0xffffffff);
	wr(ENABLE2, 0xffffffff);
	wr(ENABLE_BASIC, 0xff);
	wr(FIQ_CONTROL, FIQ_ENABLE | 3);
	gpu(3, true);
	CHECK(irq_level(&irq_out));
	CHECK(irq_level(&fiq_out));
	bcm2835_ic_reset(ic);
	for (uint64_t a = BASIC_PENDING; a <= DISABLE_BASIC; a += 4)
		CHECK_EQ(rd(a), 0);
	CHECK(!irq_level(&irq_out));
	CHECK(!irq_level(&fiq_out));
	wr(ENABLE1, 1u << 3);
	CHECK_EQ(rd(PENDING1), 1u << 3);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	teardown();
}

/* The enable registers set bits, the disable registers clear them. */
static void test_enable_and_disable(void)
{
	setup();
	wr(ENABLE1, 0x30840008);
	wr(ENABLE1, 0xfc060014);
	CHECK_EQ(rd(ENABLE1), 0xfc86001c);
	CHECK_EQ(rd(DISABLE1), 0xfc86001c);
	wr(DISABLE1, 0xfc060014);
	CHECK_EQ(rd(ENABLE1), 0x00800008);
	CHECK_EQ(rd(ENABLE2), 0);

	wr(ENABLE2, 0x12345678);
	CHECK_EQ(rd(ENABLE2), 0x12345678);
	CHECK_EQ(rd(DISABLE2), 0x12345678);
	wr(DISABLE2, 0x00005678);
	CHECK_EQ(rd(ENABLE2), 0x12340000);
	CHECK_EQ(rd(ENABLE1), 0x00800008);
	wr(DISABLE1, 0x0000000f);
	CHECK_EQ(rd(ENABLE1), 0x00800000);
	CHECK_EQ(rd(ENABLE2), 0x12340000);

	wr(ENABLE_BASIC, 0xffffffff);
	CHECK_EQ(rd(ENABLE_BASIC), 0xff);
	wr(DISABLE_BASIC, 0x0f);
	CHECK_EQ(rd(DISABLE_BASIC), 0xf0);

	/* The pending registers are read-only. */
	wr(BASIC_PENDING, 0xffffffff);
	wr(PENDING1, 0xffffffff);
	wr(PENDING2, 0xffffffff);
	CHECK_EQ(rd(BASIC_PENDING), 0);
	CHECK_EQ(rd(PENDING1), 0);
	CHECK_EQ(rd(PENDING2), 0);
	teardown();
}

/*
 * Sections 7.2 and 7.5: an interrupt is pending, and drives the IRQ,
 * while raised and enabled.
 */
static void test_pending(void)
{
	setup();
	gpu(3, true);
	gpu(40, true);
	arm(BCM2835_IC_ARM_MAILBOX, true);
	CHECK_EQ(rd(PENDING1), 0);
	CHECK_EQ(rd(PENDING2), 0);
	CHECK_EQ(rd(BASIC_PENDING), 0);
	CHECK(!irq_level(&irq_out));

	wr(ENABLE1, 1u << 3);
	CHECK_EQ(rd(PENDING1), 1u << 3);
	CHECK_EQ(rd(BASIC_PENDING), 1u << 8);
	CHECK(irq_level(&irq_out));
	wr(ENABLE2, 1u << 8);
	CHECK_EQ(rd(PENDING2), 1u << 8);
	CHECK_EQ(rd(BASIC_PENDING), 1u << 8 | 1u << 9);
	wr(ENABLE_BASIC, 1u << BCM2835_IC_ARM_MAILBOX);
	CHECK_EQ(rd(BASIC_PENDING), 1u << 8 | 1u << 9 | 1u << 1);

	/* Each source alone drives the IRQ. */
	gpu(3, false);
	gpu(40, false);
	CHECK_EQ(rd(BASIC_PENDING), 1u << 1);
	CHECK(irq_level(&irq_out));
	wr(DISABLE_BASIC, 0xff);
	CHECK(!irq_level(&irq_out));
	gpu(40, true);
	CHECK(irq_level(&irq_out));
	wr(DISABLE2, 0xffffffff);
	CHECK(!irq_level(&irq_out));
	CHECK_EQ(rd(BASIC_PENDING), 0);

	/* The eight ARM interrupts, in the low bits. */
	arm(BCM2835_IC_ARM_MAILBOX, false);
	wr(ENABLE_BASIC, 0xff);
	for (unsigned n = 0; n < BCM2835_IC_NUM_ARM; n++) {
		arm(n, true);
		CHECK_EQ(rd(BASIC_PENDING), (2u << n) - 1);
	}
	CHECK_EQ(rd(PENDING1), 0);
	teardown();
}

/*
 * Section 7.5: the basic pending register shows eleven selected GPU
 * interrupts in bits 10 to 20, and its bits 8 and 9 only the others.
 */
static void test_selected(void)
{
	static const uint8_t selected[] = {7,  9,  10, 18, 19, 53,
	                                   54, 55, 56, 57, 62};

	setup();

	/* Not enabled, they are not pending there either. */
	for (unsigned i = 0; i < ARRAY_SIZE(selected); i++)
		gpu(selected[i], true);
	CHECK_EQ(rd(BASIC_PENDING), 0);
	for (unsigned i = 0; i < ARRAY_SIZE(selected); i++)
		gpu(selected[i], false);

	wr(ENABLE1, 0xffffffff);
	wr(ENABLE2, 0xffffffff);
	for (unsigned i = 0; i < ARRAY_SIZE(selected); i++) {
		unsigned n = selected[i];

		gpu(n, true);
		CHECK_EQ(rd(BASIC_PENDING), 1u << (10 + i));
		CHECK_EQ(rd(n < 32 ? PENDING1 : PENDING2), 1u << (n % 32));
		CHECK(irq_level(&irq_out));
		gpu(n, false);
	}

	/* Every other one sets bit 8 or 9 instead. */
	for (unsigned n = 0; n < BCM2835_IC_NUM_GPU; n++) {
		bool is_selected = false;

		for (unsigned i = 0; i < ARRAY_SIZE(selected); i++)
			is_selected |= selected[i] == n;
		if (is_selected)
			continue;
		gpu(n, true);
		CHECK_EQ(rd(BASIC_PENDING), n < 32 ? 1u << 8 : 1u << 9);
		gpu(n, false);
	}
	teardown();
}

/*
 * Section 7.3: the FIQ follows the one interrupt that the FIQ register
 * selects, whether or not its IRQ is enabled: 0 to 63 a GPU interrupt,
 * 64 to 71 an ARM interrupt.
 */
static void test_fiq(void)
{
	setup();
	gpu(57, true);
	wr(FIQ_CONTROL, 57);
	CHECK(!irq_level(&fiq_out));
	wr(FIQ_CONTROL, FIQ_ENABLE | 57);
	CHECK_EQ(rd(FIQ_CONTROL), FIQ_ENABLE | 57);
	CHECK(irq_level(&fiq_out));
	CHECK(!irq_level(&irq_out));
	gpu(57, false);
	CHECK(!irq_level(&fiq_out));
	gpu(56, true);
	CHECK(!irq_level(&fiq_out));

	/* Enabled as an IRQ too, it drives both. */
	gpu(57, true);
	wr(ENABLE2, 1u << 25);
	CHECK(irq_level(&irq_out));
	CHECK(irq_level(&fiq_out));

	/* 64 + n: ARM interrupt n. */
	wr(FIQ_CONTROL, FIQ_ENABLE | (64 + BCM2835_IC_ILLEGAL_ACCESS0));
	CHECK(!irq_level(&fiq_out));
	arm(BCM2835_IC_ILLEGAL_ACCESS0, true);
	CHECK(irq_level(&fiq_out));
	wr(FIQ_CONTROL, FIQ_ENABLE | 64);
	CHECK(!irq_level(&fiq_out));
	arm(BCM2835_IC_ARM_TIMER, true);
	CHECK(irq_level(&fiq_out));
	wr(FIQ_CONTROL, 0xffffff40);
	CHECK_EQ(rd(FIQ_CONTROL), 0x40);
	CHECK(!irq_level(&fiq_out));

	/* 72 to 127, which the document says not to use. */
	wr(FIQ_CONTROL, 127);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	CHECK_EQ(rd(FIQ_CONTROL), 127);
	wr(FIQ_CONTROL, FIQ_ENABLE | 72);
	CHECK_EQ(m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "FIQ source 72 is one that the chip's "
	                              "document says not to use") != NULL);
	CHECK_EQ(rd(FIQ_CONTROL), 127);
	teardown();
}

/* A line connected now takes the output's level. */
static void test_connect(void)
{
	IrqLine irq, fiq;

	setup();
	wr(ENABLE1, 1);
	wr(FIQ_CONTROL, FIQ_ENABLE | 1);
	gpu(0, true);
	gpu(1, true);
	irq_init(&irq, NULL, NULL, 0);
	irq_init(&fiq, NULL, NULL, 0);
	bcm2835_ic_connect(ic, &irq, &fiq);
	CHECK(irq_level(&irq));
	CHECK(irq_level(&fiq));
	bcm2835_ic_connect(ic, NULL, NULL);
	gpu(0, false);
	CHECK(irq_level(&irq));
	teardown();
}

/* The rest of the block, and other sizes, stop the machine. */
static void test_not_implemented(void)
{
	uint64_t value;

	setup();
	value = rd(BASE + 0x28);
	CHECK_EQ(value, 0);
	CHECK_EQ(m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(m->stop_message, "bcm2835-ic: 4-byte read from offset "
	                              "0x028 is not implemented yet") != NULL);
	m->stop = MACHINE_STOP_NONE;
	wr(BASE + 0x1fc, 1);
	CHECK(strstr(m->stop_message, "4-byte write to offset 0x1fc") != NULL);
	m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(bus_read(m->bus, ENABLE1, 1, &value), BUS_OK);
	CHECK(strstr(m->stop_message, "1-byte read from offset 0x010") != NULL);
	m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(bus_write(m->bus, ENABLE1, 2, 0xffff), BUS_OK);
	CHECK(strstr(m->stop_message, "2-byte write to offset 0x010") != NULL);
	CHECK_EQ(rd(ENABLE1), 0);
	teardown();
}

int main(void)
{
	RUN_TEST(test_reset);
	RUN_TEST(test_enable_and_disable);
	RUN_TEST(test_pending);
	RUN_TEST(test_selected);
	RUN_TEST(test_fiq);
	RUN_TEST(test_connect);
	RUN_TEST(test_not_implemented);
	return test_summary();
}
