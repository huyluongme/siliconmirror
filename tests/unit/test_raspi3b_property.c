/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the firmware's property channel on the Raspberry Pi 3
 * (src/machine/raspi3b_property.c). Expected values come from the
 * Raspberry Pi firmware wiki, "Mailbox property interface", and from the
 * overclocking options of config.txt in the Raspberry Pi documentation.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/bus.h"
#include "core/machine.h"
#include "machine/raspi3b_property.h"
#include "test.h"
#include "util/macros.h"

#define RAM_SIZE 0x10000
#define BUF 0x1000 /* where the tests put their buffers */
#define MHZ 1000000u

#define OK 0x80000000u
#define PARSE_ERROR 0x80000001u
#define RESPONSE(len) (0x80000000u | (len))

typedef struct Setup {
	Machine *m;
	Raspi3bProperty *p;
} Setup;

static void setup(Setup *s)
{
	s->m = machine_create();
	CHECK(s->m != NULL);
	CHECK_EQ(bus_map_ram(s->m->bus, "ram", 0, RAM_SIZE), BUS_OK);
	s->p = raspi3b_property_create(s->m, RAM_SIZE);
	CHECK(s->p != NULL);
}

static void teardown(Setup *s)
{
	raspi3b_property_destroy(s->p);
	machine_destroy(s->m);
}

static void put(Setup *s, uint32_t address, const uint32_t *words, size_t n)
{
	for (size_t i = 0; i < n; i++)
		CHECK_EQ(bus_write(s->m->bus, address + 4 * i, 4, words[i]), BUS_OK);
}

static uint32_t word(Setup *s, uint32_t address)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(s->m->bus, address, 4, &value), BUS_OK);
	return (uint32_t)value;
}

/*
 * Send the buffer of n words at BUF on the property channel, through the
 * alias 0xC0000000 like Linux, and check the firmware answers with the
 * same message.
 */
static void send(Setup *s, const uint32_t *words, size_t n)
{
	uint32_t message = 0xc0000000u | BUF | 8, reply = 0;

	put(s, BUF, words, n);
	CHECK(raspi3b_property_receive(s->p, message, &reply));
	CHECK_EQ(reply, message);
	CHECK_EQ(s->m->stop, MACHINE_STOP_NONE);
}

/* Send a buffer that must stop the machine with message. */
static void send_stops(Setup *s, const uint32_t *words, size_t n,
                       const char *message)
{
	uint32_t reply = 0x5a5a;

	put(s, BUF, words, n);
	CHECK(!raspi3b_property_receive(s->p, BUF | 8, &reply));
	CHECK_EQ(reply, 0x5a5a);
	CHECK_EQ(s->m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s->m->stop_message, "firmware: ") != NULL);
	CHECK(strstr(s->m->stop_message, message) != NULL);
	s->m->stop = MACHINE_STOP_NONE;
}

/* The answer of one tag with a request of one word and an answer of two. */
static void query(Setup *s, uint32_t tag, uint32_t id, uint32_t *answer)
{
	uint32_t buf[] = {sizeof(uint32_t[8]), 0, tag, 8, 0, id, 0, 0};

	send(s, buf, ARRAY_SIZE(buf));
	CHECK_EQ(word(s, BUF + 4), OK);
	CHECK_EQ(word(s, BUF + 16), RESPONSE(8));
	CHECK_EQ(word(s, BUF + 20), id);
	*answer = word(s, BUF + 24);
}

static uint32_t rate(Setup *s, uint32_t tag, uint32_t clock)
{
	uint32_t answer;

	query(s, tag, clock, &answer);
	return answer;
}

/* Several tags in one buffer: the board's identity. */
static void test_identity(void)
{
	Setup s;
	uint32_t buf[] = {
		sizeof(uint32_t[22]),
		0,
		0x00000001,
		4,
		0,
		0, /* firmware revision */
		0x00010002,
		4,
		0,
		0, /* board revision */
		0x00010003,
		8,
		0,
		0,
		0, /* MAC address, in 6 bytes of 8 */
		0x00010004,
		8,
		0,
		0xffffffff, /* serial */
		0xffffffff,
		0,          /* end */
		0xdeadbeef, /* after the end: untouched */
	};

	setup(&s);
	send(&s, buf, ARRAY_SIZE(buf));
	CHECK_EQ(word(&s, BUF), sizeof(uint32_t[22]));
	CHECK_EQ(word(&s, BUF + 4), OK);
	CHECK_EQ(word(&s, BUF + 8), 0x00000001);
	CHECK_EQ(word(&s, BUF + 12), 4);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 20), 0x6ae89328); /* 2026-11-02 13:42:00 */
	CHECK_EQ(word(&s, BUF + 32), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 36), RASPI3B_BOARD_REVISION);
	CHECK_EQ(RASPI3B_BOARD_REVISION, 0xa02082);
	CHECK_EQ(word(&s, BUF + 48), RESPONSE(6));
	CHECK_EQ(word(&s, BUF + 52), 0xffeb27b8);
	CHECK_EQ(word(&s, BUF + 56), 0x03ee); /* 2 bytes of padding untouched */
	CHECK_EQ(word(&s, BUF + 68), RESPONSE(8));
	CHECK_EQ(word(&s, BUF + 72), 0xc0ffee03);
	CHECK_EQ(word(&s, BUF + 76), 0);
	CHECK_EQ(word(&s, BUF + 80), 0);
	CHECK_EQ(word(&s, BUF + 84), 0xdeadbeef);
	teardown(&s);
}

/* The memory split follows gpu_mem and total_mem. */
static void test_memory(void)
{
	Setup s;
	uint32_t buf[] = {sizeof(uint32_t[13]), 0, 0x00010005, 8, 0, 0, 0,
	                  0x00010006,           8, 0,          0, 0, 0};

	setup(&s);
	CHECK_EQ(raspi3b_property_arm_memory(s.p), 0x3b400000);
	send(&s, buf, ARRAY_SIZE(buf));
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(8));
	CHECK_EQ(word(&s, BUF + 20), 0);
	CHECK_EQ(word(&s, BUF + 24), 0x3b400000); /* 1024 - 76 MiB */
	CHECK_EQ(word(&s, BUF + 36), RESPONSE(8));
	CHECK_EQ(word(&s, BUF + 40), 0x3b400000);
	CHECK_EQ(word(&s, BUF + 44), 0x04c00000);

	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){
				 .total_mem_mb = 512, .gpu_mem_mb = 16, .cmdline = ""}));
	CHECK_EQ(raspi3b_property_arm_memory(s.p), 0x1f000000);
	send(&s, buf, ARRAY_SIZE(buf));
	CHECK_EQ(word(&s, BUF + 24), 0x1f000000);
	CHECK_EQ(word(&s, BUF + 40), 0x1f000000);
	CHECK_EQ(word(&s, BUF + 44), 0x01000000);
	teardown(&s);
}

/* The command line, and an answer cut to the value buffer. */
static void test_cmdline(void)
{
	Setup s;
	/* A value buffer of 32 bytes, filled with 0x5a, then the end tag. */
	uint32_t buf[] = {
		sizeof(uint32_t[15]), 0,          0x00050001, 32,         0,
		0x5a5a5a5a,           0x5a5a5a5a, 0x5a5a5a5a, 0x5a5a5a5a, 0x5a5a5a5a,
		0x5a5a5a5a,           0x5a5a5a5a, 0x5a5a5a5a, 0,          0x77};
	uint32_t short_buf[] = {sizeof(uint32_t[9]), 0,          0x00050001, 8,   0,
	                        0x5a5a5a5a,          0x5a5a5a5a, 0,          0x77};
	char text[33] = {0};

	setup(&s);
	send(&s, buf, ARRAY_SIZE(buf));
	CHECK_EQ(word(&s, BUF + 4), OK);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(0)); /* empty by default */
	CHECK_EQ(word(&s, BUF + 20), 0x5a5a5a5a);

	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){.total_mem_mb = RASPI3B_TOTAL_MEM_MB,
	                              .gpu_mem_mb = RASPI3B_GPU_MEM_MB,
	                              .cmdline = "console=serial0,115200"}));
	send(&s, buf, ARRAY_SIZE(buf));
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(22));
	CHECK_EQ(bus_read_block(s.m->bus, BUF + 20, text, 32), BUS_OK);
	CHECK(memcmp(text, "console=serial0,115200", 22) == 0);
	CHECK_EQ(word(&s, BUF + 40), 0x5a5a3030); /* "00", then the 0x5a */
	CHECK_EQ(word(&s, BUF + 48), 0x5a5a5a5a);
	CHECK_EQ(word(&s, BUF + 56), 0x77);

	/* Cut to 8 bytes; the length still says 22. */
	send(&s, short_buf, ARRAY_SIZE(short_buf));
	CHECK_EQ(word(&s, BUF + 4), OK);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(22));
	CHECK_EQ(bus_read_block(s.m->bus, BUF + 20, text, 12), BUS_OK);
	CHECK(memcmp(text, "console=", 8) == 0);
	CHECK_EQ(word(&s, BUF + 28), 0);
	CHECK_EQ(word(&s, BUF + 32), 0x77);
	teardown(&s);
}

/* Power: on and off; the state at boot is not documented. */
static void test_power(void)
{
	Setup s;
	uint32_t answer;
	uint32_t set_on[] = {sizeof(uint32_t[8]), 0, 0x00028001, 8, 0, 3, 3, 0};
	uint32_t set_off[] = {sizeof(uint32_t[8]), 0, 0x00028001, 8, 0, 3, 0, 0};
	uint32_t set_absent[] = {sizeof(uint32_t[8]), 0, 0x00028001, 8, 0, 9, 1, 0};
	uint32_t reserved[] = {sizeof(uint32_t[8]), 0, 0x00028001, 8, 0, 3, 4, 0};
	uint32_t get_sd[] = {sizeof(uint32_t[8]), 0, 0x00020001, 8, 0, 0, 0, 0};
	uint32_t timing_sd[] = {sizeof(uint32_t[8]), 0, 0x00020002, 8, 0, 8, 0, 0};

	setup(&s);
	send(&s, set_on, ARRAY_SIZE(set_on));
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(8));
	CHECK_EQ(word(&s, BUF + 20), 3);
	CHECK_EQ(word(&s, BUF + 24), 1); /* on, exists */
	query(&s, 0x00020001, 3, &answer);
	CHECK_EQ(answer, 1);
	send(&s, set_off, ARRAY_SIZE(set_off));
	CHECK_EQ(word(&s, BUF + 24), 0);
	query(&s, 0x00020001, 3, &answer);
	CHECK_EQ(answer, 0);

	/* Device 9 does not exist on the Pi 3. */
	send(&s, set_absent, ARRAY_SIZE(set_absent));
	CHECK_EQ(word(&s, BUF + 20), 9);
	CHECK_EQ(word(&s, BUF + 24), 2);
	query(&s, 0x00020001, 9, &answer);
	CHECK_EQ(answer, 2);
	query(&s, 0x00020001, 0xffffffff, &answer);
	CHECK_EQ(answer, 2);
	query(&s, 0x00020002, 9, &answer);
	CHECK_EQ(answer, 0);

	send_stops(&s, get_sd, ARRAY_SIZE(get_sd),
	           "the power state of device 0 at boot is not documented");
	send_stops(&s, timing_sd, ARRAY_SIZE(timing_sd),
	           "the power-up time of device 8 is not documented");
	send_stops(&s, reserved, ARRAY_SIZE(reserved),
	           "power state 0x4: reserved bits are set");
	/* After a reset, the state is that of boot again. */
	raspi3b_property_reset(s.p);
	get_sd[5] = 3;
	send_stops(&s, get_sd, ARRAY_SIZE(get_sd),
	           "the power state of device 3 at boot is not documented");
	teardown(&s);
}

/* The rates of the clocks of a Pi 3, by default and with enable_uart=1. */
static void test_clock_rates(void)
{
	static const struct {
		uint32_t clock, rate, max, min;
	} rates[] = {
		{3, 1200 * MHZ, 1200 * MHZ, 600 * MHZ}, /* ARM */
		{4, 400 * MHZ, 400 * MHZ, 250 * MHZ},   /* CORE */
		{5, 400 * MHZ, 400 * MHZ, 250 * MHZ},   /* V3D */
		{6, 400 * MHZ, 400 * MHZ, 250 * MHZ},   /* H264 */
		{7, 400 * MHZ, 400 * MHZ, 250 * MHZ},   /* ISP */
		{8, 450 * MHZ, 450 * MHZ, 400 * MHZ},   /* SDRAM */
		{0, 0, 0, 0},                           /* reserved */
		{11, 0, 0, 0},                          /* HEVC: not on the Pi 3 */
		{15, 0, 0, 0},
		{0xffffffff, 0, 0, 0},
	};
	static const uint32_t unknown[] = {9, 10, 12, 13, 14};
	Setup s;

	setup(&s);
	for (unsigned i = 0; i < ARRAY_SIZE(rates); i++) {
		CHECK_EQ(rate(&s, 0x00030002, rates[i].clock), rates[i].rate);
		CHECK_EQ(rate(&s, 0x00030004, rates[i].clock), rates[i].max);
		CHECK_EQ(rate(&s, 0x00030007, rates[i].clock), rates[i].min);
	}
	CHECK_EQ(rate(&s, 0x00030002, 2), 48 * MHZ);  /* UART */
	CHECK_EQ(rate(&s, 0x00030002, 1), 200 * MHZ); /* EMMC */
	for (unsigned i = 0; i < ARRAY_SIZE(unknown); i++) {
		uint32_t buf[] = {sizeof(uint32_t[8]), 0, 0x00030002, 8, 0,
		                  unknown[i],          0, 0};
		char message[64];

		snprintf(message, sizeof(message),
		         "the rates of clock %u are not documented", unknown[i]);
		send_stops(&s, buf, ARRAY_SIZE(buf), message);
	}
	for (uint32_t tag = 0x00030004; tag <= 0x00030007; tag += 3) {
		for (unsigned clock = 1; clock <= 2; clock++) { /* EMMC, UART */
			uint32_t buf[] = {sizeof(uint32_t[8]), 0, tag, 8, 0, clock, 0, 0};
			char message[64];

			snprintf(message, sizeof(message),
			         "the range of clock %u is not documented", clock);
			send_stops(&s, buf, ARRAY_SIZE(buf), message);
		}
	}

	/* enable_uart=1: the core clock stays at 250 MHz. */
	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){.total_mem_mb = RASPI3B_TOTAL_MEM_MB,
	                              .gpu_mem_mb = RASPI3B_GPU_MEM_MB,
	                              .core_freq_fixed = true,
	                              .cmdline = ""}));
	CHECK_EQ(rate(&s, 0x00030002, 4), 250 * MHZ);
	CHECK_EQ(rate(&s, 0x00030004, 4), 250 * MHZ);
	CHECK_EQ(rate(&s, 0x00030007, 4), 250 * MHZ);
	CHECK_EQ(rate(&s, 0x00030002, 3), 1200 * MHZ);
	teardown(&s);
}

/* Setting a rate: clamped to the range; fixed clocks cannot change. */
static void test_set_clock_rate(void)
{
	static const struct {
		uint32_t clock, rate, result;
	} sets[] = {
		{3, 800 * MHZ, 800 * MHZ},   {3, 2000 * MHZ, 1200 * MHZ},
		{3, 100 * MHZ, 600 * MHZ},   {3, 600 * MHZ, 600 * MHZ},
		{3, 1200 * MHZ, 1200 * MHZ}, {5, 300 * MHZ, 300 * MHZ},
		{8, 0, 400 * MHZ},           {4, 400 * MHZ, 400 * MHZ},
		{2, 48 * MHZ, 48 * MHZ},     {11, 300 * MHZ, 0},
		{99, 300 * MHZ, 0},
	};
	Setup s;

	setup(&s);
	for (unsigned i = 0; i < ARRAY_SIZE(sets); i++) {
		uint32_t buf[] = {
			sizeof(uint32_t[9]), 0, 0x00038002, 12, 0, sets[i].clock,
			sets[i].rate,        0, 0};

		send(&s, buf, ARRAY_SIZE(buf));
		CHECK_EQ(word(&s, BUF + 16), RESPONSE(8));
		CHECK_EQ(word(&s, BUF + 20), sets[i].clock);
		CHECK_EQ(word(&s, BUF + 24), sets[i].result);
		CHECK_EQ(rate(&s, 0x00030002, sets[i].clock), sets[i].result);
	}
	CHECK_EQ(rate(&s, 0x00030002, 6), 400 * MHZ); /* V3D's own rate */
	/* The range stays when the rate moves inside it. */
	{
		uint32_t arm[] = {sizeof(uint32_t[9]), 0, 0x00038002, 12, 0, 3,
		                  800 * MHZ,           0, 0};

		send(&s, arm, ARRAY_SIZE(arm));
		CHECK_EQ(rate(&s, 0x00030002, 3), 800 * MHZ);
		CHECK_EQ(rate(&s, 0x00030004, 3), 1200 * MHZ);
		CHECK_EQ(rate(&s, 0x00030007, 3), 600 * MHZ);
	}

	uint32_t core[] = {sizeof(uint32_t[9]), 0, 0x00038002, 12, 0, 4,
	                   250 * MHZ,           0, 0};
	uint32_t uart[] = {
		sizeof(uint32_t[9]), 0, 0x00038002, 12, 0, 2, 3 * MHZ, 0, 0};
	uint32_t emmc[] = {
		sizeof(uint32_t[9]), 0, 0x00038002, 12, 0, 9, 3 * MHZ, 0, 0};

	send_stops(&s, core, ARRAY_SIZE(core),
	           "changing the rate of clock 4 is not emulated");
	send_stops(&s, uart, ARRAY_SIZE(uart),
	           "changing the rate of clock 2 is not emulated");
	send_stops(&s, emmc, ARRAY_SIZE(emmc),
	           "the rates of clock 9 are not documented");

	/* A reset brings back the rates of boot. */
	raspi3b_property_reset(s.p);
	CHECK_EQ(rate(&s, 0x00030002, 5), 400 * MHZ);
	CHECK_EQ(rate(&s, 0x00030002, 8), 450 * MHZ);
	teardown(&s);
}

/* Turbo: off at boot; on only while the core clock stays at 250 MHz. */
static void test_turbo(void)
{
	Setup s;
	uint32_t answer;
	uint32_t on[] = {sizeof(uint32_t[8]), 0, 0x00038009, 8, 0, 0, 1, 0};
	uint32_t off[] = {sizeof(uint32_t[8]), 0, 0x00038009, 8, 0, 0, 0, 0};
	uint32_t level2[] = {sizeof(uint32_t[8]), 0, 0x00038009, 8, 0, 0, 2, 0};
	uint32_t set_id1[] = {sizeof(uint32_t[8]), 0, 0x00038009, 8, 0, 1, 0, 0};
	uint32_t get_id1[] = {sizeof(uint32_t[8]), 0, 0x00030009, 8, 0, 1, 0, 0};

	/* After the board revision, in the same buffer. */
	uint32_t after[] = {
		sizeof(uint32_t[17]), 0, 0x00010002, 4, 0, 0, 0x00030009, 8, 0, 0, 0,
		0x00038009,           8, 0,          0, 0, 0};

	setup(&s);
	query(&s, 0x00030009, 0, &answer);
	CHECK_EQ(answer, 0);
	send(&s, off, ARRAY_SIZE(off));
	CHECK_EQ(word(&s, BUF + 24), 0);
	send(&s, after, ARRAY_SIZE(after));
	CHECK_EQ(word(&s, BUF + 20), RASPI3B_BOARD_REVISION);
	CHECK_EQ(word(&s, BUF + 32), RESPONSE(8));
	CHECK_EQ(word(&s, BUF + 36), 0);
	CHECK_EQ(word(&s, BUF + 40), 0);
	CHECK_EQ(word(&s, BUF + 52), RESPONSE(8));
	CHECK_EQ(word(&s, BUF + 56), 0);
	CHECK_EQ(word(&s, BUF + 60), 0);
	send_stops(&s, on, ARRAY_SIZE(on),
	           "turbo level 1 would raise the core clock to 400 MHz");
	send_stops(&s, level2, ARRAY_SIZE(level2),
	           "turbo level 2 is not documented");
	send_stops(&s, set_id1, ARRAY_SIZE(set_id1),
	           "turbo id 1 is not documented");
	send_stops(&s, get_id1, ARRAY_SIZE(get_id1),
	           "turbo id 1 is not documented");

	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){.total_mem_mb = RASPI3B_TOTAL_MEM_MB,
	                              .gpu_mem_mb = RASPI3B_GPU_MEM_MB,
	                              .core_freq_fixed = true,
	                              .cmdline = ""}));
	send(&s, on, ARRAY_SIZE(on));
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(8));
	CHECK_EQ(word(&s, BUF + 20), 0);
	CHECK_EQ(word(&s, BUF + 24), 1);
	query(&s, 0x00030009, 0, &answer);
	CHECK_EQ(answer, 1);
	raspi3b_property_reset(s.p);
	query(&s, 0x00030009, 0, &answer);
	CHECK_EQ(answer, 0);
	teardown(&s);
}

/* The buffer's format: unknown tags, parse errors, short buffers. */
static void test_format(void)
{
	Setup s;
	/* An unknown tag is left alone; the next one is answered. */
	uint32_t unknown[] = {sizeof(uint32_t[12]), 0, 0x00012345, 4, 0, 0x77,
	                      0x00010002,           6, 0,          0, 0, 0};
	/* A value of 6 bytes is padded to 8. */
	uint32_t padded[] = {sizeof(uint32_t[12]), 0, 0x00010003, 6, 0, 0, 0,
	                     0x00010002,           4, 0,          0, 0};
	/* The second tag runs past the end: the first is answered. */
	uint32_t past_end[] = {sizeof(uint32_t[10]), 0, 0x00010002, 4, 0, 0,
	                       0x00010002,           8, 0,          0};
	/* A tag header cut by the end. */
	uint32_t cut[] = {sizeof(uint32_t[8]), 0, 0x00010002, 4, 0, 0,
	                  0x00010002,          4};
	/* No end tag. */
	uint32_t no_end[] = {sizeof(uint32_t[6]), 0, 0x00010002, 4, 0, 0, 0};
	/* An answer longer than the value buffer is cut. */
	uint32_t serial_cut[] = {sizeof(uint32_t[8]), 0, 0x00010004, 4, 0,
	                         0x5a5a5a5a,          0, 0x99};

	setup(&s);
	send(&s, unknown, ARRAY_SIZE(unknown));
	CHECK_EQ(word(&s, BUF + 4), OK);
	CHECK_EQ(word(&s, BUF + 16), 0);
	CHECK_EQ(word(&s, BUF + 20), 0x77);
	CHECK_EQ(word(&s, BUF + 32), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 36), RASPI3B_BOARD_REVISION);

	send(&s, padded, ARRAY_SIZE(padded));
	CHECK_EQ(word(&s, BUF + 4), OK);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(6));
	CHECK_EQ(word(&s, BUF + 36), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 40), RASPI3B_BOARD_REVISION);

	send(&s, past_end, ARRAY_SIZE(past_end));
	CHECK_EQ(word(&s, BUF + 4), PARSE_ERROR);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 32), 0);

	send(&s, cut, ARRAY_SIZE(cut));
	CHECK_EQ(word(&s, BUF + 4), PARSE_ERROR);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(4));

	send(&s, no_end, ARRAY_SIZE(no_end));
	CHECK_EQ(word(&s, BUF + 4), PARSE_ERROR);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 24), 0); /* past the size: untouched */

	send(&s, serial_cut, ARRAY_SIZE(serial_cut));
	CHECK_EQ(word(&s, BUF + 4), OK);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(8));
	CHECK_EQ(word(&s, BUF + 20), 0xc0ffee03);
	CHECK_EQ(word(&s, BUF + 24), 0);
	CHECK_EQ(word(&s, BUF + 28), 0x99);

	/* The reserved bits of a request code: U-Boot's request length. */
	uint32_t uboot[] = {sizeof(uint32_t[7]), 0, 0x00010002, 4, 4, 0, 0};

	send(&s, uboot, ARRAY_SIZE(uboot));
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 20), RASPI3B_BOARD_REVISION);

	/* A buffer of 12 bytes: only the end tag. */
	uint32_t empty[] = {12, 0, 0};

	send(&s, empty, ARRAY_SIZE(empty));
	CHECK_EQ(word(&s, BUF + 4), OK);
	teardown(&s);
}

/* What the wiki does not describe stops the machine. */
static void test_stops(void)
{
	Setup s;
	uint32_t known[] = {sizeof(uint32_t[6]), 0, 0x00048010, 0, 0, 0};
	uint32_t tag_code[] = {sizeof(uint32_t[7]), 0, 0x00010002, 4,
	                       0x80000004,          0, 0};
	uint32_t buf_code[] = {sizeof(uint32_t[3]), 1, 0};
	uint32_t tiny[] = {8, 0, 0};
	uint32_t huge[] = {(1u << 20) + 4, 0, 0};
	uint32_t past_ram[] = {RAM_SIZE - BUF + 4, 0, 0};
	uint32_t short_req[] = {sizeof(uint32_t[6]), 0, 0x00030002, 0, 0, 0};
	uint32_t short_set[] = {sizeof(uint32_t[8]), 0, 0x00028001, 4, 0, 3, 0, 0};
	uint32_t reply = 0x5a5a;

	setup(&s);
	send_stops(&s, known, ARRAY_SIZE(known), "tag 0x00048010 is not emulated");
	send_stops(&s, tag_code, ARRAY_SIZE(tag_code),
	           "a tag request code of 0x80000004 is not documented");
	send_stops(&s, buf_code, ARRAY_SIZE(buf_code),
	           "a buffer request code of 0x00000001 is not documented");
	send_stops(&s, tiny, ARRAY_SIZE(tiny),
	           "a property buffer of 8 bytes is not emulated");
	send_stops(&s, huge, ARRAY_SIZE(huge),
	           "a property buffer of 0x00100004 bytes is not emulated");
	send_stops(&s, past_ram, ARRAY_SIZE(past_ram),
	           "a property buffer of 0x0000f004 bytes is not emulated");
	send_stops(&s, short_req, ARRAY_SIZE(short_req),
	           "tag 0x00030002: a value buffer too short for the request");
	send_stops(&s, short_set, ARRAY_SIZE(short_set),
	           "tag 0x00028001: a value buffer too short for the request");
	/* Nothing was written back. */
	CHECK_EQ(word(&s, BUF + 4), 0);
	CHECK_EQ(word(&s, BUF + 16), 0);

	/* Other channels, and buffers outside the RAM. */
	CHECK(!raspi3b_property_receive(s.p, BUF | 1, &reply));
	CHECK(strstr(s.m->stop_message, "mailbox channel 1 is not emulated"));
	s.m->stop = MACHINE_STOP_NONE;
	CHECK(!raspi3b_property_receive(s.p, BUF | 9, &reply));
	CHECK(strstr(s.m->stop_message, "mailbox channel 9 is not emulated"));
	s.m->stop = MACHINE_STOP_NONE;
	CHECK(!raspi3b_property_receive(s.p, RAM_SIZE | 8, &reply));
	CHECK(strstr(s.m->stop_message,
	             "a property buffer at 0x00010000 is not in memory"));
	s.m->stop = MACHINE_STOP_NONE;
	CHECK(!raspi3b_property_receive(s.p, (RAM_SIZE - 0x10) | 8, &reply));
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	s.m->stop = MACHINE_STOP_NONE;
	CHECK_EQ(reply, 0x5a5a);
	teardown(&s);

	/* A buffer at the very end of the RAM. */
	setup(&s);
	uint32_t last[] = {12, 0, 0};

	put(&s, RAM_SIZE - 16, last, ARRAY_SIZE(last));
	CHECK(raspi3b_property_receive(s.p, (RAM_SIZE - 16) | 8, &reply));
	CHECK_EQ(word(&s, RAM_SIZE - 12), OK);
	/* The RAM ends, but the bus does not. */
	CHECK(!raspi3b_property_receive(s.p, 0x3f000008, &reply));
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	teardown(&s);
}

/*
 * SET_SDHOST_CLOCK: the firmware sets SDHOST's divider, the fastest clock
 * not above the one asked, and answers with that clock twice.
 */
static void test_sdhost_clock(void)
{
	static const struct {
		uint32_t clock, div, rate;
	} cases[] = {
		{0, 0x7ff, 250000000 / 0x801},
		{99999, 0x7ff, 250000000 / 0x801},
		{100000, 0x7ff, 250000000 / 0x801}, /* the slowest it can */
		{150000, 1665, 149970},
		{400000, 623, 400000},
		{25000000, 8, 25000000},
		{50000000, 3, 50000000},
		{41000000, 5, 35714285}, /* 250 / 6 is above 41 */
		{125000000, 0, 125000000},
		{250000000, 0, 125000000}, /* at least 2: SDCDIV 0 */
		{1, 0x7ff, 250000000 / 0x801},
		{200, 0x7ff, 250000000 / 0x801},
		{121952, 0x7ff, 250000000 / 0x801}, /* 0x800, but at most 0x7ff */
		{122070, 0x7fe, 122070},
	};
	Setup s;

	setup(&s);
	/* SDHOST's registers, as RAM, to see what the firmware writes. */
	CHECK_EQ(bus_map_ram(s.m->bus, "sdhost", 0x3f202000, 0x100), BUS_OK);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		uint32_t buf[] = {sizeof(uint32_t[9]), 0,   0x00038042, 12, 0,
		                  cases[i].clock,      ~0u, ~0u,        0};

		send(&s, buf, ARRAY_SIZE(buf));
		CHECK_EQ(word(&s, BUF + 16), RESPONSE(12));
		CHECK_EQ(word(&s, BUF + 20), cases[i].clock);
		CHECK_EQ(word(&s, BUF + 24), cases[i].rate);
		CHECK_EQ(word(&s, BUF + 28), cases[i].rate);
		CHECK_EQ(word(&s, 0x3f20200c), cases[i].div);
	}
	teardown(&s);

	/* No SDHOST where the firmware writes: the machine stops. */
	setup(&s);
	{
		uint32_t buf[] = {
			sizeof(uint32_t[9]), 0, 0x00038042, 12, 0, 400000, 0, 0, 0};

		send_stops(&s, buf, ARRAY_SIZE(buf),
		           "SDHOST is not where the firmware sets its clock");
	}
	teardown(&s);
}

/* --- The display ---------------------------------------------------------- */

#define FB_W 656 /* 720 by 480, less 32 pixels of overscan on each edge */
#define FB_H 416
#define VC_TOP 0x3f000000u  /* the VideoCore's memory, up to the peripherals */
#define ARM_END 0x3b400000u /* 948 MB: the Arm's memory, 1024 MB less 76 */
#define TAGS_MAX 300        /* words of tags in a buffer of the tests */

/* Put tags, n words, in a buffer with its header and end tag. */
static size_t wrap(uint32_t *buf, const uint32_t *tags, size_t n)
{
	CHECK(n + 3 <= TAGS_MAX);
	buf[0] = (uint32_t)(4 * (n + 3));
	buf[1] = 0;
	memcpy(buf + 2, tags, 4 * n);
	buf[n + 2] = 0;
	return n + 3;
}

/* Send tags, n words, in a buffer. */
static void send_tags(Setup *s, const uint32_t *tags, size_t n)
{
	uint32_t buf[TAGS_MAX];

	send(s, buf, wrap(buf, tags, n));
	CHECK_EQ(word(s, BUF + 4), OK);
}

/* Send tags that must stop the machine with message. */
static void tags_stop(Setup *s, const uint32_t *tags, size_t n,
                      const char *message)
{
	uint32_t buf[TAGS_MAX];

	send_stops(s, buf, wrap(buf, tags, n), message);
}

/* Word i of the tags that send_tags sent. */
static uint32_t at(Setup *s, unsigned i)
{
	return word(s, BUF + 8 + 4 * i);
}

/* The answer of one get tag of count words. */
static void get(Setup *s, uint32_t tag, unsigned count, uint32_t *values)
{
	uint32_t tags[3 + 4] = {tag, 4 * count, 0};

	CHECK(count <= 4);
	send_tags(s, tags, 3 + count);
	CHECK_EQ(at(s, 2), RESPONSE(4 * count));
	for (unsigned i = 0; i < count; i++)
		values[i] = at(s, 3 + i);
}

static uint32_t get1(Setup *s, uint32_t tag)
{
	uint32_t value;

	get(s, tag, 1, &value);
	return value;
}

/* Check the answer of Linux's GET_DISPLAY_SETTINGS for display 0. */
static void check_settings(Setup *s, const uint32_t expected[10])
{
	uint32_t tags[3 + 16] = {0x00040014, 64, 4, 0};

	for (unsigned i = 4; i < ARRAY_SIZE(tags); i++)
		tags[i] = 0x5a5a5a5a;
	send_tags(s, tags, ARRAY_SIZE(tags));
	CHECK_EQ(at(s, 2), RESPONSE(40));
	for (unsigned i = 0; i < 10; i++)
		CHECK_EQ(at(s, 3 + i), expected[i]);
	CHECK_EQ(at(s, 13), 0x5a5a5a5a);
}

/*
 * The display at boot: composite NTSC, its frame buffer 656 by 416 of 16
 * bits per pixel, not yet allocated; one display. The pixel order and
 * the alpha mode at boot are not documented.
 */
static void test_display_boot(void)
{
	static const uint32_t settings[10] = {0,    FB_W, FB_H, 16, FB_W * 2,
	                                      FB_W, FB_H, 0,    0,  0};
	Setup s;
	uint32_t v[4];

	setup(&s);
	CHECK_EQ(get1(&s, 0x00040013), 1);
	get(&s, 0x00040003, 2, v);
	CHECK_EQ(v[0], FB_W);
	CHECK_EQ(v[1], FB_H);
	get(&s, 0x00040004, 2, v);
	CHECK_EQ(v[0], FB_W);
	CHECK_EQ(v[1], FB_H);
	CHECK_EQ(get1(&s, 0x00040005), 16);
	CHECK_EQ(get1(&s, 0x00040008), FB_W * 2);
	get(&s, 0x00040009, 2, v);
	CHECK_EQ(v[0], 0);
	CHECK_EQ(v[1], 0);
	get(&s, 0x0004000a, 4, v);
	for (unsigned i = 0; i < 4; i++)
		CHECK_EQ(v[i], 32);
	check_settings(&s, settings);
	tags_stop(&s, (const uint32_t[]){0x00040006, 4, 0, 0}, 4,
	          "the pixel order at boot is not documented");
	tags_stop(&s, (const uint32_t[]){0x00040007, 4, 0, 0}, 4,
	          "the alpha mode at boot is not documented");
	teardown(&s);
}

/*
 * U-Boot 2026.07's bcm2835_set_video_params (arch/arm/mach-bcm283x/
 * msg.c): its tags, whose request codes give the requests' lengths. The
 * firmware allocates the buffer at the top of its memory.
 */
static void test_display_uboot(void)
{
	static const uint32_t tags[] = {
		0x00048003, 8,  8,  FB_W,  FB_H,       /* 0: physical size */
		0x00048004, 8,  8,  FB_W,  FB_H,       /* 5: virtual size */
		0x00048005, 4,  4,  32,                /* 10: depth */
		0x00048006, 4,  4,  1,                 /* 14: RGB */
		0x00048007, 4,  4,  2,                 /* 18: alpha ignored */
		0x00048009, 8,  8,  0,     0,          /* 22: virtual offset */
		0x0004800a, 16, 16, 0,     0,    0, 0, /* 27: no overscan */
		0x00040001, 8,  4,  0x100, 0,          /* 34: allocate */
		0x00040008, 4,  0,  0,                 /* 39: pitch */
	};
	uint32_t size = FB_W * 4 * FB_H;
	uint32_t base = 0xc0000000u | ((VC_TOP - size) & ~0xfffu);
	const uint32_t settings[10] = {0,    FB_W, FB_H, 32, FB_W * 4,
	                               FB_W, FB_H, 0,    0,  base};
	Setup s;
	uint32_t v[4];

	setup(&s);
	send_tags(&s, tags, ARRAY_SIZE(tags));
	CHECK_EQ(base, 0xfeef5000u);
	CHECK_EQ(at(&s, 2), RESPONSE(8));
	CHECK_EQ(at(&s, 3), FB_W);
	CHECK_EQ(at(&s, 4), FB_H);
	CHECK_EQ(at(&s, 7), RESPONSE(8));
	CHECK_EQ(at(&s, 8), FB_W);
	CHECK_EQ(at(&s, 9), FB_H);
	CHECK_EQ(at(&s, 12), RESPONSE(4));
	CHECK_EQ(at(&s, 13), 32);
	CHECK_EQ(at(&s, 16), RESPONSE(4));
	CHECK_EQ(at(&s, 17), 1);
	CHECK_EQ(at(&s, 20), RESPONSE(4));
	CHECK_EQ(at(&s, 21), 2);
	CHECK_EQ(at(&s, 24), RESPONSE(8));
	CHECK_EQ(at(&s, 25), 0);
	CHECK_EQ(at(&s, 26), 0);
	CHECK_EQ(at(&s, 29), RESPONSE(16));
	for (unsigned i = 30; i < 34; i++)
		CHECK_EQ(at(&s, i), 0);
	CHECK_EQ(at(&s, 36), RESPONSE(8));
	CHECK_EQ(at(&s, 37), base);
	CHECK_EQ(at(&s, 38), size);
	CHECK_EQ(at(&s, 41), RESPONSE(4));
	CHECK_EQ(at(&s, 42), FB_W * 4);

	CHECK_EQ(get1(&s, 0x00040006), 1);
	CHECK_EQ(get1(&s, 0x00040007), 2);
	get(&s, 0x0004000a, 4, v);
	for (unsigned i = 0; i < 4; i++)
		CHECK_EQ(v[i], 0);
	check_settings(&s, settings);

	/* The board starts again: the display is as at boot. */
	raspi3b_property_reset(s.p);
	CHECK_EQ(get1(&s, 0x00040005), 16);
	CHECK_EQ(get1(&s, 0x00040008), FB_W * 2);
	get(&s, 0x0004000a, 4, v);
	CHECK_EQ(v[0], 32);
	tags_stop(&s, (const uint32_t[]){0x00040006, 4, 0, 0}, 4,
	          "the pixel order at boot is not documented");
	teardown(&s);
}

/*
 * Linux's bcm2708_fb at commit cff533a: the display to act on, then a
 * buffer of the Arm's own; the old way when the firmware would not take
 * it.
 */
static void test_display_linux(void)
{
	uint32_t size = FB_W * FB_H * 2;
	uint32_t vc = 0xc0000000u | ((VC_TOP - size) & ~0xfffu);
	uint32_t tags[] = {
		0x00048003, 8, 0, FB_W,        FB_H, /* 0 */
		0x00048004, 8, 0, FB_W,        FB_H, /* 5 */
		0x00048005, 4, 0, 16,                /* 10 */
		0x00048009, 8, 0, 0,           0,    /* 14 */
		0x00040001, 8, 0, 0xc0100000u, size, /* 19 */
		0x00048008, 4, 0, FB_W * 2,          /* 24 */
	};
	Setup s;

	setup(&s);
	send_tags(&s, (const uint32_t[]){0x00048013, 4, 0, 0}, 4);
	CHECK_EQ(at(&s, 2), RESPONSE(4));
	CHECK_EQ(at(&s, 3), 0);

	send_tags(&s, tags, ARRAY_SIZE(tags));
	CHECK_EQ(at(&s, 21), RESPONSE(8));
	CHECK_EQ(at(&s, 22), 0xc0100000u);
	CHECK_EQ(at(&s, 23), size);
	CHECK_EQ(at(&s, 26), RESPONSE(4));
	CHECK_EQ(at(&s, 27), FB_W * 2);
	check_settings(&s, (const uint32_t[10]){0, FB_W, FB_H, 16, FB_W * 2, FB_W,
	                                        FB_H, 0, 0, 0xc0100000u});

	/* The old way: the firmware allocates, Linux asks for the pitch. */
	tags[22] = 0;
	tags[23] = 0;
	tags[24] = 0x00040008;
	tags[27] = 0;
	send_tags(&s, tags, ARRAY_SIZE(tags));
	CHECK_EQ(at(&s, 22), vc);
	CHECK_EQ(at(&s, 23), size);
	CHECK_EQ(at(&s, 27), FB_W * 2);
	CHECK_EQ(vc, 0xfef7a000u);

	/* A buffer of the Arm's: any alias, up to the end of its memory. */
	tags[22] = ARM_END - size;
	tags[23] = size;
	send_tags(&s, tags, ARRAY_SIZE(tags));
	CHECK_EQ(at(&s, 22), ARM_END - size);
	CHECK_EQ(at(&s, 23), size);
	tags[23] = size + 1;
	tags_stop(&s, tags, ARRAY_SIZE(tags),
	          "a frame buffer of 545793 bytes at 0x3b37ac00 is not emulated: "
	          "it must hold 545792 bytes, in the Arm's memory");
	tags[22] = 0xc0000000u | ARM_END;
	tags[23] = 1;
	tags_stop(&s, tags, ARRAY_SIZE(tags), "1 bytes at 0xfb400000 is not");
	tags[22] = ARM_END + 1;
	tags[23] = size;
	tags_stop(&s, tags, ARRAY_SIZE(tags), "at 0x3b400001 is not emulated");
	tags[22] = 0x00100000;
	tags[23] = size - 1;
	tags_stop(&s, tags, ARRAY_SIZE(tags), "at 0x00100000 is not emulated");
	teardown(&s);
}

/* Blank the screen, and the palette: only their answers. */
static void test_display_blank_palette(void)
{
	static const struct {
		uint32_t offset, length, answer; /* 0: valid */
	} palettes[] = {
		{0, 1, 0},           {0, 256, 0},         {255, 1, 0}, {255, 2, 1},
		{256, 1, 1},         {0, 0, 1},           {0, 257, 1}, {1, 256, 1},
		{0xffffffffu, 2, 1}, {2, 0xffffffffu, 1},
	};
	Setup s;

	setup(&s);
	for (uint32_t state = 0; state <= 1; state++) {
		send_tags(&s, (const uint32_t[]){0x00040002, 4, 4, state}, 4);
		CHECK_EQ(at(&s, 2), RESPONSE(4));
		CHECK_EQ(at(&s, 3), state);
	}
	for (size_t i = 0; i < ARRAY_SIZE(palettes); i++) {
		uint32_t tags[5 + 256] = {0x0004800b, 8, 0, palettes[i].offset,
		                          palettes[i].length};
		size_t n = 5;

		/* A valid palette needs its values; an invalid one does not. */
		if (palettes[i].answer == 0) {
			tags[1] = 4 * (2 + palettes[i].length);
			n += palettes[i].length;
		}
		send_tags(&s, tags, n);
		CHECK_EQ(at(&s, 2), RESPONSE(4));
		CHECK_EQ(at(&s, 3), palettes[i].answer);
	}
	tags_stop(&s, (const uint32_t[]){0x0004800b, 8, 0, 0, 1}, 5,
	          "tag 0x0004800b: a value buffer too short");
	teardown(&s);
}

/*
 * The wiki: the tags of a buffer are one operation. Get tags are
 * answered after the set tags; test tags change nothing; without an
 * allocation, settings that need a larger buffer change nothing.
 */
static void test_display_operation(void)
{
	Setup s;
	uint32_t v[2];

	setup(&s);
	/* Get after set, and the pitch of the new depth. */
	send_tags(&s,
	          (const uint32_t[]){0x00040005, 4, 0, 0, 0x00040008, 4, 0, 0,
	                             0x00048005, 4, 4, 32},
	          12);
	CHECK_EQ(at(&s, 3), 32);
	CHECK_EQ(at(&s, 7), FB_W * 4);
	CHECK_EQ(at(&s, 11), 32);

	/* Tests answer, in combination with the settings, and change nothing. */
	send_tags(&s,
	          (const uint32_t[]){0x00044003, 8, 8, 800, 600, 0x00044004, 8, 8,
	                             800, 600, 0x00044005, 4, 4, 8},
	          14);
	CHECK_EQ(at(&s, 2), RESPONSE(8));
	CHECK_EQ(at(&s, 3), 800);
	CHECK_EQ(at(&s, 4), 600);
	CHECK_EQ(at(&s, 13), 8);
	get(&s, 0x00040003, 2, v);
	CHECK_EQ(v[0], FB_W);
	CHECK_EQ(get1(&s, 0x00040005), 32);
	tags_stop(&s, (const uint32_t[]){0x00044003, 8, 8, 800, 600}, 5,
	          "a virtual offset of (0, 0) is not emulated: the display "
	          "leaves the buffer");

	/* Other tags among them are answered too. */
	send_tags(&s,
	          (const uint32_t[]){0x00048005, 4, 4, 16, 0x00010002, 4, 0, 0,
	                             0x00040008, 4, 0, 0},
	          12);
	CHECK_EQ(at(&s, 7), RASPI3B_BOARD_REVISION);
	CHECK_EQ(at(&s, 11), FB_W * 2);

	/* A buffer of 656 by 416 by 2: 32 bits need a larger one. */
	send_tags(&s, (const uint32_t[]){0x00040001, 8, 4, 0, 0}, 5);
	CHECK_EQ(at(&s, 3), 0xfef7a000u);
	send_tags(&s,
	          (const uint32_t[]){0x00048005, 4, 4, 32, 0x00048009, 8, 8, 0, 0},
	          9);
	CHECK_EQ(at(&s, 2), RESPONSE(4));
	CHECK_EQ(at(&s, 3), 16);
	CHECK_EQ(get1(&s, 0x00040005), 16);
	/* 8 bits fit in it. */
	send_tags(&s, (const uint32_t[]){0x00048005, 4, 4, 8}, 4);
	CHECK_EQ(at(&s, 3), 8);
	CHECK_EQ(get1(&s, 0x00040008), FB_W);
	CHECK_EQ(get1(&s, 0x00040005), 8);
	/* So does a buffer of the same size: 16 bits again. */
	send_tags(&s, (const uint32_t[]){0x00048005, 4, 4, 16}, 4);
	CHECK_EQ(at(&s, 3), 16);

	/* An alignment that is not a power of 2: the buffer stays. */
	send_tags(&s,
	          (const uint32_t[]){0x00048005, 4, 4, 32, 0x00040001, 8, 4, 3, 0},
	          9);
	CHECK_EQ(at(&s, 3), 16);
	CHECK_EQ(at(&s, 7), 0xfef7a000u);
	CHECK_EQ(at(&s, 8), FB_W * FB_H * 2);

	/* Released: no buffer, and settings change without one. */
	send_tags(&s, (const uint32_t[]){0x00048001, 0, 0}, 3);
	CHECK_EQ(at(&s, 2), RESPONSE(0));
	check_settings(&s, (const uint32_t[10]){0, FB_W, FB_H, 16, FB_W * 2, FB_W,
	                                        FB_H, 0, 0, 0});
	send_tags(&s,
	          (const uint32_t[]){0x00048005, 4, 4, 32, 0x00040001, 8, 4, 6, 0},
	          9);
	CHECK_EQ(at(&s, 3), 32);
	CHECK_EQ(at(&s, 7), 0);
	CHECK_EQ(at(&s, 8), 0);
	teardown(&s);
}

/* Where the firmware puts its buffer, for alignments and memory splits. */
static void test_display_allocate(void)
{
	static const struct {
		uint32_t total_mb, gpu_mb, align, base;
	} cases[] = {
		{1024, 76, 0, 0xfef7a000u},         {1024, 76, 1, 0xfef7a000u},
		{1024, 76, 0x1000, 0xfef7a000u},    {1024, 76, 0x2000, 0xfef7a000u},
		{1024, 76, 0x4000, 0xfef78000u},    {1024, 76, 0x100000, 0xfef00000u},
		{1024, 76, 0x2000000, 0xfe000000u}, {1024, 76, 0x4000000, 0xfc000000u},
		{512, 64, 0, 0xdff7a000u},          {1024, 17, 0, 0xfef7a000u},
		{32, 32, 0, 0xc1f7a000u},
	};
	uint32_t size = FB_W * FB_H * 2;
	Setup s;

	setup(&s);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		CHECK(raspi3b_property_configure(
			s.p, &(Raspi3bVcSettings){.total_mem_mb = cases[i].total_mb,
		                              .gpu_mem_mb = cases[i].gpu_mb,
		                              .cmdline = ""}));
		send_tags(&s, (const uint32_t[]){0x00040001, 8, 4, cases[i].align, 0},
		          5);
		CHECK_EQ(at(&s, 3), cases[i].base);
		CHECK_EQ(at(&s, 4), size);
	}
	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){
				 .total_mem_mb = 1024, .gpu_mem_mb = 76, .cmdline = ""}));
	/* A value buffer of one word: the next tag is no size. */
	send_tags(&s, (const uint32_t[]){0x00040001, 4, 4, 0, 0x00040013, 4, 0, 0},
	          8);
	CHECK_EQ(at(&s, 2), RESPONSE(8));
	CHECK_EQ(at(&s, 3), 0xfef7a000u);
	CHECK_EQ(at(&s, 4), 0x00040013);
	CHECK_EQ(at(&s, 7), 1);

	tags_stop(&s, (const uint32_t[]){0x00040001, 8, 4, 0x80000000u, 0}, 5,
	          "a frame buffer of 545792 bytes is not emulated: it does not "
	          "fit in the VideoCore's memory");
	tags_stop(&s, (const uint32_t[]){0x00040001, 8, 4, 0x08000000u, 0}, 5,
	          "does not fit");
	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){
				 .total_mem_mb = 1024, .gpu_mem_mb = 16, .cmdline = ""}));
	tags_stop(&s, (const uint32_t[]){0x00040001, 8, 4, 0, 0}, 5,
	          "does not fit");
	/* 32 MB, all the VideoCore's: 64 MB do not fit. */
	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){
				 .total_mem_mb = 32, .gpu_mem_mb = 32, .cmdline = ""}));
	tags_stop(&s,
	          (const uint32_t[]){0x00048003, 8, 8, 4096, 4096, 0x00048004, 8, 8,
	                             4096, 4096, 0x00048005, 4, 4, 32, 0x00040001,
	                             8, 4, 0, 0},
	          19, "a frame buffer of 67108864 bytes is not emulated");
	/* 4096 by 4096 at 32 bits: 64 MB, more than the VideoCore's memory. */
	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){
				 .total_mem_mb = 1024, .gpu_mem_mb = 76, .cmdline = ""}));
	tags_stop(&s,
	          (const uint32_t[]){0x00048003, 8, 8, 4096, 4096, 0x00048004, 8, 8,
	                             4096, 4096, 0x00048005, 4, 4, 32, 0x00040001,
	                             8, 4, 0, 0},
	          19, "a frame buffer of 67108864 bytes is not emulated");
	/* At 8 bits, 16 MB: it fits. */
	send_tags(&s,
	          (const uint32_t[]){0x00048003, 8, 8, 4096, 4096, 0x00048004, 8, 8,
	                             4096, 4096, 0x00048005, 4, 4, 8, 0x00040001, 8,
	                             4, 0, 0},
	          19);
	CHECK_EQ(at(&s, 17), 0xfe000000u);
	CHECK_EQ(at(&s, 18), 4096 * 4096);
	teardown(&s);
}

/* Every test tag, and depths of 24 bits. */
static void test_display_tests(void)
{
	static const uint32_t tags[] = {
		0x00044003, 8,  8,  FB_W, FB_H,       /* 0 */
		0x00044004, 8,  8,  FB_W, FB_H,       /* 5 */
		0x00044005, 4,  4,  24,               /* 10 */
		0x00044006, 4,  4,  0,                /* 14 */
		0x00044007, 4,  4,  1,                /* 18 */
		0x00044009, 8,  8,  0,    0,          /* 22 */
		0x0004400a, 16, 16, 1,    2,    3, 4, /* 27 */
	};
	Setup s;
	uint32_t v[4];

	setup(&s);
	send_tags(&s, tags, ARRAY_SIZE(tags));
	CHECK_EQ(at(&s, 3), FB_W);
	CHECK_EQ(at(&s, 8), FB_W);
	CHECK_EQ(at(&s, 13), 24);
	CHECK_EQ(at(&s, 16), RESPONSE(4));
	CHECK_EQ(at(&s, 17), 0);
	CHECK_EQ(at(&s, 20), RESPONSE(4));
	CHECK_EQ(at(&s, 21), 1);
	CHECK_EQ(at(&s, 24), RESPONSE(8));
	CHECK_EQ(at(&s, 29), RESPONSE(16));
	CHECK_EQ(at(&s, 33), 4);
	CHECK_EQ(get1(&s, 0x00040005), 16);
	get(&s, 0x0004000a, 4, v);
	CHECK_EQ(v[3], 32);

	send_tags(&s,
	          (const uint32_t[]){0x00048005, 4, 4, 24, 0x00040001, 8, 4, 0, 0,
	                             0x00040008, 4, 0, 0},
	          13);
	CHECK_EQ(at(&s, 3), 24);
	CHECK_EQ(at(&s, 8), FB_W * 3 * FB_H);
	CHECK_EQ(at(&s, 12), FB_W * 3);
	teardown(&s);
}

/* The pitch that Linux sets, and the pitch of a new virtual width. */
static void test_display_pitch(void)
{
	Setup s;
	uint32_t vc = 0xc0000000u | ((VC_TOP - 16384 * FB_H) & ~0xfffu);

	setup(&s);
	send_tags(
		&s, (const uint32_t[]){0x00048008, 4, 4, 16384, 0x00040001, 8, 4, 0, 0},
		9);
	CHECK_EQ(at(&s, 3), 16384);
	CHECK_EQ(at(&s, 7), vc);
	CHECK_EQ(at(&s, 8), 16384 * FB_H);
	/* Other settings keep it. */
	send_tags(&s, (const uint32_t[]){0x00048006, 4, 4, 1}, 4);
	CHECK_EQ(get1(&s, 0x00040008), 16384);
	/* A new virtual width, of the same depth: a line of its pixels. */
	send_tags(
		&s,
		(const uint32_t[]){0x00048004, 8, 8, 700, FB_H, 0x00040001, 8, 4, 0, 0},
		10);
	CHECK_EQ(get1(&s, 0x00040008), 1400);
	teardown(&s);
}

/*
 * A buffer that leaves the display at an offset; buffers that end where
 * they must; an odd size. The board starts again with the boot display.
 */
static void test_display_offset(void)
{
	static const uint32_t boot[10] = {0,    FB_W, FB_H, 16, FB_W * 2,
	                                  FB_W, FB_H, 0,    0,  0};
	uint32_t base = 0xc0000000u | ((VC_TOP - 1400 * 450) & ~0xfffu);
	Setup s;

	setup(&s);
	send_tags(&s,
	          (const uint32_t[]){0x00048004, 8, 8, 700, 450, 0x00048009, 8, 8,
	                             3, 4, 0x00040001, 8, 4, 0, 0},
	          15);
	CHECK_EQ(at(&s, 8), 3);
	CHECK_EQ(at(&s, 9), 4);
	check_settings(&s, (const uint32_t[10]){0, FB_W, FB_H, 16, 1400, 700, 450,
	                                        3, 4, base});
	raspi3b_property_reset(s.p);
	check_settings(&s, boot);

	/* 4096 by 3840 by 4: 60 MB, all the VideoCore's memory. */
	send_tags(&s,
	          (const uint32_t[]){0x00048003, 8, 8, 4096, 3840, 0x00048004, 8, 8,
	                             4096, 3840, 0x00048005, 4, 4, 32, 0x00040001,
	                             8, 4, 0, 0},
	          19);
	CHECK_EQ(at(&s, 17), 0xc0000000u | ARM_END);
	CHECK_EQ(at(&s, 18), VC_TOP - ARM_END);

	/* 655 by 415 by 1: an odd size, the base still aligned. */
	send_tags(&s,
	          (const uint32_t[]){0x00048003, 8, 8, 655, 415, 0x00048004, 8, 8,
	                             655, 415, 0x00048005, 4, 4, 8, 0x00040001, 8,
	                             4, 0, 0},
	          19);
	CHECK_EQ(at(&s, 17), 0xfefbd000u);
	CHECK_EQ(at(&s, 18), 655 * 415);
	send_tags(&s, (const uint32_t[]){0x00040001, 8, 4, 0x100000, 0}, 5);
	CHECK_EQ(at(&s, 3), 0xfef00000u);
	teardown(&s);
}

/* What tells an alignment from a buffer of the Arm's. */
static void test_display_alignment(void)
{
	Setup s;

	setup(&s);
	/* A value buffer of 7 bytes: one word of request, no size. */
	send_tags(&s, (const uint32_t[]){0x00040001, 7, 4, 3, 0x1234}, 5);
	CHECK_EQ(at(&s, 2), RESPONSE(8));
	CHECK_EQ(at(&s, 3), 0);
	/* The word after the request is another tag's. */
	send_tags(
		&s, (const uint32_t[]){0x00040001, 8, 4, 3, 0, 0x00040013, 4, 0, 0}, 9);
	CHECK_EQ(at(&s, 3), 0);
	CHECK_EQ(at(&s, 4), 0);
	CHECK_EQ(at(&s, 8), 1);
	/* 7 bytes again, and an alignment: the firmware allocates. */
	send_tags(&s, (const uint32_t[]){0x00040001, 7, 4, 0, 0x1234}, 5);
	CHECK_EQ(at(&s, 3), 0xfef7a000u);
	teardown(&s);
}

/* What nothing documents stops the machine. */
static void test_display_stops(void)
{
	static const struct {
		uint32_t words[12];
		size_t n;
		const char *message;
	} stops[] = {
		{{0x00048003, 8, 8, 0, FB_H},
	     5,
	     "a frame buffer of 0 by 416, 656 by 416 virtual, is not emulated"},
		{{0x00048003, 8, 8, FB_W, 0}, 5, "of 656 by 0, 656 by 416 virtual"},
		{{0x00048004, 8, 8, 0, FB_H}, 5, "of 656 by 416, 0 by 416 virtual"},
		{{0x00048004, 8, 8, FB_W, 0}, 5, "of 656 by 416, 656 by 0 virtual"},
		{{0x00048004, 8, 8, 4097, FB_H}, 5, "656 by 416, 4097 by 416 virtual"},
		{{0x00048004, 8, 8, FB_W, 4097}, 5, "656 by 416, 656 by 4097 virtual"},
		{{0x00048005, 4, 4, 0}, 4, "a depth of 0 bits per pixel is not"},
		{{0x00048005, 4, 4, 15}, 4, "a depth of 15 bits"},
		{{0x00048005, 4, 4, 33}, 4, "a depth of 33 bits"},
		{{0x00048005, 4, 4, 4}, 4, "a depth of 4 bits"},
		{{0x00048006, 4, 4, 2}, 4, "pixel order 2 is not in the wiki"},
		{{0x00048006, 4, 4, 0xfffffffe}, 4, "pixel order 4294967294 is not"},
		{{0x00048007, 4, 4, 3}, 4, "alpha mode 3 is not in the wiki"},
		{{0x00048009, 8, 8, 1, 0},
	     5,
	     "a virtual offset of (1, 0) is not emulated: the display leaves "
	     "the buffer"},
		{{0x00048009, 8, 8, 0, 1}, 5, "a virtual offset of (0, 1)"},
		{{0x00048003, 8, 8, FB_W + 1, FB_H}, 5, "a virtual offset of (0, 0)"},
		{{0x00048003, 8, 8, FB_W, FB_H + 1}, 5, "a virtual offset of (0, 0)"},
		{{0x00048004, 8, 8, FB_W + 1, FB_H, 0x00048009, 8, 8, 2, 0},
	     10,
	     "a virtual offset of (2, 0)"},
		{{0x00048004, 8, 8, FB_W, FB_H + 1, 0x00048009, 8, 8, 0, 2},
	     10,
	     "a virtual offset of (0, 2)"},
		{{0x0004800a, 16, 16, 240, 240, 0, 0},
	     7,
	     "an overscan of 240, 240, 0, 0 is not emulated: nothing is left of "
	     "the display"},
		{{0x0004800a, 16, 16, 0, 0, 360, 360}, 7, "an overscan of 0, 0, 360"},
		{{0x0004800a, 16, 16, 0xffffffffu, 2, 0, 0}, 7, "an overscan of 4294"},
		{{0x0004800a, 16, 16, 0, 0, 2, 0xffffffffu},
	     7,
	     "an overscan of 0, 0, 2"},
		{{0x00048008, 4, 4, FB_W * 2 - 1},
	     4,
	     "a pitch of 1311 bytes is not emulated"},
		{{0x00048008, 4, 4, 16385}, 4, "a pitch of 16385 bytes"},
		{{0x00040002, 4, 4, 2}, 4, "blank state 0x2: reserved bits are set"},
		{{0x00040014, 4, 4, 1},
	     4,
	     "tag 0x00040014: display 1 is not emulated: smirror has one "
	     "display, 0"},
		{{0x00048013, 4, 4, 1}, 4, "tag 0x00048013: display 1 is not"},
		{{0x00048003, 4, 4, 1},
	     4,
	     "tag 0x00048003: a value buffer too short for the request"},
		{{0x0004800a, 12, 12, 0, 0, 0}, 6, "tag 0x0004800a: a value buffer"},
		{{0x00044005, 0, 0}, 3, "tag 0x00044005: a value buffer"},
		{{0x00040002, 0, 0}, 3, "tag 0x00040002: a value buffer"},
		{{0x0004800b, 4, 4, 0}, 4, "tag 0x0004800b: a value buffer"},
		{{0x0004800b, 12, 12, 0, 2, 0}, 6, "tag 0x0004800b: a value buffer"},
		{{0x00040001, 0, 0}, 3, "tag 0x00040001: a value buffer"},
		{{0x00040014, 0, 0}, 3, "tag 0x00040014: a value buffer"},
		{{0x00048013, 0, 0}, 3, "tag 0x00048013: a value buffer"},
		{{0x00048003, 7, 7, FB_W, FB_H}, 5, "tag 0x00048003: a value buffer"},
		{{0x00040002, 3, 3, 0}, 4, "tag 0x00040002: a value buffer"},
		{{0x0004800b, 7, 7, 0, 0}, 5, "tag 0x0004800b: a value buffer"},
		{{0x00040001, 3, 3, 0}, 4, "tag 0x00040001: a value buffer"},
		{{0x00040014, 3, 3, 0}, 4, "tag 0x00040014: a value buffer"},
		{{0x00048013, 3, 3, 0}, 4, "tag 0x00048013: a value buffer"},
		{{0x00040013, 4, 0, 0, 0x00040005, 4, 0, 0, 0x00040005, 4, 0, 0},
	     12,
	     "tag 0x00040005 twice in a buffer"},
		{{0x00040005, 4, 0, 0, 0x00040005, 4, 0, 0},
	     8,
	     "tag 0x00040005 twice in a buffer is not emulated"},
		{{0x00044005, 4, 4, 8, 0x00040005, 4, 0, 0},
	     8,
	     "test tags with get or set tags in a buffer are not emulated"},
		{{0x00040013, 4, 0, 0, 0x00044005, 4, 4, 8}, 8, "test tags with get"},
		{{0x00040001, 8, 4, 0, 0, 0x00048001, 0, 0},
	     8,
	     "allocating and releasing the frame buffer at once is not emulated"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(stops); i++) {
		Setup s;

		setup(&s);
		tags_stop(&s, stops[i].words, stops[i].n, stops[i].message);
		/* Nothing changed. */
		CHECK_EQ(get1(&s, 0x00040005), 16);
		CHECK_EQ(get1(&s, 0x00040008), FB_W * 2);
		teardown(&s);
	}
}

/* Many tags, a buffer without an end tag. */
static void test_display_limits(void)
{
	uint32_t tags[4 * 33];
	uint32_t buf[] = {sizeof(uint32_t[7]), 0, 0x00048005, 4, 4, 32, 0x12345};
	Setup s;

	setup(&s);
	for (unsigned i = 0; i < 33; i++) {
		tags[4 * i] = 0x00040003 + (i % 8);
		tags[4 * i + 1] = 4;
		tags[4 * i + 2] = 0;
		tags[4 * i + 3] = 0;
	}
	tags_stop(&s, tags, ARRAY_SIZE(tags),
	          "more than 32 frame buffer tags in a buffer are not emulated");
	tags_stop(&s, tags, 4 * 32, "tag 0x00040003 twice in a buffer");

	send(&s, buf, ARRAY_SIZE(buf));
	CHECK_EQ(word(&s, BUF + 4), PARSE_ERROR);
	CHECK_EQ(word(&s, BUF + 16), 4);
	CHECK_EQ(get1(&s, 0x00040005), 16);
	teardown(&s);
}

/* The MAC address: the Raspberry Pi OUI, then the serial's low bytes. */
static void test_mac(void)
{
	uint8_t mac[6];

	raspi3b_property_mac(mac);
	CHECK_EQ(mac[0], 0xb8);
	CHECK_EQ(mac[1], 0x27);
	CHECK_EQ(mac[2], 0xeb);
	CHECK_EQ(mac[3], 0xff);
	CHECK_EQ(mac[4], 0xee);
	CHECK_EQ(mac[5], 0x03);
}

/*
 * The firmware's build, as Linux asks for it: its variant and Git hash;
 * whether the board was throttled; the virtual GPIOs' buffer.
 */
static void test_firmware_build(void)
{
	uint32_t buf[] = {
		sizeof(uint32_t[28]),
		0,
		0x00000002,
		4,
		0,
		0xffffffff, /* variant */
		0x00000003,
		20,
		0,
		0,
		0,
		0,
		0,
		0, /* hash */
		0x00030046,
		4,
		0,
		0xffff, /* throttled, as Linux asks */
		0x00048020,
		4,
		0,
		0x3a001000, /* the virtual GPIOs' buffer */
		0,          /* end */
		0,
		0,
		0,
		0,
		0,
	};
	uint32_t shortbuf[] = {sizeof(uint32_t[6]), 0, 0x00048020, 0, 0, 0};
	Setup s;

	setup(&s);
	send(&s, buf, ARRAY_SIZE(buf));
	CHECK_EQ(word(&s, BUF + 4), OK);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 20), 1); /* start */
	CHECK_EQ(word(&s, BUF + 32), RESPONSE(20));
	CHECK_EQ(word(&s, BUF + 36), 0x13c4219f);
	CHECK_EQ(word(&s, BUF + 40), 0x16dbe58c);
	CHECK_EQ(word(&s, BUF + 44), 0x5b994608);
	CHECK_EQ(word(&s, BUF + 48), 0x6b474baf);
	CHECK_EQ(word(&s, BUF + 52), 0x572512bb);
	CHECK_EQ(word(&s, BUF + 64), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 68), 0);
	CHECK_EQ(word(&s, BUF + 80), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 84), 0); /* success */
	send_stops(&s, shortbuf, ARRAY_SIZE(shortbuf),
	           "tag 0x00048020: a value buffer too short for the request is "
	           "not emulated");
	shortbuf[3] = 3; /* a word less one byte */
	send_stops(&s, shortbuf, ARRAY_SIZE(shortbuf),
	           "tag 0x00048020: a value buffer too short for the request is "
	           "not emulated");
	teardown(&s);
}

/*
 * One tag of the expander, with a value buffer of size bytes that starts
 * with args: what it answers in *out, out_n words. The firmware's hash
 * comes first, so that a word the tag does not answer is not 0.
 */
static void exp_tag(Setup *s, uint32_t tag, uint32_t size, const uint32_t *args,
                    size_t n, uint32_t *out, size_t out_n)
{
	uint32_t buf[24] = {0};

	buf[0] = sizeof(buf);
	buf[2] = 0x00000003; /* the hash, 20 bytes */
	buf[3] = 20;
	buf[10] = tag;
	buf[11] = size;
	memcpy(buf + 13, args, n * sizeof(*args));
	send(s, buf, ARRAY_SIZE(buf));
	CHECK_EQ(word(s, BUF + 4), OK);
	CHECK_EQ(word(s, BUF + 48), RESPONSE(out_n * 4));
	for (size_t i = 0; i < out_n && 4 * i < size; i++)
		out[i] = word(s, BUF + 52 + 4 * (uint32_t)i);
}

static void exp_config(Setup *s, uint32_t gpio, uint32_t out[5])
{
	exp_tag(s, 0x00030043, 20, &gpio, 1, out, 5);
}

static uint32_t exp_get(Setup *s, uint32_t gpio)
{
	uint32_t out[2];

	exp_tag(s, 0x00030041, 8, &gpio, 1, out, 2);
	CHECK_EQ(out[0], 0);
	return out[1];
}

static void exp_set_config(Setup *s, uint32_t gpio, uint32_t direction,
                           uint32_t state)
{
	uint32_t args[6] = {gpio, direction, 0, 0, 0, state}, out;

	exp_tag(s, 0x00038043, 24, args, 6, &out, 1);
	CHECK_EQ(out, 0);
}

static void exp_set(Setup *s, uint32_t gpio, uint32_t state)
{
	uint32_t args[2] = {gpio, state}, out;

	exp_tag(s, 0x00038041, 8, args, 2, &out, 1);
	CHECK_EQ(out, 0);
}

/*
 * The GPIO expander, as Linux's gpio-raspberrypi-exp.c drives it: how
 * its lines start, after a reset too; changing them; what is not
 * emulated.
 */
static void test_gpio_expander(void)
{
	static const uint32_t output[8] = {1, 1, 1, 1, 0, 1, 1, 0};
	static const uint32_t state[8] = {0, 1, 0, 1, 1, 0, 0, 1};
	static const struct {
		uint32_t words[12];
		const char *message;
	} stops[] = {
		{{sizeof(uint32_t[12]), 0, 0x00030043, 20, 0, 127},
	     "tag 0x00030043: GPIO 127 is not emulated: smirror has the "
	     "expander's, 128 to 135"},
		{{sizeof(uint32_t[12]), 0, 0x00030041, 8, 0, 136},
	     "tag 0x00030041: GPIO 136 is not emulated"},
		{{sizeof(uint32_t[12]), 0, 0x00030043, 3, 0, 128},
	     "tag 0x00030043: a value buffer too short"},
		{{sizeof(uint32_t[12]), 0, 0x00038043, 20, 0, 128, 1},
	     "tag 0x00038043: a value buffer too short"},
		{{sizeof(uint32_t[12]), 0, 0x00038041, 4, 0, 128},
	     "tag 0x00038041: a value buffer too short"},
		{{sizeof(uint32_t[12]), 0, 0x00038041, 7, 0, 128},
	     "tag 0x00038041: a value buffer too short"},
		{{sizeof(uint32_t[12]), 0, 0x00038043, 23, 0, 128, 1},
	     "tag 0x00038043: a value buffer too short"},
		{{sizeof(uint32_t[12]), 0, 0x00030041, 3, 0, 128},
	     "tag 0x00030041: a value buffer too short"},
		{{sizeof(uint32_t[12]), 0, 0x00038041, 8, 0, 132, 1},
	     "GPIO 132: setting an input is not emulated"},
		{{sizeof(uint32_t[12]), 0, 0x00038043, 24, 0, 128, 2},
	     "GPIO 128: direction 2, polarity 0 or termination 0 is not "
	     "emulated"},
		{{sizeof(uint32_t[12]), 0, 0x00038043, 24, 0, 128, 1, 1},
	     "GPIO 128: direction 1, polarity 1 or termination 0 is not "
	     "emulated"},
		{{sizeof(uint32_t[12]), 0, 0x00038043, 24, 0, 128, 0, 0, 1},
	     "GPIO 128: direction 0, polarity 0 or termination 1 is not "
	     "emulated"},
	};
	uint32_t config[5];
	Setup s;

	setup(&s);
	for (uint32_t i = 0; i < 8; i++) {
		exp_config(&s, 128 + i, config);
		CHECK_EQ(config[0], 0); /* success */
		CHECK_EQ(config[1], output[i]);
		CHECK_EQ(config[2], 0);
		CHECK_EQ(config[3], 0);
		CHECK_EQ(config[4], 0);
		CHECK_EQ(exp_get(&s, 128 + i), state[i]);
	}

	/* PWR_LOW_N driven low, then an input again: the power is good. */
	exp_set_config(&s, 135, 1, 0);
	exp_config(&s, 135, config);
	CHECK_EQ(config[1], 1);
	CHECK_EQ(exp_get(&s, 135), 0);
	exp_set(&s, 135, 7);
	CHECK_EQ(exp_get(&s, 135), 1);
	exp_set_config(&s, 135, 0, 0);
	CHECK_EQ(exp_get(&s, 135), 1);
	/* BT_ON on; CAM_GPIO1 an output already, with state 5 (high). */
	exp_set(&s, 128, 1);
	CHECK_EQ(exp_get(&s, 128), 1);
	exp_set(&s, 128, 0);
	CHECK_EQ(exp_get(&s, 128), 0);
	exp_set_config(&s, 134, 1, 5);
	CHECK_EQ(exp_get(&s, 134), 1);
	/* An output turned input reads the line outside: low. */
	exp_set_config(&s, 134, 0, 1);
	CHECK_EQ(exp_get(&s, 134), 0);
	exp_config(&s, 134, config);
	CHECK_EQ(config[1], 0);

	/* Each output, turned into an input, reads the line outside: low. */
	for (uint32_t i = 0; i < 8; i++) {
		if (output[i]) {
			exp_set_config(&s, 128 + i, 0, 0);
			CHECK_EQ(exp_get(&s, 128 + i), 0);
		}
	}
	/* A value buffer of the GPIO alone is enough to ask. */
	{
		uint32_t gpio = 128, out[5] = {1, 1, 1, 1, 1};

		exp_tag(&s, 0x00030043, 4, &gpio, 1, out, 5);
		CHECK_EQ(out[0], 0);
		out[0] = 1;
		exp_tag(&s, 0x00030041, 4, &gpio, 1, out, 2);
		CHECK_EQ(out[0], 0);
	}

	/* A reset of the board: as they started. */
	raspi3b_property_reset(s.p);
	for (uint32_t i = 0; i < 8; i++) {
		exp_config(&s, 128 + i, config);
		CHECK_EQ(config[1], output[i]);
		CHECK_EQ(exp_get(&s, 128 + i), state[i]);
	}
	teardown(&s);

	for (size_t i = 0; i < ARRAY_SIZE(stops); i++) {
		setup(&s);
		send_stops(&s, stops[i].words, ARRAY_SIZE(stops[i].words),
		           stops[i].message);
		teardown(&s);
	}
}

/*
 * Every tag of the wiki and of Linux's raspberrypi-firmware.h. Those that
 * smirror does not answer stop the machine; tags next to them that
 * neither names are unknown, and ignored.
 */
static void test_known_tags(void)
{
	static const uint32_t known[] = {
		0x00000001, 0x00000002, 0x00000003, 0x00008010, 0x00008011, 0x00008012,
		0x00010001, 0x00010002, 0x00010003, 0x00010004, 0x00010005, 0x00010006,
		0x00010007, 0x00020001, 0x00020002, 0x00028001, 0x00030001, 0x00030002,
		0x00030003, 0x00030004, 0x00030005, 0x00030006, 0x00030007, 0x00030008,
		0x00030009, 0x0003000a, 0x0003000b, 0x0003000c, 0x0003000d, 0x0003000e,
		0x0003000f, 0x00030010, 0x00030011, 0x00030012, 0x00030014, 0x00030020,
		0x00030021, 0x00030023, 0x00030030, 0x00030041, 0x00030043, 0x00030045,
		0x00030046, 0x00030047, 0x00030048, 0x00030049, 0x00030050, 0x00030058,
		0x00030064, 0x00030066, 0x00030087, 0x0003008a, 0x00034041, 0x00038001,
		0x00038002, 0x00038003, 0x00038009, 0x00038021, 0x00038030, 0x00038041,
		0x00038042, 0x00038043, 0x00038045, 0x00038049, 0x00038064, 0x00038087,
		0x0003808a, 0x00040001, 0x00040002, 0x00040003, 0x00040004, 0x00040005,
		0x00040006, 0x00040007, 0x00040008, 0x00040009, 0x0004000a, 0x0004000b,
		0x0004000c, 0x0004000d, 0x0004000e, 0x0004000f, 0x00040010, 0x00040013,
		0x00040014, 0x00040016, 0x00040017, 0x00040018, 0x00044003, 0x00044004,
		0x00044005, 0x00044006, 0x00044007, 0x00044009, 0x0004400a, 0x0004400b,
		0x0004400c, 0x0004400d, 0x0004400e, 0x00048001, 0x00048003, 0x00048004,
		0x00048005, 0x00048006, 0x00048007, 0x00048008, 0x00048009, 0x0004800a,
		0x0004800b, 0x0004800c, 0x0004800d, 0x0004800e, 0x0004800f, 0x00048010,
		0x00048013, 0x00048015, 0x00048017, 0x00048019, 0x0004801f, 0x00048020,
		0x00050001, 0x00060001,
	};
	static const uint32_t answered[] = {
		0x00000001,
		0x00000002,
		0x00000003,
		0x00030046,
		0x00048020,
		0x00030041,
		0x00038041,
		0x00030043,
		0x00038043,
		0x00010002,
		0x00010003,
		0x00010004,
		0x00010005,
		0x00010006,
		0x00020001,
		0x00020002,
		0x00028001,
		0x00030002,
		0x00030004,
		0x00030007,
		0x00030009,
		0x00038002,
		0x00038009,
		0x00038042,
		0x00050001,
		/* The frame buffer's. */
		0x00040001,
		0x00040002,
		0x00040003,
		0x00040004,
		0x00040005,
		0x00040006,
		0x00040007,
		0x00040008,
		0x00040009,
		0x0004000a,
		0x00040013,
		0x00040014,
		0x00044003,
		0x00044004,
		0x00044005,
		0x00044006,
		0x00044007,
		0x00044009,
		0x0004400a,
		0x00048001,
		0x00048003,
		0x00048004,
		0x00048005,
		0x00048006,
		0x00048007,
		0x00048008,
		0x00048009,
		0x0004800a,
		0x0004800b,
		0x00048013,
	};
	Setup s;

	setup(&s);
	for (size_t i = 0; i < ARRAY_SIZE(known); i++) {
		bool is_answered = false;

		for (size_t j = 0; j < ARRAY_SIZE(answered); j++)
			if (answered[j] == known[i])
				is_answered = true;
		if (!is_answered) {
			uint32_t buf[] = {sizeof(uint32_t[6]), 0, known[i], 0, 0, 0};
			char message[64];

			snprintf(message, sizeof(message), "tag 0x%08x is not emulated",
			         (unsigned)known[i]);
			send_stops(&s, buf, ARRAY_SIZE(buf), message);
		}
		for (int d = -1; d <= 1; d += 2) {
			uint32_t tag = known[i] + (uint32_t)d;
			bool is_known = false;

			for (size_t j = 0; j < ARRAY_SIZE(known); j++)
				if (known[j] == tag)
					is_known = true;
			if (is_known || tag == 0)
				continue;
			uint32_t buf[] = {sizeof(uint32_t[6]), 0, tag, 0, 0, 0};

			send(&s, buf, ARRAY_SIZE(buf));
			CHECK_EQ(word(&s, BUF + 4), OK);
			CHECK_EQ(word(&s, BUF + 16), 0);
		}
	}
	teardown(&s);
}

/*
 * Each tag needs a value buffer as long as its request: one byte less
 * stops the machine.
 */
static void test_short_values(void)
{
	static const struct {
		uint32_t tag, len, args[3], answer;
	} tags[] = {
		{0x00020001, 4, {9}, 8},
		{0x00020002, 4, {9}, 8},
		{0x00028001, 8, {9, 0}, 8},
		{0x00030002, 4, {3}, 8},
		{0x00030004, 4, {3}, 8},
		{0x00030007, 4, {3}, 8},
		{0x00038002, 12, {3, 1200 * MHZ}, 8},
		{0x00030009, 4, {0}, 8},
		{0x00038009, 8, {0, 0}, 8},
		{0x00038042, 4, {400000}, 12},
	};
	Setup s;

	setup(&s);
	CHECK_EQ(bus_map_ram(s.m->bus, "sdhost", 0x3f202000, 0x100), BUS_OK);
	for (size_t i = 0; i < ARRAY_SIZE(tags); i++) {
		uint32_t buf[10] = {sizeof(buf), 0, tags[i].tag, tags[i].len, 0};
		char message[80];

		memcpy(&buf[5], tags[i].args, tags[i].len);
		send(&s, buf, ARRAY_SIZE(buf));
		CHECK_EQ(word(&s, BUF + 4), OK);
		CHECK_EQ(word(&s, BUF + 16), RESPONSE(tags[i].answer));

		buf[3] = tags[i].len - 1;
		snprintf(message, sizeof(message),
		         "tag 0x%08x: a value buffer too short for the request",
		         (unsigned)tags[i].tag);
		send_stops(&s, buf, ARRAY_SIZE(buf), message);
		CHECK_EQ(word(&s, BUF + 16), 0); /* nothing written back */
	}
	teardown(&s);
}

/* Tags at the very end of the buffer, and a value padded from 5 bytes. */
static void test_format_edges(void)
{
	Setup s;
	/* A header that ends where the buffer does: answered, no end tag. */
	uint32_t header_at_end[] = {20, 0, 0x00010002, 0, 0, 0x99};
	/* A value one byte longer than what is left. */
	uint32_t value_past[] = {24, 0, 0x00010002, 5, 0, 0, 0x99};
	/* A header cut one byte short of its end. */
	uint32_t header_cut[] = {19, 0, 0x00010002, 0, 0x5a5a5a5a};
	/* A tag whose first word is cut by the end. */
	uint32_t tag_cut[] = {23, 0, 0x00012345, 0, 0, 0x00010002};
	/* A value of 5 bytes is padded to 8. */
	uint32_t padded[] = {sizeof(uint32_t[13]), 0, 0x00010002, 5, 0, 0, 0,
	                     0x00010002,           4, 0,          0, 0, 0};

	setup(&s);
	send(&s, header_at_end, ARRAY_SIZE(header_at_end));
	CHECK_EQ(word(&s, BUF + 4), PARSE_ERROR);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 20), 0x99); /* past the size: untouched */

	send(&s, value_past, ARRAY_SIZE(value_past));
	CHECK_EQ(word(&s, BUF + 4), PARSE_ERROR);
	CHECK_EQ(word(&s, BUF + 16), 0);
	CHECK_EQ(word(&s, BUF + 20), 0);

	send(&s, header_cut, ARRAY_SIZE(header_cut));
	CHECK_EQ(word(&s, BUF + 4), PARSE_ERROR);
	CHECK_EQ(word(&s, BUF + 16), 0x5a5a5a5a);

	send(&s, tag_cut, ARRAY_SIZE(tag_cut));
	CHECK_EQ(word(&s, BUF + 4), PARSE_ERROR);
	CHECK_EQ(word(&s, BUF + 20), 0x00010002);

	send(&s, padded, ARRAY_SIZE(padded));
	CHECK_EQ(word(&s, BUF + 4), OK);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 20), RASPI3B_BOARD_REVISION);
	CHECK_EQ(word(&s, BUF + 36), RESPONSE(4));
	CHECK_EQ(word(&s, BUF + 40), RASPI3B_BOARD_REVISION);
	teardown(&s);
}

/*
 * The limits of a buffer: at most 1 MiB, at least 12 bytes, inside the
 * Arm's memory, with the request code 0.
 */
static void test_buffer_limits(void)
{
	const uint32_t big_ram = 4u << 20;
	Machine *m = machine_create();
	Raspi3bProperty *p;
	uint32_t reply = 0;
	uint32_t size_11[] = {11, 0, 0};
	uint32_t response[] = {12, 0x80000000u, 0};
	uint32_t to_the_end[] = {16, 0, 0, 0};
	Setup s;

	/* With 4 MiB of memory, only BUFFER_MAX limits the size. */
	CHECK(m != NULL);
	CHECK_EQ(bus_map_ram(m->bus, "ram", 0, big_ram), BUS_OK);
	p = raspi3b_property_create(m, big_ram);
	CHECK(p != NULL);
	CHECK_EQ(bus_write(m->bus, BUF, 4, 1u << 20), BUS_OK);
	CHECK(raspi3b_property_receive(p, BUF | 8, &reply));
	CHECK_EQ(reply, BUF | 8);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	CHECK_EQ(bus_write(m->bus, BUF, 4, (1u << 20) + 4), BUS_OK);
	CHECK(!raspi3b_property_receive(p, BUF | 8, &reply));
	CHECK(strstr(m->stop_message,
	             "a property buffer of 0x00100004 bytes is not emulated"));
	raspi3b_property_destroy(p);
	machine_destroy(m);

	setup(&s);
	send_stops(&s, size_11, ARRAY_SIZE(size_11),
	           "a property buffer of 11 bytes is not emulated");
	/* A buffer sent back as it came, its response code still in it. */
	send_stops(&s, response, ARRAY_SIZE(response),
	           "a buffer request code of 0x80000000 is not documented");
	/* A buffer that ends where the memory does. */
	put(&s, RAM_SIZE - 16, to_the_end, ARRAY_SIZE(to_the_end));
	CHECK(raspi3b_property_receive(s.p, (RAM_SIZE - 16) | 8, &reply));
	CHECK_EQ(word(&s, RAM_SIZE - 12), OK);
	teardown(&s);

	/*
	 * The Arm's memory ends 4 bytes after the buffer, then 3 bytes after
	 * it; the bus goes on.
	 */
	setup(&s);
	put(&s, BUF, (const uint32_t[]){12, 0, 0}, 3);
	for (uint32_t end = 4; end >= 3; end--) {
		p = raspi3b_property_create(s.m, BUF + end);
		CHECK(p != NULL);
		CHECK(!raspi3b_property_receive(p, BUF | 8, &reply));
		CHECK(strstr(s.m->stop_message,
		             end == 4 ? "a property buffer of 0x0000000c bytes is not "
		                        "emulated"
		                      : "a property buffer at 0x00001000 is not in "
		                        "memory"));
		s.m->stop = MACHINE_STOP_NONE;
		raspi3b_property_destroy(p);
	}
	teardown(&s);
}

/* A command line of 4096 bytes is answered; a longer one is not. */
static void test_cmdline_limit(void)
{
	static char line[4098];
	Setup s;
	uint32_t head[] = {12 + 4096 + 4 + 8, 0, 0x00050001, 4096, 0};

	memset(line, 'x', 4097);
	line[4096] = '\0';
	setup(&s);
	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){.total_mem_mb = RASPI3B_TOTAL_MEM_MB,
	                              .gpu_mem_mb = RASPI3B_GPU_MEM_MB,
	                              .cmdline = line}));
	put(&s, BUF + 20 + 4096, (const uint32_t[]){0}, 1);
	send(&s, head, ARRAY_SIZE(head));
	CHECK_EQ(word(&s, BUF + 4), OK);
	CHECK_EQ(word(&s, BUF + 16), RESPONSE(4096));
	CHECK_EQ(word(&s, BUF + 20 + 4092), 0x78787878);

	line[4096] = 'x';
	CHECK(raspi3b_property_configure(
		s.p, &(Raspi3bVcSettings){.total_mem_mb = RASPI3B_TOTAL_MEM_MB,
	                              .gpu_mem_mb = RASPI3B_GPU_MEM_MB,
	                              .cmdline = line}));
	send_stops(&s, head, ARRAY_SIZE(head),
	           "a command line of 4097 bytes is not emulated");
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_identity);
	RUN_TEST(test_firmware_build);
	RUN_TEST(test_gpio_expander);
	RUN_TEST(test_memory);
	RUN_TEST(test_cmdline);
	RUN_TEST(test_power);
	RUN_TEST(test_clock_rates);
	RUN_TEST(test_set_clock_rate);
	RUN_TEST(test_turbo);
	RUN_TEST(test_format);
	RUN_TEST(test_stops);
	RUN_TEST(test_mac);
	RUN_TEST(test_display_boot);
	RUN_TEST(test_display_uboot);
	RUN_TEST(test_display_linux);
	RUN_TEST(test_display_blank_palette);
	RUN_TEST(test_display_operation);
	RUN_TEST(test_display_allocate);
	RUN_TEST(test_display_tests);
	RUN_TEST(test_display_pitch);
	RUN_TEST(test_display_offset);
	RUN_TEST(test_display_alignment);
	RUN_TEST(test_display_stops);
	RUN_TEST(test_display_limits);
	RUN_TEST(test_sdhost_clock);
	RUN_TEST(test_known_tags);
	RUN_TEST(test_short_values);
	RUN_TEST(test_format_edges);
	RUN_TEST(test_buffer_limits);
	RUN_TEST(test_cmdline_limit);
	return test_summary();
}
