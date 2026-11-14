/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the BCM2835's EMMC controller (src/hw/sd/bcm2835_emmc.c),
 * from chapter 5 of the BCM2835 ARM Peripherals datasheet, with the WiFi
 * chip that does not answer memory card commands.
 */

#include <stdint.h>
#include <string.h>

#include "core/bus.h"
#include "core/event.h"
#include "core/irq.h"
#include "core/machine.h"
#include "hw/sd/bcm2835_emmc.h"
#include "test.h"
#include "util/macros.h"
#include "util/muldiv.h"

#define BASE 0x3f300000
#define BASE_HZ 200000000

enum {
	ARG2 = 0x00,
	BLKSIZECNT = 0x04,
	ARG1 = 0x08,
	CMDTM = 0x0c,
	RESP0 = 0x10,
	STATUS = 0x24,
	CONTROL0 = 0x28,
	CONTROL1 = 0x2c,
	INTERRUPT = 0x30,
	IRPT_MASK = 0x34,
	IRPT_EN = 0x38,
	CAPS = 0x40,
	CONTROL2 = 0x3c,
	CAPS_1 = 0x44,
	FORCE_IRPT = 0x50,
	BOOT_TIMEOUT = 0x70,
	DBG_SEL = 0x74,
	EXRDFIFO_CFG = 0x80,
	EXRDFIFO_EN = 0x84,
	TUNE_STEP = 0x88,
	TUNE_STEPS_STD = 0x8c,
	TUNE_STEPS_DDR = 0x90,
	SPI_INT_SPT = 0xf0,
	SLOTISR_VER = 0xfc,
};

#define CLK_INTLEN 1u
#define CLK_STABLE 2u
#define CLK_EN 4u
#define CMD_DONE 1u
#define ERR (1u << 15)
#define CTO_ERR (1u << 16)
#define CMD(index, rsp) ((uint32_t)(index) << 24 | (uint32_t)(rsp) << 16)

typedef struct Setup {
	Machine *m;
	IrqLine irq;
	Bcm2835Emmc *emmc;
} Setup;

static void setup(Setup *s)
{
	memset(s, 0, sizeof(*s));
	s->m = machine_create();
	CHECK(s->m != NULL);
	irq_init(&s->irq, NULL, NULL, 0);
	s->emmc = bcm2835_emmc_create(s->m, BASE_HZ, &s->irq);
	CHECK(s->emmc != NULL);
	CHECK_EQ(bcm2835_emmc_map(s->emmc, s->m->bus, "emmc", BASE), BUS_OK);
}

static void teardown(Setup *s)
{
	bcm2835_emmc_destroy(s->emmc);
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

static void run_until(Setup *s, uint64_t t)
{
	event_queue_run_until(&s->m->events, t);
}

/* The time of n SD clocks at base / (2 div), or the base clock for 0. */
static uint64_t clocks(uint64_t n, uint32_t div)
{
	return mul_div_ceil(n, 1000000000, div ? BASE_HZ / (2 * div) : BASE_HZ);
}

/* The clock on, divided by div, and every flag enabled. */
static void clock_on(Setup *s, uint32_t div)
{
	wr(s, CONTROL1,
	   CLK_INTLEN | CLK_EN | (div & 0xff) << 8 | (div >> 8 & 3) << 6);
	wr(s, IRPT_MASK, 0xffffffff);
}

/* Reset values, the capabilities and the version. */
static void test_reset_values(void)
{
	Setup s;

	setup(&s);
	CHECK_EQ(rd(&s, STATUS), 0x1ff00000); /* DAT, CMD lines high */
	CHECK_EQ(rd(&s, CONTROL0), 0);
	CHECK_EQ(rd(&s, CONTROL1), 0);
	CHECK_EQ(rd(&s, INTERRUPT), 0);
	CHECK_EQ(rd(&s, CAPS), 0x01210000);
	CHECK_EQ(rd(&s, CAPS_1), 0x00000030);
	CHECK_EQ(rd(&s, SLOTISR_VER), 0x00020000);
	/* The internal clock is stable as soon as it runs. */
	wr(&s, CONTROL1, CLK_INTLEN);
	CHECK_EQ(rd(&s, CONTROL1), CLK_INTLEN | CLK_STABLE);
	wr(&s, CONTROL1, CLK_STABLE); /* read-only */
	CHECK_EQ(rd(&s, CONTROL1), 0);
	CHECK(!irq_level(&s.irq));
	CHECK(bcm2835_emmc_create(s.m, 0, &s.irq) == NULL);
	teardown(&s);
}

/*
 * Commands: none is answered. Without a response, CMD_DONE after 48 bits
 * and Ncc; with one, a timeout after 48 bits and 64 clocks of Ncr.
 */
static void test_commands(void)
{
	Setup s;
	uint64_t t;

	setup(&s);
	clock_on(&s, 250); /* 400 kHz */
	wr(&s, ARG1, 0);
	t = s.m->events.now;
	wr(&s, CMDTM, CMD(0, 0));
	CHECK_EQ(rd(&s, STATUS) & 1, 1); /* CMD_INHIBIT */
	run_until(&s, t + clocks(56, 250) - 1);
	CHECK_EQ(rd(&s, INTERRUPT), 0);
	run_until(&s, t + clocks(56, 250));
	CHECK_EQ(rd(&s, INTERRUPT), CMD_DONE);
	CHECK_EQ(rd(&s, STATUS) & 1, 0);
	wr(&s, INTERRUPT, CMD_DONE);
	CHECK_EQ(rd(&s, INTERRUPT), 0);

	/* CMD8, R7: no answer. */
	wr(&s, ARG1, 0x1aa);
	t = s.m->events.now;
	wr(&s, CMDTM, CMD(8, 2));
	CHECK_EQ(rd(&s, CMDTM), CMD(8, 2));
	run_until(&s, t + clocks(112, 250) - 1);
	CHECK_EQ(rd(&s, INTERRUPT), 0);
	run_until(&s, t + clocks(112, 250));
	CHECK_EQ(rd(&s, INTERRUPT), ERR | CTO_ERR);
	CHECK_EQ(rd(&s, STATUS) & 1, 0);
	wr(&s, INTERRUPT, ERR); /* read-only: stays */
	CHECK_EQ(rd(&s, INTERRUPT), ERR | CTO_ERR);
	wr(&s, INTERRUPT, CTO_ERR);
	CHECK_EQ(rd(&s, INTERRUPT), 0);

	/* The base clock undivided; a long response. */
	clock_on(&s, 0);
	t = s.m->events.now;
	wr(&s, CMDTM, CMD(2, 1));
	run_until(&s, t + clocks(112, 0) - 1);
	CHECK_EQ(rd(&s, INTERRUPT), 0);
	run_until(&s, t + clocks(112, 0));
	CHECK_EQ(rd(&s, INTERRUPT), ERR | CTO_ERR);
	wr(&s, INTERRUPT, CTO_ERR);

	/* A divider that needs its two high bits: 0x301. */
	clock_on(&s, 0x301);
	t = s.m->events.now;
	wr(&s, CMDTM, CMD(0, 0));
	run_until(&s, t + clocks(56, 0x301) - 1);
	CHECK_EQ(rd(&s, INTERRUPT), 0);
	run_until(&s, t + clocks(56, 0x301));
	CHECK_EQ(rd(&s, INTERRUPT), CMD_DONE);
	wr(&s, INTERRUPT, CMD_DONE);
	clock_on(&s, 0x200);
	t = s.m->events.now;
	wr(&s, CMDTM, CMD(0, 0));
	run_until(&s, t + clocks(56, 0x200) - 1);
	CHECK_EQ(rd(&s, INTERRUPT), 0);
	run_until(&s, t + clocks(56, 0x200));
	CHECK_EQ(rd(&s, INTERRUPT), CMD_DONE);
	teardown(&s);
}

/* Flags only where IRPT_MASK enables them; the line where IRPT_EN does. */
static void test_interrupts(void)
{
	Setup s;

	setup(&s);
	clock_on(&s, 0);
	wr(&s, IRPT_MASK, CTO_ERR);
	wr(&s, CMDTM, CMD(0, 0));
	run_until(&s, s.m->events.now + 1000000);
	CHECK_EQ(rd(&s, INTERRUPT), 0); /* CMD_DONE not enabled */
	wr(&s, CMDTM, CMD(8, 2));
	run_until(&s, s.m->events.now + 1000000);
	CHECK_EQ(rd(&s, INTERRUPT), ERR | CTO_ERR);
	CHECK(!irq_level(&s.irq));
	CHECK_EQ(rd(&s, SLOTISR_VER), 0x00020000);
	wr(&s, IRPT_EN, CMD_DONE);
	CHECK(!irq_level(&s.irq));
	wr(&s, IRPT_EN, CTO_ERR);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(rd(&s, SLOTISR_VER), 0x00020001);
	wr(&s, INTERRUPT, CTO_ERR);
	CHECK(!irq_level(&s.irq));
	/* FORCE_IRPT sets flags. */
	wr(&s, FORCE_IRPT, CTO_ERR | CMD_DONE);
	CHECK_EQ(rd(&s, INTERRUPT), ERR | CTO_ERR | CMD_DONE);
	CHECK(irq_level(&s.irq));
	teardown(&s);
}

/* SRST_CMD ends a command; SRST_HC resets everything. */
static void test_resets(void)
{
	Setup s;

	setup(&s);
	clock_on(&s, 0);
	wr(&s, CONTROL0, 0x0f02);
	wr(&s, CMDTM, CMD(8, 2));
	wr(&s, CONTROL1, CLK_INTLEN | CLK_EN | 1u << 25);
	CHECK_EQ(rd(&s, STATUS) & 1, 0);
	CHECK_EQ(rd(&s, CONTROL1), CLK_INTLEN | CLK_STABLE | CLK_EN);
	run_until(&s, s.m->events.now + 1000000);
	CHECK_EQ(rd(&s, INTERRUPT), 0); /* the command never ended */
	wr(&s, CMDTM, CMD(8, 2));
	wr(&s, IRPT_EN, 0xffffffff);
	wr(&s, CONTROL1, 1u << 24);
	CHECK_EQ(rd(&s, CONTROL0), 0);
	CHECK_EQ(rd(&s, CONTROL1), 0);
	CHECK_EQ(rd(&s, STATUS), 0x1ff00000);
	run_until(&s, s.m->events.now + 1000000);
	CHECK_EQ(rd(&s, INTERRUPT), 0);
	CHECK(!irq_level(&s.irq));
	/* A reset of the board too. */
	clock_on(&s, 0);
	wr(&s, CMDTM, CMD(8, 2));
	bcm2835_emmc_reset(s.emmc);
	run_until(&s, s.m->events.now + 1000000);
	CHECK_EQ(rd(&s, INTERRUPT), 0);
	CHECK_EQ(rd(&s, CONTROL1), 0);
	teardown(&s);
}

/* Each register keeps its bits, and changes no other. */
static void test_register_values(void)
{
	static const struct {
		uint64_t offset;
		uint32_t value, read;
	} regs[] = {
		{ARG2, 0xffffffff, 0xffffffff},
		{BLKSIZECNT, 0xffffffff, 0xffff03ff},
		{ARG1, 0xffffffff, 0xffffffff},
		{CONTROL0, 0x00000f02, 0x00000f02},
		{IRPT_MASK, 0xffffffff, 0xffffffff},
		{IRPT_EN, 0xffffffff, 0xffffffff},
		{CONTROL2, 0x00000004, 0x00000004},
		{BOOT_TIMEOUT, 0xffffffff, 0xffffffff},
		{DBG_SEL, 0xffffffff, 1},
		{EXRDFIFO_CFG, 0xffffffff, 7},
		{EXRDFIFO_EN, 0xffffffff, 1},
		{TUNE_STEP, 0xffffffff, 7},
		{TUNE_STEPS_STD, 0xffffffff, 0x3f},
		{TUNE_STEPS_DDR, 0xffffffff, 0x3f},
		{SPI_INT_SPT, 0xffffffff, 0xff},
		{INTERRUPT, 0xffffffff, 0},
		{CMDTM, 0, 0},    /* read only here: a write is a command */
		{CONTROL1, 0, 0}, /* read only here: its writes reset */
		{RESP0, 0, 0},
		{RESP0 + 4, 0, 0},
		{RESP0 + 8, 0, 0},
		{RESP0 + 12, 0, 0},
	};
	const size_t n = ARRAY_SIZE(regs);
	Setup s;

	setup(&s);
	for (size_t i = 0; i < n; i++) {
		if (regs[i].offset == CMDTM || regs[i].offset == CONTROL1 ||
		    (regs[i].offset & ~0xfu) == RESP0)
			continue;
		wr(&s, regs[i].offset, regs[i].value);
		for (size_t j = 0; j < n; j++)
			CHECK_EQ(rd(&s, regs[j].offset), i == j ? regs[j].read : 0);
		wr(&s, regs[i].offset, 0);
	}
	/* The host controller's reset: all back, with the clock bits written. */
	for (size_t i = 0; i < n; i++)
		if (regs[i].offset != CMDTM && regs[i].offset != CONTROL1 &&
		    (regs[i].offset & ~0xfu) != RESP0)
			wr(&s, regs[i].offset, regs[i].value);
	wr(&s, CONTROL1, 1u << 24 | CLK_INTLEN | CLK_EN);
	for (size_t i = 0; i < n; i++)
		CHECK_EQ(rd(&s, regs[i].offset), 0);
	CHECK_EQ(rd(&s, CONTROL1), 0);
	/* SRST_DATA ends at once: CONTROL1 does not keep it. */
	wr(&s, CONTROL1, 1u << 26 | CLK_INTLEN);
	CHECK_EQ(rd(&s, CONTROL1), CLK_INTLEN | CLK_STABLE);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/*
 * The line rises when a command ends; a CONTROL1 write without
 * SRST_CMD leaves the command alone; a reset drops the line.
 */
static void test_command_end(void)
{
	Setup s;

	setup(&s);
	clock_on(&s, 0);
	wr(&s, IRPT_EN, 0xffffffff);
	wr(&s, CMDTM, CMD(0, 0));
	wr(&s, CONTROL1, CLK_INTLEN | CLK_EN);
	run_until(&s, s.m->events.now + 1000000);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(rd(&s, INTERRUPT), CMD_DONE);
	wr(&s, CONTROL1, CLK_INTLEN | CLK_EN); /* the flags stay */
	CHECK_EQ(rd(&s, INTERRUPT), CMD_DONE);
	wr(&s, CONTROL1, 1u << 24);
	CHECK(!irq_level(&s.irq));
	/* FORCE_IRPT sets flags and nothing else; a board reset drops them. */
	wr(&s, IRPT_EN, 0xffffffff);
	wr(&s, FORCE_IRPT, CMD_DONE);
	CHECK(irq_level(&s.irq));
	CHECK_EQ(rd(&s, BOOT_TIMEOUT), 0);
	bcm2835_emmc_reset(s.emmc);
	CHECK(!irq_level(&s.irq));
	/* Destroyed with a command on the line, which never ends. */
	clock_on(&s, 0);
	wr(&s, CMDTM, CMD(0, 0));
	bcm2835_emmc_destroy(s.emmc);
	run_until(&s, s.m->events.now + 1000000);
	machine_destroy(s.m);
}

/* What the controller or the WiFi chip would need stops the machine. */
static void test_stops(void)
{
	static const struct {
		bool write;
		uint64_t offset;
		unsigned size;
		uint32_t value;
		bool clock;
		const char *message;
	} stops[] = {
		{true, CMDTM, 4, CMD(5, 2), true, "an SDIO command"},
		{true, CMDTM, 4, CMD(52, 2), true, "an SDIO command"},
		{true, CMDTM, 4, CMD(53, 2), true, "an SDIO command"},
		{true, CMDTM, 4, CMD(0, 0), false, "a command without the SD clock"},
		{false, 0x20, 4, 0, true, "no card sends data"},
		{true, 0x20, 4, 0, true, "no card takes data"},
		{false, 0x48, 4, 0, true, "is not a documented register"},
		{true, STATUS, 4, 0, true, "is not a register that software"},
		{true, CAPS, 4, 0, true, "is not a register that software"},
		{false, CONTROL1, 2, 0, true, "is not a 32-bit register read"},
		{true, 0x2f, 1, 1, true, "is not a 32-bit register write"},
		{true, CONTROL0, 2, 0, true, "is not a 32-bit register write"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(stops); i++) {
		Setup s;
		uint64_t v;

		setup(&s);
		if (stops[i].clock)
			clock_on(&s, 0);
		if (stops[i].write)
			bus_write(s.m->bus, BASE + stops[i].offset, stops[i].size,
			          stops[i].value);
		else
			bus_read(s.m->bus, BASE + stops[i].offset, stops[i].size, &v);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, "bcm2835-emmc: ") != NULL);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		teardown(&s);
	}

	/* A command while one is on the line; the extent of the registers. */
	Setup s;
	uint64_t v;

	setup(&s);
	clock_on(&s, 0);
	wr(&s, CMDTM, CMD(8, 2));
	bus_write(s.m->bus, BASE + CMDTM, 4, CMD(8, 2));
	CHECK(strstr(s.m->stop_message, "the last command has not finished"));
	teardown(&s);
	/* The internal clock alone is not the SD clock. */
	setup(&s);
	wr(&s, CONTROL1, CLK_INTLEN);
	bus_write(s.m->bus, BASE + CMDTM, 4, CMD(0, 0));
	CHECK(strstr(s.m->stop_message, "a command without the SD clock"));
	CHECK_EQ(bus_read(s.m->bus, BASE + 0x100, 4, &v), BUS_ERR_UNMAPPED);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_reset_values);
	RUN_TEST(test_commands);
	RUN_TEST(test_interrupts);
	RUN_TEST(test_resets);
	RUN_TEST(test_register_values);
	RUN_TEST(test_command_end);
	RUN_TEST(test_stops);
	return test_summary();
}
